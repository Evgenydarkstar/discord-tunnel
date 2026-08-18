from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENV_PATH = PROJECT_ROOT / ".env"


def _load_dotenv_file(path: Path) -> None:
    if not path.exists():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        os.environ.setdefault(key, value)


def _default_app_dir() -> Path:
    raw = os.environ.get("APP_DIR", "").strip()
    if raw:
        return Path(raw)
    if os.name == "nt":
        return PROJECT_ROOT
    container_app_dir = Path("/app")
    if container_app_dir.exists():
        return container_app_dir
    return PROJECT_ROOT


def _default_data_dir(app_dir: Path) -> Path:
    raw = os.environ.get("DATA_DIR", "").strip()
    if raw:
        return Path(raw)
    if os.name == "nt":
        return PROJECT_ROOT / "data"
    container_data_dir = Path("/data")
    if container_data_dir.exists() or app_dir == Path("/app"):
        return container_data_dir
    return PROJECT_ROOT / "data"


@dataclass(frozen=True)
class Settings:
    public_ip: str
    vpn_port: int
    vpn_type: str
    worker_id: str
    worker_token: str
    worker_api_port: int
    worker_public_url: str
    worker_tls_cert_path: str
    worker_tls_key_path: str
    central_api_url: str
    central_api_ca_cert_path: str
    central_mode: str
    app_version: str
    data_dir: Path = Path("/data")
    app_dir: Path = Path("/app")

    @property
    def db_path(self) -> Path:
        return self.data_dir / "db" / "control.db"

    @property
    def certs_dir(self) -> Path:
        return self.data_dir / "certs"

    @property
    def api_ca_cert_path(self) -> Path:
        return self.data_dir / "control-api" / "ca.crt"

    @property
    def api_ca_key_path(self) -> Path:
        return self.data_dir / "control-api" / "ca.key"

    @property
    def control_api_cert_path(self) -> Path:
        return self.data_dir / "control-api" / "tls.crt"

    @property
    def control_api_key_path(self) -> Path:
        return self.data_dir / "control-api" / "tls.key"


def load_settings() -> Settings:
    _load_dotenv_file(DEFAULT_ENV_PATH)
    app_dir = _default_app_dir()
    data_dir = _default_data_dir(app_dir)
    public_ip = os.environ["PUBLIC_IP"].strip()
    if not public_ip:
        raise ValueError("PUBLIC_IP is required")

    worker_api_port = int(os.environ.get("WORKER_API_PORT", "8080"))
    worker_public_url = os.environ.get("WORKER_PUBLIC_URL", "").strip()
    if not worker_public_url:
        worker_public_url = f"http://127.0.0.1:{worker_api_port}"

    return Settings(
        public_ip=public_ip,
        vpn_port=int(os.environ.get("VPN_PORT", "443")),
        vpn_type=os.environ.get("VPN_TYPE", "discord").strip().lower(),
        worker_id=os.environ.get("WORKER_ID", "").strip(),
        worker_token=os.environ.get("WORKER_TOKEN", "").strip(),
        worker_api_port=worker_api_port,
        worker_public_url=worker_public_url,
        worker_tls_cert_path=os.environ.get("WORKER_TLS_CERT_PATH", "").strip(),
        worker_tls_key_path=os.environ.get("WORKER_TLS_KEY_PATH", "").strip(),
        central_api_url=os.environ.get("CENTRAL_API_URL", "").strip(),
        central_api_ca_cert_path=os.environ.get("CENTRAL_API_CA_CERT_PATH", "").strip(),
        central_mode=os.environ.get("CENTRAL_MODE", "single").strip().lower(),
        app_version=os.environ.get("APP_VERSION", "dev").strip(),
        data_dir=data_dir,
        app_dir=app_dir,
    )