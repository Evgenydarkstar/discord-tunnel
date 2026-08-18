from __future__ import annotations

import ipaddress
import struct
from dataclasses import dataclass

DEFAULT_ALLOWED_HOSTS = (
    "discord.com",
    "discord.gg",
    "discord.media",
    "discordapp.com",
    "discordapp.net",
    "discord-attachments-uploads-prd.storage.googleapis.com",
)

MAX_HOST_LEN = 255
MAX_WIRE_DATAGRAM = 1150
MAX_DATAGRAM_PAYLOAD = 65507
MAX_FRAGMENT_COUNT = 64
MAX_FRAGMENT_ASSEMBLIES = 128
FRAGMENT_MAGIC = b"DRF1"
FRAGMENT_HEADER_SIZE = 12


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class DatagramPacket:
    flow_id: int
    host: str
    port: int
    payload: bytes


def encode_datagram(packet: DatagramPacket) -> bytes:
    host = packet.host.encode("idna")
    if not (0 <= packet.flow_id <= 0xFFFFFFFF):
        raise ProtocolError("flow id out of range")
    if not host or len(host) > MAX_HOST_LEN:
        raise ProtocolError("host is empty or too long")
    if not (1 <= packet.port <= 65535):
        raise ProtocolError("port out of range")
    encoded = b"".join(
        [
            struct.pack(">IBH", packet.flow_id, len(host), packet.port),
            host,
            packet.payload,
        ]
    )
    if len(encoded) > MAX_WIRE_DATAGRAM:
        raise ProtocolError("datagram too large for tunnel")
    return encoded


def decode_datagram(data: bytes) -> DatagramPacket:
    if len(data) > MAX_WIRE_DATAGRAM:
        raise ProtocolError("datagram too large for tunnel")
    if len(data) < 7:
        raise ProtocolError("datagram too short")
    flow_id, host_len, port = struct.unpack(">IBH", data[:7])
    end = 7 + host_len
    if host_len == 0 or len(data) < end:
        raise ProtocolError("invalid datagram host")
    try:
        host = data[7:end].decode("idna")
    except UnicodeError as exc:
        raise ProtocolError("invalid datagram host encoding") from exc
    if not (1 <= port <= 65535):
        raise ProtocolError("port out of range")
    return DatagramPacket(flow_id, host, port, data[end:])


def fragment_datagram(packet: DatagramPacket, *, message_id: int) -> list[DatagramPacket]:
    if len(packet.payload) > MAX_DATAGRAM_PAYLOAD:
        raise ProtocolError("datagram payload too large")
    host_len = len(packet.host.encode("idna"))
    plain_size = 7 + host_len + len(packet.payload)
    if plain_size <= MAX_WIRE_DATAGRAM:
        return [packet]
    chunk_size = MAX_WIRE_DATAGRAM - 7 - host_len - FRAGMENT_HEADER_SIZE
    if chunk_size <= 0:
        raise ProtocolError("host leaves no room for datagram payload")
    count = (len(packet.payload) + chunk_size - 1) // chunk_size
    if not 1 < count <= MAX_FRAGMENT_COUNT:
        raise ProtocolError("too many datagram fragments")
    fragments = []
    for index in range(count):
        start = index * chunk_size
        payload = b"".join(
            [
                FRAGMENT_MAGIC,
                struct.pack(">IHH", message_id & 0xFFFFFFFF, index, count),
                packet.payload[start : start + chunk_size],
            ]
        )
        fragments.append(
            DatagramPacket(packet.flow_id, packet.host, packet.port, payload)
        )
    return fragments


def reassemble_datagram(
    packet: DatagramPacket,
    state: dict[tuple[int, str, int, int], list[bytes | None]],
) -> DatagramPacket | None:
    payload = packet.payload
    if not payload.startswith(FRAGMENT_MAGIC):
        return packet
    if len(payload) < FRAGMENT_HEADER_SIZE:
        raise ProtocolError("truncated datagram fragment")
    message_id, index, count = struct.unpack(">IHH", payload[4:FRAGMENT_HEADER_SIZE])
    if not 1 < count <= MAX_FRAGMENT_COUNT or index >= count:
        raise ProtocolError("invalid datagram fragment")
    key = (packet.flow_id, packet.host, packet.port, message_id)
    if key not in state:
        if len(state) >= MAX_FRAGMENT_ASSEMBLIES:
            state.pop(next(iter(state)))
        state[key] = [None] * count
    parts = state[key]
    if len(parts) != count:
        del state[key]
        raise ProtocolError("conflicting datagram fragments")
    parts[index] = payload[FRAGMENT_HEADER_SIZE:]
    if any(part is None for part in parts):
        return None
    del state[key]
    combined = b"".join(part for part in parts if part is not None)
    if len(combined) > MAX_DATAGRAM_PAYLOAD:
        raise ProtocolError("reassembled datagram payload too large")
    return DatagramPacket(packet.flow_id, packet.host, packet.port, combined)


def is_allowed_destination(host: str, allowed_hosts: tuple[str, ...]) -> bool:
    host = host.strip().rstrip(".").lower()
    if not host:
        return False

    try:
        return ipaddress.ip_address(host).is_global
    except ValueError:
        pass

    for allowed in allowed_hosts:
        allowed = allowed.strip().rstrip(".").lower()
        if not allowed:
            continue
        if host == allowed or host.endswith("." + allowed):
            return True
    return False
