from __future__ import annotations

import asyncio
import hmac
import ipaddress
import logging
import os
import socket
import time
from dataclasses import dataclass, field
from pathlib import Path

from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, H3Event, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import DatagramFrameReceived, ProtocolNegotiated, QuicEvent

from app.certs import CertificateManager
from app.config import load_settings
from app.discord_protocol import (
    DEFAULT_ALLOWED_HOSTS,
    DatagramPacket,
    ProtocolError,
    decode_datagram,
    encode_datagram,
    fragment_datagram,
    is_allowed_destination,
    reassemble_datagram,
)

logger = logging.getLogger(__name__)
BUFFER_SIZE = 64 * 1024
MAX_PENDING_TCP_BYTES = 1024 * 1024
MAX_UDP_FLOWS = 128
HEARTBEAT_INTERVAL_SECONDS = 5


@dataclass
class TcpTunnel:
    pending: bytearray = field(default_factory=bytearray)
    writer: asyncio.StreamWriter | None = None
    task: asyncio.Task[None] | None = None
    input_ended: bool = False
    upstream_eof: bool = False


class UdpRelay(asyncio.DatagramProtocol):
    def __init__(self, owner: "DiscordHttp3Protocol", flow_id: int) -> None:
        self.owner = owner
        self.flow_id = flow_id
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            packet = DatagramPacket(self.flow_id, addr[0], addr[1], data)
            message_id = self.owner._next_udp_message_id()
            for fragment in fragment_datagram(packet, message_id=message_id):
                self.owner._quic.send_datagram_frame(encode_datagram(fragment))
            self.owner.transmit()
        except (ProtocolError, ValueError):
            logger.debug("dropping oversized UDP response for flow %d", self.flow_id)

    def close(self) -> None:
        if self.transport is not None:
            self.transport.close()


