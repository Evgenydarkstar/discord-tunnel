from __future__ import annotations

from app.certs import CertificateManager
from app.config import load_settings


def main() -> None:
    settings = load_settings()
    certs = CertificateManager(settings)
    if settings.central_mode != "worker":
        certs.ensure_control_api_tls_material()
    certs.ensure_self_signed()
    if settings.vpn_type != "discord":
        raise RuntimeError(f"Unsupported VPN type: {settings.vpn_type}")
    certs.ensure_self_signed_runtime()


if __name__ == "__main__":
    main()
