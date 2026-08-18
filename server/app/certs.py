from __future__ import annotations

import ipaddress
import shutil
import ssl
import subprocess
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Callable

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from app.config import Settings

CONTROL_API_HOST_ALIAS = "host.docker.internal"

API_CERT_RENEW_BEFORE = timedelta(days=30)
API_CA_RENEW_BEFORE = timedelta(days=365)
DEFAULT_CERT_VALIDITY_DAYS = 3650
DEFAULT_FALLBACK_RENEW_BEFORE = timedelta(days=30)
ProgressCallback = Callable[[str, str], None]


class CertificateManager:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def certificate_dir_for_hostname(self, hostname: str) -> Path:
        return self.settings.certs_dir / hostname.lower()

    def certificate_paths_for_hostname(self, hostname: str) -> tuple[Path, Path]:
        cert_dir = self.certificate_dir_for_hostname(hostname)
        return cert_dir / "server-cert.pem", cert_dir / "server-key.pem"

    def default_certificate_paths(self) -> tuple[Path, Path]:
        cert_dir = self.settings.certs_dir / "_default"
        return cert_dir / "server-cert.pem", cert_dir / "server-key.pem"

    def default_ca_paths(self) -> tuple[Path, Path]:
        cert_dir = self.settings.certs_dir / "_default"
        return cert_dir / "ca-cert.pem", cert_dir / "ca-key.pem"

    def api_ca_paths(self) -> tuple[Path, Path]:
        return self.settings.api_ca_cert_path, self.settings.api_ca_key_path

    def _certificate_expires_within(self, cert_path: Path, delta: timedelta) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
        except Exception:
            return True
        return certificate.not_valid_after_utc <= datetime.now(UTC) + delta

    def _api_ca_has_required_extensions(self, ca_cert_path: Path) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(ca_cert_path.read_bytes())
            basic_constraints = certificate.extensions.get_extension_for_class(
                x509.BasicConstraints
            ).value
            key_usage = certificate.extensions.get_extension_for_class(
                x509.KeyUsage
            ).value
            certificate.extensions.get_extension_for_class(x509.SubjectKeyIdentifier)
        except Exception:
            return False
        return (
            basic_constraints.ca is True
            and key_usage.key_cert_sign is True
            and key_usage.crl_sign is True
        )

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

    def ensure_api_ca(self) -> tuple[Path, Path]:
        ca_cert_path, ca_key_path = self.api_ca_paths()
        if (
            ca_cert_path.exists()
            and ca_key_path.exists()
            and self._api_ca_has_required_extensions(ca_cert_path)
            and not self._certificate_expires_within(ca_cert_path, API_CA_RENEW_BEFORE)
        ):
            return ca_cert_path, ca_key_path

        ca_cert_path.parent.mkdir(parents=True, exist_ok=True)
        ca_private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        ca_subject = x509.Name(
            [x509.NameAttribute(NameOID.COMMON_NAME, "Discord Tunnel Control API CA")]
        )
        ca_certificate = (
            x509.CertificateBuilder()
            .subject_name(ca_subject)
            .issuer_name(ca_subject)
            .public_key(ca_private_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.now(UTC))
            .not_valid_after(datetime.now(UTC) + timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .add_extension(
                x509.KeyUsage(
                    digital_signature=True,
                    content_commitment=False,
                    key_encipherment=False,
                    data_encipherment=False,
                    key_agreement=False,
                    key_cert_sign=True,
                    crl_sign=True,
                    encipher_only=False,
                    decipher_only=False,
                ),
                critical=True,
            )
            .add_extension(
                x509.SubjectKeyIdentifier.from_public_key(
                    ca_private_key.public_key()
                ),
                critical=False,
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
        ca_key_path.chmod(0o600)
        return ca_cert_path, ca_key_path

    def issue_api_server_certificate(
        self, *, cert_path: Path, key_path: Path, san_entries: list[str]
    ) -> tuple[Path, Path]:
        ca_cert_path, ca_key_path = self.ensure_api_ca()
        ca_certificate = x509.load_pem_x509_certificate(ca_cert_path.read_bytes())
        ca_private_key = serialization.load_pem_private_key(
            ca_key_path.read_bytes(), password=None
        )

        key_path.parent.mkdir(parents=True, exist_ok=True)
        private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        common_name = san_entries[0] if san_entries else "localhost"
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
        san_values: list[x509.GeneralName] = []
        for item in san_entries:
            try:
                san_values.append(x509.IPAddress(ipaddress.ip_address(item)))
            except ValueError:
                san_values.append(x509.DNSName(item))
        certificate = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(ca_certificate.subject)
            .public_key(private_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.now(UTC) - timedelta(minutes=5))
            .not_valid_after(datetime.now(UTC) + timedelta(days=825))
            .add_extension(x509.SubjectAlternativeName(san_values), critical=False)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(
                x509.KeyUsage(
                    digital_signature=True,
                    content_commitment=False,
                    key_encipherment=True,
                    data_encipherment=False,
                    key_agreement=False,
                    key_cert_sign=False,
                    crl_sign=False,
                    encipher_only=False,
                    decipher_only=False,
                ),
                critical=True,
            )
            .add_extension(
                x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
                critical=False,
            )
            .add_extension(
                x509.SubjectKeyIdentifier.from_public_key(private_key.public_key()),
                critical=False,
            )
            .add_extension(
                x509.AuthorityKeyIdentifier.from_issuer_public_key(
                    ca_private_key.public_key()
                ),
                critical=False,
            )
            .sign(ca_private_key, hashes.SHA256())
        )
        key_path.write_bytes(
            private_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
        cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        key_path.chmod(0o600)
        return cert_path, key_path

    def _api_certificate_covers(self, cert_path: Path, required_entries: list[str]) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
            san = certificate.extensions.get_extension_for_class(
                x509.SubjectAlternativeName
            ).value
        except Exception:
            return False

        for entry in required_entries:
            try:
                ip_value = ipaddress.ip_address(entry)
            except ValueError:
                if entry not in san.get_values_for_type(x509.DNSName):
                    return False
            else:
                if ip_value not in san.get_values_for_type(x509.IPAddress):
                    return False
        return True

    def _api_certificate_has_required_extensions(self, cert_path: Path) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
            certificate.extensions.get_extension_for_class(
                x509.AuthorityKeyIdentifier
            )
            certificate.extensions.get_extension_for_class(
                x509.SubjectKeyIdentifier
            )
            certificate.extensions.get_extension_for_class(x509.ExtendedKeyUsage)
        except Exception:
            return False
        return True

    def _api_certificate_is_signed_by_current_ca(self, cert_path: Path) -> bool:
        try:
            certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
            ca_cert_path, _ = self.api_ca_paths()
            ca_certificate = x509.load_pem_x509_certificate(ca_cert_path.read_bytes())
            authority_key_id = certificate.extensions.get_extension_for_class(
                x509.AuthorityKeyIdentifier
            ).value.key_identifier
            subject_key_id = ca_certificate.extensions.get_extension_for_class(
                x509.SubjectKeyIdentifier
            ).value.digest
        except Exception:
            return False
        return certificate.issuer == ca_certificate.subject and authority_key_id == subject_key_id

    def api_certificate_needs_rotation(
        self, cert_path: Path, required_entries: list[str]
    ) -> bool:
        return not (
            cert_path.exists()
            and self._api_certificate_covers(cert_path, required_entries)
            and self._api_certificate_has_required_extensions(cert_path)
            and self._api_certificate_is_signed_by_current_ca(cert_path)
            and not self._certificate_expires_within(cert_path, API_CERT_RENEW_BEFORE)
        )

    def ensure_control_api_tls_material(self) -> tuple[Path, Path]:
        cert_path = Path(self.settings.worker_tls_cert_path or self.settings.control_api_cert_path)
        key_path = Path(self.settings.worker_tls_key_path or self.settings.control_api_key_path)
        san_entries = ["127.0.0.1", "localhost", CONTROL_API_HOST_ALIAS]
        if self.settings.public_ip:
            san_entries.append(self.settings.public_ip)
        self.ensure_api_ca()
        if key_path.exists() and not self.api_certificate_needs_rotation(
            cert_path, san_entries
        ):
            return cert_path, key_path
        return self.issue_api_server_certificate(
            cert_path=cert_path,
            key_path=key_path,
            san_entries=san_entries,
        )

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

    def has_installed_certificate(self, hostname: str) -> bool:
        cert_path, key_path = self.certificate_paths_for_hostname(hostname)
        return cert_path.exists() and key_path.exists()

    def installed_certificate_covers(self, hostnames: list[str]) -> bool:
        if len(hostnames) != 1:
            return False

        hostname = hostnames[0].lower()
        cert_path, _ = self.certificate_paths_for_hostname(hostname)
        return self._certificate_path_covers_hostname(cert_path, hostname)

    def _certificate_path_covers_hostname(self, cert_path: Path, hostname: str) -> bool:
        if not cert_path.exists():
            return False

        try:
            cert_info = ssl._ssl._test_decode_cert(str(cert_path))
        except Exception:
            return False

        not_after = cert_info.get("notAfter")
        if not not_after:
            return False
        try:
            expires_at = datetime.strptime(not_after, "%b %d %H:%M:%S %Y %Z").replace(
                tzinfo=UTC
            )
        except ValueError:
            return False
        if expires_at <= datetime.now(UTC):
            return False

        san = cert_info.get("subjectAltName", [])
        dns_names = {value.lower() for kind, value in san if kind == "DNS"}
        if hostname in dns_names:
            return True
        try:
            expected_ip = ipaddress.ip_address(hostname)
        except ValueError:
            return False
        ip_values = {
            ipaddress.ip_address(value)
            for kind, value in san
            if kind == "IP Address"
        }
        return expected_ip in ip_values

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

    def generate_client_certificate(self, fqdn: str, username: str) -> tuple[str, str]:
        fqdn = fqdn.lower()
        client_dir = self.settings.certs_dir / fqdn / "clients" / username
        client_dir.mkdir(parents=True, exist_ok=True)
        cert_path = client_dir / "client-cert.pem"
        key_path = client_dir / "client-key.pem"
        ca_cert_path, ca_key_path = self.certificate_paths_for_hostname(fqdn)
        if not ca_cert_path.exists() or not ca_key_path.exists():
            raise RuntimeError(f"CA certificate not found for {fqdn}")
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-nodes",
                "-newkey",
                "rsa:2048",
                "-days",
                "3650",
                "-keyout",
                str(key_path),
                "-out",
                str(cert_path),
                "-subj",
                f"/CN={username}",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        key_path.chmod(0o600)
        return str(cert_path), str(key_path)

    def revoke_client_certificate(self, fqdn: str, username: str) -> str:
        fqdn = fqdn.lower()
        client_cert_path = (
            self.settings.certs_dir / fqdn / "clients" / username / "client-cert.pem"
        )
        crl_path = self.settings.certs_dir / "crl.pem"
        ca_cert_path, ca_key_path = self.certificate_paths_for_hostname(fqdn)
        if not client_cert_path.exists():
            raise RuntimeError(f"Client certificate not found for {username}@{fqdn}")
        if not ca_cert_path.exists() or not ca_key_path.exists():
            raise RuntimeError(f"CA certificate not found for {fqdn}")
        revoked_list_path = self.settings.certs_dir / "revoked.pem"
        with open(client_cert_path, "rb") as src, open(revoked_list_path, "ab") as dst:
            dst.write(src.read())
            dst.write(b"\n")
        tmpl_path = self.settings.certs_dir / "crl.tmpl"
        if not tmpl_path.exists():
            tmpl_path.write_text(
                "crl_next_update = 365\ncrl_number = 1\n", encoding="utf-8"
            )
        subprocess.run(
            [
                "openssl",
                "ca",
                "-gencrl",
                "-keyfile",
                str(ca_key_path),
                "-cert",
                str(ca_cert_path),
                "-revoke",
                str(client_cert_path),
                "-out",
                str(crl_path),
                "-config",
                "/etc/ssl/openssl.cnf",
            ],
            capture_output=True,
            text=True,
        )
        return str(crl_path)