class DiscordHttp3Protocol(QuicConnectionProtocol):
    def __init__(self, *args, token: str, allowed_hosts: tuple[str, ...], **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._token = token
        self._allowed_hosts = allowed_hosts
        self._http: H3Connection | None = None
        self._authenticated = False
        self._tcp: dict[int, TcpTunnel] = {}
        self._udp: dict[int, UdpRelay] = {}
        self._udp_fragments: dict[
            tuple[int, str, int, int], list[bytes | None]
        ] = {}
        self._udp_message_id = 0

    def _next_udp_message_id(self) -> int:
        self._udp_message_id = (self._udp_message_id + 1) & 0xFFFFFFFF
        return self._udp_message_id

    def quic_event_received(self, event: QuicEvent) -> None:
        if isinstance(event, ProtocolNegotiated):
            self._http = H3Connection(self._quic)
        if isinstance(event, DatagramFrameReceived):
            if self._authenticated:
                asyncio.create_task(self._handle_udp(event.data))
            return
        if self._http is not None:
            for http_event in self._http.handle_event(event):
                self._handle_http_event(http_event)

    def connection_lost(self, exc: Exception | None) -> None:
        for tunnel in self._tcp.values():
            if tunnel.writer is not None:
                tunnel.writer.close()
            if tunnel.task is not None:
                tunnel.task.cancel()
        for relay in self._udp.values():
            relay.close()
        super().connection_lost(exc)

    def _handle_http_event(self, event: H3Event) -> None:
        if isinstance(event, HeadersReceived):
            headers = dict(event.headers)
            if not self._authorize(headers):
                self._respond(event.stream_id, 401, end_stream=True)
                return
            self._authenticated = True
            method = headers.get(b":method", b"")
            path = headers.get(b":path", b"")
            if method == b"GET" and path == b"/v1/session":
                self._respond(event.stream_id, 200, end_stream=True)
                return
            if method != b"CONNECT" or path != b"/v1/tcp":
                self._respond(event.stream_id, 404, end_stream=True)
                return
            try:
                host = headers[b"x-target-host"].decode("idna")
                port = int(headers[b"x-target-port"])
            except (KeyError, UnicodeError, ValueError):
                self._respond(event.stream_id, 400, end_stream=True)
                return
            if not 1 <= port <= 65535 or not is_allowed_destination(
                host, self._allowed_hosts
            ):
                self._respond(event.stream_id, 403, end_stream=True)
                return
            tunnel = TcpTunnel()
            self._tcp[event.stream_id] = tunnel
            tunnel.task = asyncio.create_task(
                self._open_tcp(event.stream_id, host, port, tunnel)
            )
        elif isinstance(event, DataReceived):
            tunnel = self._tcp.get(event.stream_id)
            if tunnel is None:
                return
            if event.data:
                if tunnel.writer is None:
                    if len(tunnel.pending) + len(event.data) > MAX_PENDING_TCP_BYTES:
                        self._close_tcp(event.stream_id)
                        return
                    tunnel.pending.extend(event.data)
                else:
                    tunnel.writer.write(event.data)
                    asyncio.create_task(self._drain_tcp_writer(tunnel.writer))
            if event.stream_ended:
                tunnel.input_ended = True
                if tunnel.writer is not None and tunnel.writer.can_write_eof():
                    tunnel.writer.write_eof()
                if tunnel.upstream_eof and self._http is not None:
                    self._close_tcp(event.stream_id)

    def _authorize(self, headers: dict[bytes, bytes]) -> bool:
        supplied = headers.get(b"authorization", b"")
        return hmac.compare_digest(supplied, f"Bearer {self._token}".encode())

    def _respond(self, stream_id: int, status: int, *, end_stream: bool) -> None:
        if self._http is None:
            return
        self._http.send_headers(
            stream_id,
            [(b":status", str(status).encode()), (b"server", b"manyserver-h3")],
            end_stream=end_stream,
        )
        self.transmit()

    async def _drain_tcp_writer(self, writer: asyncio.StreamWriter) -> None:
        try:
            await writer.drain()
        except (ConnectionError, OSError):
            pass

    async def _open_tcp(
        self, stream_id: int, host: str, port: int, tunnel: TcpTunnel
    ) -> None:
        connected = False
        upstream_eof = False
        try:
            address = await _resolve_global(host, port, socket.SOCK_STREAM)
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(address, port), timeout=10
            )
            connected = True
            tunnel.writer = writer
            if tunnel.pending:
                writer.write(tunnel.pending)
                tunnel.pending.clear()
                await writer.drain()
            self._respond(stream_id, 200, end_stream=False)
            while data := await reader.read(BUFFER_SIZE):
                if self._http is None:
                    break
                self._http.send_data(stream_id, data, end_stream=False)
                self.transmit()
            if self._http is not None:
                upstream_eof = True
                tunnel.upstream_eof = True
                self._http.send_data(stream_id, b"", end_stream=True)
                self.transmit()
                if tunnel.input_ended:
                    self._close_tcp(stream_id)
        except (OSError, asyncio.TimeoutError):
            if not connected:
                self._respond(stream_id, 502, end_stream=True)
            elif self._http is not None:
                self._http.send_data(stream_id, b"", end_stream=True)
                self.transmit()
        finally:
            if not connected or not upstream_eof or tunnel.input_ended:
                self._close_tcp(stream_id)

    def _close_tcp(self, stream_id: int) -> None:
        tunnel = self._tcp.pop(stream_id, None)
        if tunnel is None:
            return
        if tunnel.writer is not None:
            tunnel.writer.close()
        if tunnel.task is not None and tunnel.task is not asyncio.current_task():
            tunnel.task.cancel()

    async def _handle_udp(self, data: bytes) -> None:
        try:
            packet = decode_datagram(data)
            packet = reassemble_datagram(packet, self._udp_fragments)
        except ProtocolError:
            return
        if packet is None:
            return
        if not is_allowed_destination(packet.host, self._allowed_hosts):
            return
        try:
            address = await _resolve_global(packet.host, packet.port, socket.SOCK_DGRAM)
        except (OSError, asyncio.TimeoutError):
            return
        relay = self._udp.get(packet.flow_id)
        if relay is None:
            if len(self._udp) >= MAX_UDP_FLOWS:
                return
            relay = UdpRelay(self, packet.flow_id)
            await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: relay, local_addr=("0.0.0.0", 0)
            )
            self._udp[packet.flow_id] = relay
        if relay.transport is not None:
            relay.transport.sendto(packet.payload, (address, packet.port))


