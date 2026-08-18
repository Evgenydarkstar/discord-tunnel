from __future__ import annotations

import asyncio
import os
import secrets
import string
from dataclasses import dataclass
from typing import Callable

from app.certs import CertificateManager
from app.config import Settings
from app.db import Database


def generate_secret(length: int = 20) -> str:
    alphabet = string.ascii_letters + string.digits
    return "".join(secrets.choice(alphabet) for _ in range(length))


@dataclass(frozen=True)
class VpnConfigResult:
    config_id: int
    protocol_type: str
    fqdn: str
    port: int
    username: str
    password: str
    connection_uri: str
    certificate_path: str
    private_key_path: str
    client_cert_path: str = ""
    client_key_path: str = ""


ProgressCallback = Callable[[str, str], None]


class ControlService:
    def __init__(self, settings: Settings, db: Database) -> None:
        self.settings = settings
        self.db = db
        self.certs = CertificateManager(settings)

    @property
    def runtime_type(self) -> str:
        return self.settings.vpn_type or "discord"

    @property
    def server_host(self) -> str:
        return self.settings.public_ip

    async def create_vpn_config(
        self,
        username: str | None = None,
        password: str | None = None,
        auth_type: str = "password",
        protocol_type: str | None = None,
        progress_callback: ProgressCallback | None = None,
    ) -> VpnConfigResult:
        target_protocol = (protocol_type or self.runtime_type).strip().lower()
        if target_protocol != "discord":
            raise RuntimeError(f"Unsupported VPN protocol: {target_protocol}")
        return await self._create_discord_config(
            progress_callback=progress_callback,
        )

    async def _create_discord_config(
        self,
        *,
        progress_callback: ProgressCallback | None,
    ) -> VpnConfigResult:
        host = self.server_host
        existing_hosts = {
            str(row["fqdn"]).lower()
            for row in self.db.list_vpn_configs()
            if str(row["protocol_type"]) == "discord"
        }
        if existing_hosts and host.lower() not in existing_hosts:
            raise RuntimeError(
                "Discord tunnel server can use only one TLS host. "
                f"Current host: {sorted(existing_hosts)[0]}"
            )
        if progress_callback is not None:
            progress_callback("config-save", "Saving Discord tunnel config parameters...")
        token = os.environ.get("DISCORD_HTTP3_TOKEN", "").strip()
        if not token:
            raise RuntimeError("DISCORD_HTTP3_TOKEN is not configured on the worker")
        username = f"discord-{generate_secret(10).lower()}"
        connection_uri = f"discord+h3://{host}:{self.settings.vpn_port}?token={token}"
        config_id = self.db.add_vpn_config(
            host,
            "",
            host,
            username,
            token,
            server_id=None,
            protocol_type="discord",
            connection_uri=connection_uri,
            display_host=host,
            display_port=self.settings.vpn_port,
            auth_type="token",
        )
        try:
            await asyncio.to_thread(self.sync_runtime, True, progress_callback)
        except Exception:
            self.db.remove_vpn_config(config_id)
            raise
        if progress_callback is not None:
            progress_callback("done", "Discord tunnel config is ready.")
        return VpnConfigResult(
            config_id=config_id,
            protocol_type="discord",
            fqdn=host,
            port=self.settings.vpn_port,
            username=username,
            password=token,
            connection_uri=connection_uri,
            certificate_path="",
            private_key_path="",
        )

    def list_vpn_configs(self):
        return self.db.list_vpn_configs()

    def list_subdomains(self):
        return self.db.list_subdomains()

    async def delete_vpn_config(self, config_id: int) -> bool:
        row = self.db.get_vpn_config(config_id)
        if row is None:
            return False
        removed = self.db.remove_vpn_config(config_id)
        if not removed:
            return False
        try:
            await asyncio.to_thread(self.sync_runtime, True)
        except Exception:
            self.db.add_vpn_config(
                row["fqdn"],
                row["root_domain"],
                row["label"],
                row["username"],
                row["password"],
                server_id=row["server_id"],
                protocol_type=row["protocol_type"],
                connection_uri=row["connection_uri"],
                display_host=row["display_host"],
                display_port=row["display_port"],
                certificate_path=row["certificate_path"],
                private_key_path=row["private_key_path"],
                client_cert_path=row["client_cert_path"],
                client_key_path=row["client_key_path"],
                auth_type=row["auth_type"],
                config_id=int(row["id"]),
            )
            raise
        return True

    def sync_runtime(
        self,
        strict_certificates: bool = False,
        progress_callback: ProgressCallback | None = None,
    ) -> None:
        if self.runtime_type != "discord":
            raise RuntimeError(f"Unsupported runtime type: {self.runtime_type}")
        if progress_callback is not None:
            progress_callback("render-runtime", "Updating Discord tunnel certificate...")
        self.certs.ensure_self_signed_runtime()
        if progress_callback is not None:
            progress_callback("restart-runtime", "Restarting Discord tunnel runtime...")
        (self.settings.data_dir / "run" / "discord-http3" / "restart.flag").write_text(
            "restart\n", encoding="utf-8"
        )

    async def background_renewal_loop(self) -> None:
        renewal_interval = 12 * 60 * 60
        retry_interval = 30 * 60
        last_renewal = 0.0
        while True:
            try:
                now = asyncio.get_running_loop().time()
                if now - last_renewal >= renewal_interval:
                    try:
                        await self.rotate_control_api_tls_once()
                    except Exception:
                        pass
                    last_renewal = now
                configs = self.db.list_vpn_configs()
                hostnames = {str(row["fqdn"]).lower() for row in configs}
                missing = [
                    h for h in hostnames
                    if not self.certs.installed_certificate_covers([h])
                ]
                if missing:
                    await asyncio.to_thread(self.sync_runtime, False)
                await asyncio.sleep(retry_interval)
            except Exception:
                await asyncio.sleep(retry_interval)

    async def rotate_control_api_tls_once(self) -> None:
        self.certs.ensure_control_api_tls_material()
        ca_cert_path, _ = self.certs.api_ca_paths()
        ca_cert_pem = ca_cert_path.read_text(encoding="utf-8")
        for row in self.db.list_servers(enabled_only=True):
            await self.rotate_worker_control_tls(row, ca_cert_pem=ca_cert_pem)

    async def rotate_worker_control_tls(
        self, row, *, ca_cert_pem: str | None = None, force: bool = False
    ) -> None:
        worker_id = str(row["worker_id"])
        if not worker_id:
            return
        if ca_cert_pem is None:
            self.certs.ensure_control_api_tls_material()
            ca_cert_path, _ = self.certs.api_ca_paths()
            ca_cert_pem = ca_cert_path.read_text(encoding="utf-8")
        cert_path = (
            self.settings.data_dir / "control-api" / "workers" / f"{worker_id}.crt"
        )
        key_path = (
            self.settings.data_dir / "control-api" / "workers" / f"{worker_id}.key"
        )
        required_entries = [
            str(row["host"]),
            str(row["public_ip"]),
            "127.0.0.1",
            "localhost",
            "worker-api",
        ]
        if (
            not force
            and key_path.exists()
            and not self.certs.api_certificate_needs_rotation(cert_path, required_entries)
        ):
            return
        self.certs.issue_api_server_certificate(
            cert_path=cert_path,
            key_path=key_path,
            san_entries=required_entries,
        )
        verify: bool | str = int(row["verify_tls"]) == 1
        if verify:
            verify = str(self.settings.api_ca_cert_path)
        from app.worker_client import WorkerClient

        client = WorkerClient(
            api_url=str(row["api_url"]),
            worker_id=worker_id,
            worker_token=str(row["worker_token"]),
            verify_tls=verify,
        )
        try:
            await client.repair_control_tls(
                tls_cert_pem=cert_path.read_text(encoding="utf-8"),
                tls_key_pem=key_path.read_text(encoding="utf-8"),
                ca_cert_pem=ca_cert_pem,
            )
        except Exception:
            return


