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
    data_dir: Path = Path("/data")

    @property
    def certs_dir(self) -> Path:
        return self.data_dir / "certs"


def load_settings() -> Settings:
    _load_dotenv_file(DEFAULT_ENV_PATH)
    app_dir = _default_app_dir()
    data_dir = _default_data_dir(app_dir)
    public_ip = os.environ["PUBLIC_IP"].strip()
    if not public_ip:
        raise ValueError("PUBLIC_IP is required")

    return Settings(
        public_ip=public_ip,
        data_dir=data_dir,
    )