async def _resolve_global(host: str, port: int, sock_type: int) -> str:
    try:
        address = ipaddress.ip_address(host)
        if not address.is_global:
            raise OSError("destination address is not global")
        return str(address)
    except ValueError:
        pass

    results = await asyncio.wait_for(
        asyncio.get_running_loop().getaddrinfo(
            host, port, family=socket.AF_INET, type=sock_type
        ),
        timeout=5,
    )
    for _, _, _, _, sockaddr in results:
        address = ipaddress.ip_address(sockaddr[0])
        if address.is_global:
            return str(address)
    raise OSError("destination hostname did not resolve to a global IPv4 address")


def _required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise RuntimeError(f"{name} is required")
    return value


def _port_from_env() -> int:
    for name in ("DISCORD_HTTP3_PORT", "VPN_PORT"):
        raw = os.environ.get(name, "").strip()
        if not raw:
            continue
        port = int(raw)
        if 1 <= port <= 65535:
            return port
        raise RuntimeError(f"{name} must be between 1 and 65535")
    return 8443


def _tls_paths_from_env_or_default() -> tuple[str, str]:
    cert_path = os.environ.get("DISCORD_HTTP3_CERT_PATH", "").strip()
    key_path = os.environ.get("DISCORD_HTTP3_KEY_PATH", "").strip()
    if cert_path or key_path:
        if not cert_path or not key_path:
            raise RuntimeError(
                "DISCORD_HTTP3_CERT_PATH and DISCORD_HTTP3_KEY_PATH must be set together"
            )
        if Path(cert_path).exists() and Path(key_path).exists():
            return cert_path, key_path
        logging.getLogger(__name__).warning(
            "Configured TLS files missing (%s, %s); generating self-signed certificate",
            cert_path,
            key_path,
        )
    settings = load_settings()
    cert_file, key_file = CertificateManager(settings).ensure_self_signed_runtime()
    return str(cert_file), str(key_file)


def _heartbeat_path() -> Path:
    settings = load_settings()
    return settings.data_dir / "run" / "discord-http3" / "heartbeat"


async def _heartbeat_loop(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    while True:
        path.write_text(str(int(time.time())), encoding="utf-8")
        await asyncio.sleep(HEARTBEAT_INTERVAL_SECONDS)


async def main() -> None:
    logging.basicConfig(level=os.environ.get("LOG_LEVEL", "WARNING"))
    host = os.environ.get("DISCORD_HTTP3_HOST", "0.0.0.0")
    port = _port_from_env()
    token = _required_env("DISCORD_HTTP3_TOKEN")
    cert_path, key_path = _tls_paths_from_env_or_default()
    allowed_hosts = tuple(
        item.strip().rstrip(".").lower()
        for item in os.environ.get(
            "DISCORD_HTTP3_ALLOWED_HOSTS", ",".join(DEFAULT_ALLOWED_HOSTS)
        ).split(",")
        if item.strip()
    )
    if not Path(cert_path).is_file() or not Path(key_path).is_file():
        raise RuntimeError(
            f"Discord HTTP/3 TLS files are missing: cert={cert_path} key={key_path}"
        )
    configuration = QuicConfiguration(
        is_client=False,
        alpn_protocols=H3_ALPN,
        max_datagram_frame_size=65536,
        idle_timeout=120,
    )
    configuration.load_cert_chain(cert_path, key_path)
    asyncio.create_task(_heartbeat_loop(_heartbeat_path()))
    await serve(
        host,
        port,
        configuration=configuration,
        create_protocol=lambda *args, **kwargs: DiscordHttp3Protocol(
            *args, token=token, allowed_hosts=allowed_hosts, **kwargs
        ),
    )
    logger.info("Discord HTTP/3 tunnel listening on %s:%d/udp", host, port)

    await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
