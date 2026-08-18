from __future__ import annotations

import ipaddress
import shutil
from datetime import UTC, datetime, timedelta
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from app.config import Settings

DEFAULT_CERT_VALIDITY_DAYS = 3650
DEFAULT_FALLBACK_RENEW_BEFORE = timedelta(days=30)


class CertificateManager:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def default_certificate_paths(self) -> tuple[Path, Path]:
        cert_dir = self.settings.certs_dir / "_default"
        return cert_dir / "server-cert.pem", cert_dir / "server-key.pem"

    def default_ca_paths(self) -> tuple[Path, Path]:
        cert_dir = self.settings.certs_dir / "_default"
        return cert_dir / "ca-cert.pem", cert_dir / "ca-key.pem"

    def _certificate_expires_within(self, cert_path: Path, delta: timedelta) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
        except Exception:
            return True
        return certificate.not_valid_after_utc <= datetime.now(UTC) + delta

    def _default_ca_has_required_extensions(self, ca_cert_path: Path) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(ca_cert_path.read_bytes())
            basic_constraints = certificate.extensions.get_extension_for_class(
                x509.BasicConstraints
            ).value
            key_usage = certificate.extensions.get_extension_for_class(
                x509.KeyUsage
            ).value
        except Exception:
            return False
        return (
            basic_constraints.ca is True
            and key_usage.key_cert_sign is True
            and key_usage.crl_sign is True
        )

    def _default_server_certificate_matches_ca(
        self, cert_path: Path, ca_cert_path: Path
    ) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
            ca_certificate = x509.load_pem_x509_certificate(ca_cert_path.read_bytes())
            subject_cn = certificate.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[
                0
            ].value
            issuer_cn = certificate.issuer.get_attributes_for_oid(NameOID.COMMON_NAME)[
                0
            ].value
            san = certificate.extensions.get_extension_for_class(
                x509.SubjectAlternativeName
            ).value
            basic_constraints = certificate.extensions.get_extension_for_class(
                x509.BasicConstraints
            ).value
            key_usage = certificate.extensions.get_extension_for_class(
                x509.KeyUsage
            ).value
            ca_subject_cn = ca_certificate.subject.get_attributes_for_oid(
                NameOID.COMMON_NAME
            )[0].value
        except Exception:
            return False
        return (
            subject_cn == "temporary.invalid"
            and issuer_cn == ca_subject_cn == "temporary.invalid CA"
            and basic_constraints.ca is False
            and key_usage.digital_signature is True
            and key_usage.key_encipherment is True
            and "temporary.invalid" in san.get_values_for_type(x509.DNSName)
            and self._ip_san_covers(san, self.settings.public_ip)
        )

    def _ip_san_covers(
        self, san: x509.SubjectAlternativeName, host: str | None
    ) -> bool:
        if not host:
            return True
        try:
            expected = ipaddress.ip_address(host)
        except ValueError:
            return True
        return any(value == expected for value in san.get_values_for_type(x509.IPAddress))

    def ensure_self_signed(self) -> tuple[Path, Path]:
        cert_path, key_path = self.default_certificate_paths()
        ca_cert_path, ca_key_path = self.default_ca_paths()
        if (
            cert_path.exists()
            and key_path.exists()
            and ca_cert_path.exists()
            and ca_key_path.exists()
            and self._default_ca_has_required_extensions(ca_cert_path)
            and self._default_server_certificate_matches_ca(cert_path, ca_cert_path)
            and not self._certificate_expires_within(
                cert_path, DEFAULT_FALLBACK_RENEW_BEFORE
            )
            and not self._certificate_expires_within(
                ca_cert_path, DEFAULT_FALLBACK_RENEW_BEFORE
            )
        ):
            return cert_path, key_path

        cert_path.parent.mkdir(parents=True, exist_ok=True)
        ca_private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        ca_subject = x509.Name(
            [x509.NameAttribute(NameOID.COMMON_NAME, "temporary.invalid CA")]
        )
        ca_certificate = (
            x509.CertificateBuilder()
            .subject_name(ca_subject)
            .issuer_name(ca_subject)
            .public_key(ca_private_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.now(UTC))
            .not_valid_after(datetime.now(UTC) + timedelta(days=DEFAULT_CERT_VALIDITY_DAYS))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .add_extension(
                x509.KeyUsage(
                    digital_signature=False,
                    key_encipherment=False,
                    content_commitment=False,
                    data_encipherment=False,
                    key_agreement=False,
                    key_cert_sign=True,
                    crl_sign=True,
                    encipher_only=False,
                    decipher_only=False,
                ),
                critical=True,
            )
            .sign(ca_private_key, hashes.SHA256())
        )

        private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        subject = x509.Name(
            [x509.NameAttribute(NameOID.COMMON_NAME, "temporary.invalid")]
        )
        san_values = [x509.DNSName("temporary.invalid")]
        if self.settings.public_ip:
            try:
                san_values.append(
                    x509.IPAddress(ipaddress.ip_address(self.settings.public_ip))
                )
            except ValueError:
                pass
        certificate = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(ca_subject)
            .public_key(private_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.now(UTC))
            .not_valid_after(datetime.now(UTC) + timedelta(days=DEFAULT_CERT_VALIDITY_DAYS))
            .add_extension(
                x509.SubjectAlternativeName(san_values),
                critical=False,
            )
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(
                x509.KeyUsage(
                    digital_signature=True,
                    key_encipherment=True,
                    content_commitment=False,
                    data_encipherment=False,
                    key_agreement=False,
                    key_cert_sign=False,
                    crl_sign=False,
                    encipher_only=False,
                    decipher_only=False,
                ),
                critical=True,
            )
            .sign(ca_private_key, hashes.SHA256())
        )
        ca_key_path.write_bytes(
            ca_private_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
        ca_cert_path.write_bytes(
            ca_certificate.public_bytes(serialization.Encoding.PEM)
        )
        key_path.write_bytes(
            private_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
        cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        ca_key_path.chmod(0o600)
        key_path.chmod(0o600)
        return cert_path, key_path

    def ensure_self_signed_runtime(self) -> tuple[Path, Path]:
        cert_path, key_path = self.ensure_self_signed()
        run_dir = self.settings.data_dir / "run" / "discord-http3"
        run_dir.mkdir(parents=True, exist_ok=True)
        target_cert = run_dir / "tls.crt"
        target_key = run_dir / "tls.key"
        shutil.copy2(cert_path, target_cert)
        shutil.copy2(key_path, target_key)
        target_key.chmod(0o600)
        return target_cert, target_key

