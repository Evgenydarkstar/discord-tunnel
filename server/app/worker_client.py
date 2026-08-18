from __future__ import annotations

import asyncio
import logging

import httpx

from app.schemas import ConfigResponse, OperationStatusResponse


logger = logging.getLogger(__name__)
CREATE_CONFIG_TIMEOUT_SECONDS = 900.0


def is_tls_verification_error(exc: Exception) -> bool:
    current: BaseException | None = exc
    while current is not None:
        if isinstance(current, httpx.ConnectError):
            text = str(current).lower()
            if "certificate verify failed" in text or "ssl:" in text:
                return True
        current = current.__cause__
    return False


class WorkerClient:
    def __init__(
        self,
        api_url: str,
        worker_id: str,
        worker_token: str,
        verify_tls: bool | str = True,
    ) -> None:
        self.api_url = api_url.rstrip("/")
        self.worker_id = worker_id
        self.worker_token = worker_token
        self.verify_tls = verify_tls

    async def _request(
        self,
        method: str,
        path: str,
        *,
        json_payload: dict | None = None,
        timeout: float = 20.0,
        retries: int = 3,
    ):
        last_exc: Exception | None = None
        for attempt in range(retries):
            try:
                async with httpx.AsyncClient(
                    timeout=timeout, verify=self.verify_tls
                ) as client:
                    response = await client.request(
                        method, f"{self.api_url}{path}", json=json_payload
                    )
                response.raise_for_status()
                return response
            except httpx.HTTPStatusError as exc:
                error_detail = str(exc)
                try:
                    body = exc.response.json()
                    if isinstance(body, dict) and body.get("detail"):
                        error_detail = str(body["detail"])
                except Exception:
                    pass
                logger.error(
                    "worker http error: method=%s path=%s attempt=%s status=%s detail=%s",
                    method,
                    path,
                    attempt + 1,
                    exc.response.status_code,
                    error_detail,
                )
                if exc.response.status_code < 500:
                    raise RuntimeError(error_detail)
                last_exc = RuntimeError(error_detail)
                if attempt == retries - 1:
                    raise last_exc
                await asyncio.sleep(1 + attempt)
            except (
                httpx.TimeoutException,
                httpx.NetworkError,
                httpx.RemoteProtocolError,
            ) as exc:
                last_exc = exc
                error_message = str(exc) or exc.__class__.__name__
                logger.warning(
                    "worker request failed: method=%s path=%s attempt=%s error=%s",
                    method,
                    path,
                    attempt + 1,
                    error_message,
                )
                if attempt == retries - 1:
                    if isinstance(exc, httpx.TimeoutException):
                        raise RuntimeError(
                            "Worker did not finish the request in time. "
                            "Certificate issuance may still be running; try again in a few minutes."
                        ) from exc
                    raise
                await asyncio.sleep(1 + attempt)
        if last_exc:
            raise last_exc

    async def status(self) -> dict:
        response = await self._request("GET", "/api/v1/status", timeout=20.0)
        return response.json()

    async def create_config(
        self,
        *,
        operation_id: str | None = None,
        auth_type: str = "password",
        username: str | None = None,
        password: str | None = None,
    ) -> ConfigResponse:
        payload = {
            "worker_id": self.worker_id,
            "worker_token": self.worker_token,
            "operation_id": operation_id,
            "auth_type": auth_type,
            "username": username,
            "password": password,
        }
        response = await self._request(
            "POST",
            "/api/v1/configs/create",
            json_payload=payload,
            timeout=CREATE_CONFIG_TIMEOUT_SECONDS,
            retries=1,
        )
        return ConfigResponse.model_validate(response.json())


    async def operation_status(self, *, operation_id: str) -> OperationStatusResponse:
        payload = {
            "worker_id": self.worker_id,
            "worker_token": self.worker_token,
            "operation_id": operation_id,
        }
        response = await self._request(
            "POST",
            "/api/v1/operations/status",
            json_payload=payload,
            timeout=20.0,
            retries=1,
        )
        return OperationStatusResponse.model_validate(response.json())

    async def delete_config(self, *, config_id: int, fqdn: str, username: str) -> None:
        payload = {
            "worker_id": self.worker_id,
            "worker_token": self.worker_token,
            "config_id": config_id,
            "fqdn": fqdn,
            "username": username,
        }
        await self._request(
            "POST",
            "/api/v1/configs/delete",
            json_payload=payload,
            timeout=300.0,
            retries=1,
        )

    async def runtime_sync(self, *, strict_certificates: bool = False) -> dict:
        payload = {
            "worker_id": self.worker_id,
            "worker_token": self.worker_token,
            "strict_certificates": strict_certificates,
        }
        response = await self._request(
            "POST", "/api/v1/runtime/sync", json_payload=payload, timeout=300.0
        )
        return response.json()

    async def subdomains_sync(self, *, subdomains: list[dict]) -> dict:
        payload = {
            "worker_id": self.worker_id,
            "worker_token": self.worker_token,
            "subdomains": subdomains,
        }
        response = await self._request(
            "POST", "/api/v1/subdomains/sync", json_payload=payload, timeout=300.0
        )
        return response.json()


    async def rotate_control_tls(
        self, *, tls_cert_pem: str, tls_key_pem: str, ca_cert_pem: str
    ) -> dict:
        payload = {
            "worker_id": self.worker_id,
            "worker_token": self.worker_token,
            "tls_cert_pem": tls_cert_pem,
            "tls_key_pem": tls_key_pem,
            "ca_cert_pem": ca_cert_pem,
        }
        response = await self._request(
            "POST",
            "/api/v1/control-tls/rotate",
            json_payload=payload,
            timeout=60.0,
            retries=1,
        )
        return response.json()

    async def repair_control_tls(
        self, *, tls_cert_pem: str, tls_key_pem: str, ca_cert_pem: str
    ) -> dict:
        try:
            return await self.rotate_control_tls(
                tls_cert_pem=tls_cert_pem,
                tls_key_pem=tls_key_pem,
                ca_cert_pem=ca_cert_pem,
            )
        except Exception as exc:
            if not is_tls_verification_error(exc):
                raise
        repair_client = WorkerClient(
            api_url=self.api_url,
            worker_id=self.worker_id,
            worker_token=self.worker_token,
            verify_tls=False,
        )
        return await repair_client.rotate_control_tls(
            tls_cert_pem=tls_cert_pem,
            tls_key_pem=tls_key_pem,
            ca_cert_pem=ca_cert_pem,
        )

