from __future__ import annotations

from pydantic import BaseModel


class WorkerStatusResponse(BaseModel):
    worker_id: str
    status: str
    vpn_port: int
    public_ip: str
    api_port: int
    active_configs: int
    tls_enabled: bool
    version: str
    runtime_type: str
    runtime_status: str
    disk_used_percent: float
    memory_used_percent: float


class CreateConfigRequest(BaseModel):
    worker_id: str
    worker_token: str
    operation_id: str | None = None
    username: str | None = None
    password: str | None = None
    auth_type: str = "password"


class ConfigResponse(BaseModel):
    fqdn: str
    port: int
    username: str
    password: str
    protocol_type: str = "discord"
    connection_uri: str = ""
    certificate_path: str = ""
    private_key_path: str = ""
    client_cert_path: str = ""
    client_key_path: str = ""


class OperationStatusRequest(BaseModel):
    worker_id: str
    worker_token: str
    operation_id: str


class OperationStatusResponse(BaseModel):
    operation_id: str
    status: str
    step: str
    message: str


class DeleteConfigRequest(BaseModel):
    worker_id: str
    worker_token: str
    config_id: int | None = None
    fqdn: str
    username: str


class RegisterWorkerRequest(BaseModel):
    worker_id: str
    worker_token: str
    api_url: str
    vpn_port: int
    public_ip: str
    api_port: int
    version: str


class HeartbeatRequest(BaseModel):
    worker_id: str
    worker_token: str
    api_url: str
    vpn_port: int
    public_ip: str
    api_port: int
    version: str
    status: str
    active_configs: int
    tls_enabled: bool
    runtime_type: str
    runtime_status: str
    disk_used_percent: float
    memory_used_percent: float


class RuntimeSyncRequest(BaseModel):
    worker_id: str
    worker_token: str
    strict_certificates: bool = False


class RotateControlTlsRequest(BaseModel):
    worker_id: str
    worker_token: str
    tls_cert_pem: str
    tls_key_pem: str
    ca_cert_pem: str
