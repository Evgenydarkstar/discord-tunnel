from __future__ import annotations

import asyncio
import json
import logging
import os
import shutil
import time

import httpx
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from app.config import load_settings
from app.db import Database
from app.schemas import (
    ConfigResponse,
    CreateConfigRequest,
    DeleteConfigRequest,
    HeartbeatRequest,
    OperationStatusRequest,
    OperationStatusResponse,
    RegisterWorkerRequest,
RuntimeSyncRequest,
    RotateControlTlsRequest,
    WorkerStatusResponse,
)
from app.service import ControlService


logging.basicConfig(
    level=getattr(logging, os.environ.get("LOG_LEVEL", "WARNING").upper(), logging.WARNING),
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger(__name__)

settings = load_settings()
db = Database(settings.db_path)
service = ControlService(settings, db)
app = FastAPI(title="Discord Tunnel Control API")
OPERATION_PROGRESS: dict[str, dict[str, str]] = {}


@app.exception_handler(Exception)
async def global_exception_handler(request: Request, exc: Exception) -> JSONResponse:
    logger.exception(
        "unhandled exception: method=%s path=%s error=%s",
        request.method,
        request.url.path,
        exc,
    )
    return JSONResponse(status_code=500, content={"detail": str(exc)})


@app.exception_handler(StarletteHTTPException)
async def http_exception_handler(
    request: Request, exc: StarletteHTTPException
) -> JSONResponse:
    return JSONResponse(status_code=exc.status_code, content={"detail": exc.detail})


def _validate_local_worker(worker_id: str, worker_token: str) -> None:
    if worker_id != settings.worker_id or worker_token != settings.worker_token:
        raise HTTPException(status_code=401, detail="invalid worker credentials")














def _discord_runtime_status_from_heartbeat() -> str | None:
    heartbeat_path = settings.data_dir / "run" / "discord-http3" / "heartbeat"
    try:
        heartbeat_mtime = heartbeat_path.stat().st_mtime
    except OSError:
        return "stopped"
    if time.time() - heartbeat_mtime <= 15:
        return "running"
    return "stopped"






def _runtime_status() -> tuple[str, str]:
    if settings.vpn_type == "discord":
        heartbeat_status = _discord_runtime_status_from_heartbeat()
        if heartbeat_status is not None:
            return settings.vpn_type, heartbeat_status
    return settings.vpn_type, "unknown"


def _memory_used_percent() -> float:
    try:
        meminfo: dict[str, int] = {}
        with open("/proc/meminfo", "r", encoding="utf-8") as handle:
            for line in handle:
                key, raw_value = line.split(":", 1)
                meminfo[key] = int(raw_value.strip().split()[0])
        total = meminfo.get("MemTotal", 0)
        available = meminfo.get("MemAvailable", 0)
        if total <= 0:
            return 0.0
        used = total - available
        return round((used / total) * 100, 2)
    except Exception:
        return 0.0


def _status_payload_dict() -> dict:
    disk = shutil.disk_usage(settings.data_dir)
    disk_used_percent = 0.0
    if disk.total > 0:
        disk_used_percent = round(((disk.total - disk.free) / disk.total) * 100, 2)
    runtime_type, runtime_status = _runtime_status()
    return {
        "worker_id": settings.worker_id,
        "status": "online",
        "vpn_port": settings.vpn_port,
        "public_ip": settings.public_ip,
        "api_port": settings.worker_api_port,
        "active_configs": len(service.list_vpn_configs()),
        "tls_enabled": bool(
            settings.worker_tls_cert_path and settings.worker_tls_key_path
        ),
        "version": settings.app_version,
        "api_url": settings.worker_public_url,
        "runtime_type": runtime_type,
        "runtime_status": runtime_status,
        "disk_used_percent": disk_used_percent,
        "memory_used_percent": _memory_used_percent(),
    }


def _validate_remote_worker(worker_id: str, worker_token: str):
    row = db.get_server_by_worker_id(worker_id)
    if row is None or row["worker_token"] != worker_token:
        raise HTTPException(status_code=401, detail="invalid worker credentials")
    return row


def _set_operation_progress(
    operation_id: str, *, status: str, step: str, message: str
) -> None:
    OPERATION_PROGRESS[operation_id] = {
        "operation_id": operation_id,
        "status": status,
        "step": step,
        "message": message,
    }


async def _post_to_central(path: str, payload: dict) -> None:
    if not settings.central_api_url:
        return
    url = settings.central_api_url.rstrip("/") + path
    verify: bool | str = True
    if settings.central_api_ca_cert_path:
        verify = settings.central_api_ca_cert_path
    async with httpx.AsyncClient(timeout=20.0, verify=verify) as client:
        response = await client.post(url, json=payload)
    response.raise_for_status()


async def _worker_registration_loop() -> None:
    if settings.central_mode != "worker" or not settings.central_api_url:
        return
    while True:
        try:
            payload = _status_payload_dict()
            register_payload = {
                "worker_id": settings.worker_id,
                "worker_token": settings.worker_token,
                "api_url": settings.worker_public_url,
                "vpn_port": settings.vpn_port,
                "public_ip": settings.public_ip,
                "api_port": settings.worker_api_port,
                "version": settings.app_version,
            }
            await _post_to_central("/api/v1/worker/register", register_payload)
            heartbeat_payload = {
                **register_payload,
                "status": payload["status"],
                "active_configs": payload["active_configs"],
                "tls_enabled": payload["tls_enabled"],
                "runtime_type": payload["runtime_type"],
                "runtime_status": payload["runtime_status"],
                "disk_used_percent": payload["disk_used_percent"],
                "memory_used_percent": payload["memory_used_percent"],
            }
            await _post_to_central("/api/v1/worker/heartbeat", heartbeat_payload)
        except Exception as exc:
            logger.warning("worker registration/heartbeat failed: %s", exc)
        await asyncio.sleep(60)


@app.on_event("startup")
async def startup_event() -> None:
    asyncio.create_task(_worker_registration_loop())


@app.get("/api/v1/status", response_model=WorkerStatusResponse)
async def status(request: Request) -> WorkerStatusResponse:
    return WorkerStatusResponse.model_validate(_status_payload_dict())


@app.post("/api/v1/worker/register")
async def register_worker(request: RegisterWorkerRequest) -> dict[str, str]:
    row = _validate_remote_worker(request.worker_id, request.worker_token)
    db.update_server_installation(
        int(row["id"]),
        api_url=request.api_url,
        worker_id=request.worker_id,
        worker_token=request.worker_token,
        vpn_port=request.vpn_port,
        public_ip=request.public_ip,
        verify_tls=bool(int(row["verify_tls"])),
        stack_path=str(row["stack_path"]),
        api_port=request.api_port,
        status="online",
        mark_seen=True,
    )
    db.add_worker_heartbeat(
        int(row["id"]),
        json.dumps(
            {
                "event": "register",
                "api_url": request.api_url,
                "vpn_port": request.vpn_port,
                "public_ip": request.public_ip,
                "api_port": request.api_port,
                "version": request.version,
            },
            ensure_ascii=False,
        ),
    )
    asyncio.create_task(service.rotate_worker_control_tls(row))
    return {"status": "registered"}


@app.post("/api/v1/worker/heartbeat")
async def worker_heartbeat(request: HeartbeatRequest) -> dict[str, str]:
    row = _validate_remote_worker(request.worker_id, request.worker_token)
    db.update_server_installation(
        int(row["id"]),
        api_url=request.api_url,
        worker_id=request.worker_id,
        worker_token=request.worker_token,
        vpn_port=request.vpn_port,
        public_ip=request.public_ip,
        verify_tls=bool(int(row["verify_tls"])),
        stack_path=str(row["stack_path"]),
        api_port=request.api_port,
        status=request.status,
        mark_seen=True,
    )
    db.add_worker_heartbeat(
        int(row["id"]),
        json.dumps(request.model_dump(), ensure_ascii=False),
    )
    asyncio.create_task(service.rotate_worker_control_tls(row))
    return {"status": "ok"}


@app.post("/api/v1/configs/create", response_model=ConfigResponse)
async def create_config(
    payload: CreateConfigRequest, request: Request
) -> ConfigResponse:
    _validate_local_worker(payload.worker_id, payload.worker_token)
    operation_id = payload.operation_id or ""
    if operation_id:
        _set_operation_progress(
            operation_id,
            status="running",
            step="queued",
            message="Queuing config creation task...",
        )
    try:
        def on_progress(step: str, message: str) -> None:
            if operation_id:
                _set_operation_progress(
                    operation_id,
                    status="running",
                    step=step,
                    message=message,
                )

        result = await service.create_vpn_config(
            username=payload.username,
            password=payload.password,
            auth_type=payload.auth_type,
            progress_callback=on_progress,
        )
        if operation_id:
            _set_operation_progress(
                operation_id,
                status="completed",
                step="done",
                message="Config created.",
            )
        return ConfigResponse(
            fqdn=result.fqdn,
            port=result.port,
            username=result.username,
            password=result.password,
            protocol_type=result.protocol_type,
            connection_uri=result.connection_uri,
            certificate_path=result.certificate_path,
            private_key_path=result.private_key_path,
            client_cert_path=result.client_cert_path,
            client_key_path=result.client_key_path,
        )
    except Exception as exc:
        if operation_id:
            _set_operation_progress(
                operation_id,
                status="failed",
                step="failed",
                message=str(exc),
            )
        logger.exception("failed to create vpn config")
        raise HTTPException(status_code=500, detail=str(exc))




@app.post("/api/v1/operations/status", response_model=OperationStatusResponse)
async def operation_status(
    payload: OperationStatusRequest, request: Request
) -> OperationStatusResponse:
    _validate_local_worker(payload.worker_id, payload.worker_token)
    state = OPERATION_PROGRESS.get(payload.operation_id)
    if state is None:
        raise HTTPException(status_code=404, detail="operation not found")
    return OperationStatusResponse.model_validate(state)


@app.post("/api/v1/configs/delete")
async def delete_config(
    payload: DeleteConfigRequest, request: Request
) -> dict[str, str]:
    _validate_local_worker(payload.worker_id, payload.worker_token)
    target_id = payload.config_id
    if target_id is None:
        rows = [
            row
            for row in service.list_vpn_configs()
            if row["fqdn"] == payload.fqdn and row["username"] == payload.username
        ]
        if not rows:
            raise HTTPException(status_code=404, detail="config not found")
        target_id = int(rows[0]["id"])
    deleted = await service.delete_vpn_config(target_id)
    return {"status": "deleted" if deleted else "absent"}


@app.post("/api/v1/runtime/sync")
async def runtime_sync(payload: RuntimeSyncRequest, request: Request) -> dict[str, str]:
    _validate_local_worker(payload.worker_id, payload.worker_token)
    await asyncio.to_thread(service.sync_runtime, payload.strict_certificates)
    return {"status": "synced"}




def _schedule_process_restart() -> None:
    async def restart() -> None:
        await asyncio.sleep(1)
        os._exit(0)

    asyncio.create_task(restart())


@app.post("/api/v1/control-tls/rotate")
async def rotate_control_tls(
    payload: RotateControlTlsRequest, request: Request
) -> dict[str, str]:
    _validate_local_worker(payload.worker_id, payload.worker_token)
    cert_path = settings.worker_tls_cert_path
    key_path = settings.worker_tls_key_path
    ca_path = settings.central_api_ca_cert_path
    if not cert_path or not key_path or not ca_path:
        raise HTTPException(status_code=400, detail="control tls paths are not configured")
    for raw_value, label in (
        (payload.tls_cert_pem, "tls cert"),
        (payload.tls_key_pem, "tls key"),
        (payload.ca_cert_pem, "ca cert"),
    ):
        if "-----BEGIN" not in raw_value or "-----END" not in raw_value:
            raise HTTPException(status_code=400, detail=f"invalid {label} pem")
    for path in (cert_path, key_path, ca_path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(cert_path, "w", encoding="utf-8") as handle:
        handle.write(payload.tls_cert_pem)
    with open(key_path, "w", encoding="utf-8") as handle:
        handle.write(payload.tls_key_pem)
    os.chmod(key_path, 0o600)
    with open(ca_path, "w", encoding="utf-8") as handle:
        handle.write(payload.ca_cert_pem)
    _schedule_process_restart()
    return {"status": "rotated"}

