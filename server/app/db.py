from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from pathlib import Path


SCHEMA = """
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS servers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    host TEXT NOT NULL,
    ssh_port INTEGER NOT NULL DEFAULT 22,
    ssh_user TEXT NOT NULL DEFAULT '',
    auth_type TEXT NOT NULL DEFAULT '',
    auth_secret TEXT NOT NULL DEFAULT '',
    api_url TEXT NOT NULL,
    worker_id TEXT NOT NULL UNIQUE,
    worker_token TEXT NOT NULL,
    vpn_port INTEGER NOT NULL,
    public_ip TEXT NOT NULL,
    verify_tls INTEGER NOT NULL DEFAULT 1,
    stack_path TEXT NOT NULL DEFAULT '',
    api_port INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'unknown',
    enabled INTEGER NOT NULL DEFAULT 1,
    vpn_type TEXT NOT NULL DEFAULT 'discord',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen_at TEXT
);

CREATE TABLE IF NOT EXISTS server_install_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id INTEGER,
    status TEXT NOT NULL DEFAULT 'pending',
    step TEXT NOT NULL DEFAULT '',
    log TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(server_id) REFERENCES servers(id)
);

CREATE TABLE IF NOT EXISTS server_install_job_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id INTEGER NOT NULL,
    server_id INTEGER,
    step TEXT NOT NULL DEFAULT '',
    log TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(job_id) REFERENCES server_install_jobs(id),
    FOREIGN KEY(server_id) REFERENCES servers(id)
);

CREATE TABLE IF NOT EXISTS subdomains (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    fqdn TEXT NOT NULL UNIQUE,
    root_domain TEXT NOT NULL,
    label TEXT NOT NULL,
    record_type TEXT NOT NULL DEFAULT 'A',
    record_content TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS vpn_configs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id INTEGER,
    fqdn TEXT NOT NULL,
    root_domain TEXT NOT NULL,
    label TEXT NOT NULL,
    username TEXT NOT NULL UNIQUE,
    password TEXT NOT NULL,
    protocol_type TEXT NOT NULL DEFAULT 'discord',
    connection_uri TEXT NOT NULL DEFAULT '',
    config_payload TEXT NOT NULL DEFAULT '',
    display_host TEXT NOT NULL DEFAULT '',
    display_port INTEGER NOT NULL DEFAULT 0,
    auth_type TEXT NOT NULL DEFAULT 'password',
    certificate_path TEXT NOT NULL DEFAULT '',
    private_key_path TEXT NOT NULL DEFAULT '',
    client_cert_path TEXT NOT NULL DEFAULT '',
    client_key_path TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(server_id) REFERENCES servers(id)
);

CREATE TABLE IF NOT EXISTS worker_heartbeats (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id INTEGER NOT NULL,
    status_payload TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(server_id) REFERENCES servers(id)
);
"""


class Database:
    SQLITE_TIMEOUT_SECONDS = 30.0
    SQLITE_BUSY_TIMEOUT_MS = 30000
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.initialize()

    @contextmanager
    def connect(self):
        conn = sqlite3.connect(self.path, timeout=self.SQLITE_TIMEOUT_SECONDS)
        conn.row_factory = sqlite3.Row
        conn.execute(f"PRAGMA busy_timeout={self.SQLITE_BUSY_TIMEOUT_MS}")
        # WAL mode for better concurrency and durability
        conn.execute("PRAGMA journal_mode=WAL")
        # FULL synchronous to guarantee data durability on disk
        conn.execute("PRAGMA synchronous=FULL")
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()

    def initialize(self) -> None:
        with self.connect() as conn:
            conn.executescript(SCHEMA)
            self._ensure_column(
                conn, "servers", "ssh_port", "INTEGER NOT NULL DEFAULT 22"
            )
            self._ensure_column(conn, "servers", "ssh_user", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(
                conn, "servers", "auth_type", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "servers", "auth_secret", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "servers", "verify_tls", "INTEGER NOT NULL DEFAULT 1"
            )
            self._ensure_column(
                conn, "servers", "stack_path", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "servers", "api_port", "INTEGER NOT NULL DEFAULT 0"
            )
            self._ensure_column(conn, "vpn_configs", "server_id", "INTEGER")
            self._ensure_column(
                conn, "vpn_configs", "certificate_path", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "vpn_configs", "private_key_path", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "vpn_configs", "protocol_type", "TEXT NOT NULL DEFAULT 'discord'"
            )
            self._ensure_column(
                conn, "vpn_configs", "connection_uri", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "vpn_configs", "config_payload", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "vpn_configs", "display_host", "TEXT NOT NULL DEFAULT ''"
            )
            self._ensure_column(
                conn, "vpn_configs", "display_port", "INTEGER NOT NULL DEFAULT 0"
            )
            self._drop_column_if_exists(conn, "servers", "is_local")
            self._ensure_column(
                conn, "servers", "vpn_type", "TEXT NOT NULL DEFAULT 'discord'"
            )
            self._drop_column_if_exists(conn, "vpn_configs", "cookie")

    def _drop_column_if_exists(
        self, conn: sqlite3.Connection, table: str, column: str
    ) -> None:
        columns = {
            row["name"]
            for row in conn.execute(f"PRAGMA table_info({table})").fetchall()
        }
        if column in columns:
            try:
                conn.execute(f"ALTER TABLE {table} DROP COLUMN {column}")
            except Exception:
                pass  # SQLite < 3.35.0 does not support DROP COLUMN

    def _ensure_column(
        self, conn: sqlite3.Connection, table: str, column: str, ddl: str
    ) -> None:
        columns = {
            row["name"]
            for row in conn.execute(f"PRAGMA table_info({table})").fetchall()
        }
        if column not in columns:
            conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {ddl}")

    def set_setting(self, key: str, value: str) -> None:
        with self.connect() as conn:
            conn.execute(
                "INSERT INTO settings(key, value) VALUES(?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, value),
            )

    def get_setting(self, key: str) -> str | None:
        with self.connect() as conn:
            row = conn.execute(
                "SELECT value FROM settings WHERE key = ?", (key,)
            ).fetchone()
            return row["value"] if row else None

    def add_server(
        self,
        *,
        name: str,
        host: str,
        api_url: str,
        worker_id: str,
        worker_token: str,
        vpn_port: int,
        public_ip: str,
        ssh_port: int = 22,
        ssh_user: str = "",
        auth_type: str = "",
        auth_secret: str = "",
        verify_tls: bool = True,
        stack_path: str = "",
        api_port: int = 0,
        enabled: bool = True,
        status: str = "unknown",
        vpn_type: str = "discord",
    ) -> int:
        with self.connect() as conn:
            cur = conn.execute(
                "INSERT INTO servers(name, host, ssh_port, ssh_user, auth_type, auth_secret, api_url, worker_id, worker_token, vpn_port, public_ip, verify_tls, stack_path, api_port, status, enabled, vpn_type) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    name,
                    host,
                    ssh_port,
                    ssh_user,
                    auth_type,
                    auth_secret,
                    api_url,
                    worker_id,
                    worker_token,
                    vpn_port,
                    public_ip,
                    int(verify_tls),
                    stack_path,
                    api_port,
                    status,
                    int(enabled),
                    vpn_type,
                ),
            )
            return int(cur.lastrowid)

    def list_servers(self, enabled_only: bool = False) -> list[sqlite3.Row]:
        with self.connect() as conn:
            query = (
                "SELECT id, name, host, api_url, worker_id, worker_token, vpn_port, public_ip, "
                "ssh_port, ssh_user, auth_type, auth_secret, verify_tls, stack_path, api_port, "
                "status, enabled, vpn_type, created_at, updated_at, last_seen_at "
                "FROM servers"
            )
            params: tuple[object, ...] = ()
            if enabled_only:
                query += " WHERE enabled = 1"
            query += " ORDER BY name ASC"
            return conn.execute(query, params).fetchall()

    def get_server(self, server_id: int) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT id, name, host, api_url, worker_id, worker_token, vpn_port, public_ip, "
                "ssh_port, ssh_user, auth_type, auth_secret, verify_tls, stack_path, api_port, "
                "status, enabled, vpn_type, created_at, updated_at, last_seen_at "
                "FROM servers WHERE id = ?",
                (server_id,),
            ).fetchone()

    def get_server_by_worker_id(self, worker_id: str) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT id, name, host, api_url, worker_id, worker_token, vpn_port, public_ip, "
                "ssh_port, ssh_user, auth_type, auth_secret, verify_tls, stack_path, api_port, "
                "status, enabled, vpn_type, created_at, updated_at, last_seen_at "
                "FROM servers WHERE worker_id = ?",
                (worker_id,),
            ).fetchone()

    def update_server_status(
        self, server_id: int, status: str, mark_seen: bool = True
    ) -> None:
        with self.connect() as conn:
            if mark_seen:
                conn.execute(
                    "UPDATE servers SET status = ?, updated_at = CURRENT_TIMESTAMP, last_seen_at = CURRENT_TIMESTAMP "
                    "WHERE id = ?",
                    (status, server_id),
                )
            else:
                conn.execute(
                    "UPDATE servers SET status = ?, updated_at = CURRENT_TIMESTAMP "
                    "WHERE id = ?",
                    (status, server_id),
                )

    def update_server_installation(
        self,
        server_id: int,
        *,
        api_url: str,
        worker_id: str,
        worker_token: str,
        vpn_port: int,
        public_ip: str,
        verify_tls: bool,
        stack_path: str,
        api_port: int,
        status: str,
        mark_seen: bool = False,
    ) -> None:
        with self.connect() as conn:
            if mark_seen:
                conn.execute(
                    "UPDATE servers SET api_url = ?, worker_id = ?, worker_token = ?, vpn_port = ?, public_ip = ?, "
                    "verify_tls = ?, stack_path = ?, api_port = ?, status = ?, updated_at = CURRENT_TIMESTAMP, "
                    "last_seen_at = CURRENT_TIMESTAMP WHERE id = ?",
                    (
                        api_url,
                        worker_id,
                        worker_token,
                        vpn_port,
                        public_ip,
                        int(verify_tls),
                        stack_path,
                        api_port,
                        status,
                        server_id,
                    ),
                )
            else:
                conn.execute(
                    "UPDATE servers SET api_url = ?, worker_id = ?, worker_token = ?, vpn_port = ?, public_ip = ?, "
                    "verify_tls = ?, stack_path = ?, api_port = ?, status = ?, updated_at = CURRENT_TIMESTAMP "
                    "WHERE id = ?",
                    (
                        api_url,
                        worker_id,
                        worker_token,
                        vpn_port,
                        public_ip,
                        int(verify_tls),
                        stack_path,
                        api_port,
                        status,
                        server_id,
                    ),
                )

    def update_server_auth(
        self, server_id: int, *, auth_type: str, auth_secret: str
    ) -> None:
        with self.connect() as conn:
            conn.execute(
                "UPDATE servers SET auth_type = ?, auth_secret = ?, updated_at = CURRENT_TIMESTAMP "
                "WHERE id = ?",
                (auth_type, auth_secret, server_id),
            )

    def create_install_job(
        self, server_id: int | None, *, status: str, step: str, log: str = ""
    ) -> int:
        with self.connect() as conn:
            cur = conn.execute(
                "INSERT INTO server_install_jobs(server_id, status, step, log) VALUES(?, ?, ?, ?)",
                (server_id, status, step, log),
            )
            job_id = int(cur.lastrowid)
            conn.execute(
                "INSERT INTO server_install_job_events(job_id, server_id, step, log) VALUES(?, ?, ?, ?)",
                (job_id, server_id, step, log),
            )
            return job_id

    def update_install_job(
        self, job_id: int, *, status: str, step: str, log: str
    ) -> None:
        with self.connect() as conn:
            row = conn.execute(
                "SELECT server_id FROM server_install_jobs WHERE id = ?",
                (job_id,),
            ).fetchone()
            conn.execute(
                "UPDATE server_install_jobs SET status = ?, step = ?, log = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                (status, step, log, job_id),
            )
            conn.execute(
                "INSERT INTO server_install_job_events(job_id, server_id, step, log) VALUES(?, ?, ?, ?)",
                (job_id, row["server_id"] if row else None, step, log),
            )

    def assign_install_job_server(self, job_id: int, server_id: int) -> None:
        with self.connect() as conn:
            conn.execute(
                "UPDATE server_install_jobs SET server_id = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                (server_id, job_id),
            )
            conn.execute(
                "UPDATE server_install_job_events SET server_id = ? WHERE job_id = ? AND server_id IS NULL",
                (server_id, job_id),
            )

    def list_install_jobs(
        self, server_id: int | None = None, limit: int = 10
    ) -> list[sqlite3.Row]:
        with self.connect() as conn:
            if server_id is None:
                return conn.execute(
                    "SELECT id, server_id, status, step, log, created_at, updated_at FROM server_install_jobs "
                    "ORDER BY id DESC LIMIT ?",
                    (limit,),
                ).fetchall()
            return conn.execute(
                "SELECT id, server_id, status, step, log, created_at, updated_at FROM server_install_jobs "
                "WHERE server_id = ? OR (server_id IS NULL AND step = 'ssh-bootstrap') "
                "ORDER BY id DESC LIMIT ?",
                (server_id, limit),
            ).fetchall()

    def list_install_job_events(
        self, server_id: int | None = None, limit: int = 20
    ) -> list[sqlite3.Row]:
        with self.connect() as conn:
            if server_id is None:
                return conn.execute(
                    "SELECT id, job_id, server_id, step, log, created_at FROM server_install_job_events "
                    "ORDER BY id DESC LIMIT ?",
                    (limit,),
                ).fetchall()
            return conn.execute(
                "SELECT id, job_id, server_id, step, log, created_at FROM server_install_job_events "
                "WHERE server_id = ? ORDER BY id DESC LIMIT ?",
                (server_id, limit),
            ).fetchall()

    def add_worker_heartbeat(self, server_id: int, status_payload: str) -> None:
        with self.connect() as conn:
            conn.execute(
                "INSERT INTO worker_heartbeats(server_id, status_payload) VALUES(?, ?)",
                (server_id, status_payload),
            )

    def list_worker_heartbeats(
        self, server_id: int, limit: int = 10
    ) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                "SELECT id, server_id, status_payload, created_at FROM worker_heartbeats WHERE server_id = ? "
                "ORDER BY id DESC LIMIT ?",
                (server_id, limit),
            ).fetchall()

    def remove_server(self, server_id: int) -> bool:
        with self.connect() as conn:
            conn.execute("DELETE FROM vpn_configs WHERE server_id = ?", (server_id,))
            conn.execute(
                "DELETE FROM worker_heartbeats WHERE server_id = ?", (server_id,)
            )
            conn.execute(
                "DELETE FROM server_install_job_events WHERE server_id = ?",
                (server_id,),
            )
            conn.execute(
                "DELETE FROM server_install_jobs WHERE server_id = ?", (server_id,)
            )
            cur = conn.execute("DELETE FROM servers WHERE id = ?", (server_id,))
            return cur.rowcount > 0

    def set_server_enabled(self, server_id: int, enabled: bool) -> None:
        with self.connect() as conn:
            conn.execute(
                "UPDATE servers SET enabled = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                (int(enabled), server_id),
            )

    def clear_server_auth_secret(self, server_id: int) -> None:
        with self.connect() as conn:
            conn.execute(
                "UPDATE servers SET auth_secret = '', updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                (server_id,),
            )

    def count_vpn_configs_for_server(self, server_id: int) -> int:
        with self.connect() as conn:
            row = conn.execute(
                "SELECT COUNT(*) AS total FROM vpn_configs WHERE server_id = ?",
                (server_id,),
            ).fetchone()
            return int(row["total"])

    def add_subdomain(
        self,
        fqdn: str,
        root_domain: str,
        label: str,
        record_content: str,
        record_type: str = "A",
    ) -> None:
        with self.connect() as conn:
            conn.execute(
                "INSERT OR REPLACE INTO subdomains(fqdn, root_domain, label, record_type, record_content) "
                "VALUES(?, ?, ?, ?, ?)",
                (fqdn, root_domain, label, record_type, record_content),
            )

    def remove_subdomain(self, fqdn: str) -> None:
        with self.connect() as conn:
            conn.execute("DELETE FROM subdomains WHERE fqdn = ?", (fqdn,))

    def list_subdomains(self) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                "SELECT id, fqdn, root_domain, label, record_type, record_content, created_at "
                "FROM subdomains ORDER BY created_at DESC"
            ).fetchall()

    def get_subdomain_by_id(self, subdomain_id: int) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT id, fqdn, root_domain, label, record_type, record_content, created_at "
                "FROM subdomains WHERE id = ?",
                (subdomain_id,),
            ).fetchone()

    def get_subdomain(self, fqdn: str) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT id, fqdn, root_domain, label, record_type, record_content, created_at "
                "FROM subdomains WHERE fqdn = ?",
                (fqdn,),
            ).fetchone()

    def add_vpn_config(
        self,
        fqdn: str,
        root_domain: str,
        label: str,
        username: str,
        password: str,
        server_id: int | None = None,
        protocol_type: str = "discord",
        connection_uri: str = "",
        config_payload: str = "",
        display_host: str = "",
        display_port: int = 0,
        certificate_path: str = "",
        private_key_path: str = "",
        client_cert_path: str = "",
        client_key_path: str = "",
        auth_type: str = "password",
        config_id: int | None = None,
    ) -> int:
        with self.connect() as conn:
            if config_id is None:
                cur = conn.execute(
                    "INSERT INTO vpn_configs(server_id, fqdn, root_domain, label, username, password, protocol_type, connection_uri, config_payload, display_host, display_port, auth_type, certificate_path, private_key_path, client_cert_path, client_key_path) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        server_id,
                        fqdn,
                        root_domain,
                        label,
                        username,
                        password,
                        protocol_type,
                        connection_uri,
                        config_payload,
                        display_host,
                        display_port,
                        auth_type,
                        certificate_path,
                        private_key_path,
                        client_cert_path,
                        client_key_path,
                    ),
                )
            else:
                cur = conn.execute(
                    "INSERT INTO vpn_configs(id, server_id, fqdn, root_domain, label, username, password, protocol_type, connection_uri, config_payload, display_host, display_port, auth_type, certificate_path, private_key_path, client_cert_path, client_key_path) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        config_id,
                        server_id,
                        fqdn,
                        root_domain,
                        label,
                        username,
                        password,
                        protocol_type,
                        connection_uri,
                        config_payload,
                        display_host,
                        display_port,
                        auth_type,
                        certificate_path,
                        private_key_path,
                        client_cert_path,
                        client_key_path,
                    ),
                )
            return int(cur.lastrowid)

    def list_vpn_configs(self) -> list[sqlite3.Row]:
        with self.connect() as conn:
            return conn.execute(
                "SELECT v.id, v.server_id, s.name AS server_name, v.fqdn, v.root_domain, v.label, "
                "v.username, v.password, v.protocol_type, v.connection_uri, v.config_payload, "
                "v.display_host, v.display_port, v.auth_type, v.certificate_path, v.private_key_path, "
                "v.client_cert_path, v.client_key_path, v.created_at "
                "FROM vpn_configs v LEFT JOIN servers s ON s.id = v.server_id "
                "ORDER BY v.created_at DESC"
            ).fetchall()

    def get_vpn_config(self, config_id: int) -> sqlite3.Row | None:
        with self.connect() as conn:
            return conn.execute(
                "SELECT v.id, v.server_id, s.name AS server_name, v.fqdn, v.root_domain, v.label, "
                "v.username, v.password, v.protocol_type, v.connection_uri, v.config_payload, "
                "v.display_host, v.display_port, v.auth_type, v.certificate_path, v.private_key_path, "
                "v.client_cert_path, v.client_key_path, v.created_at "
                "FROM vpn_configs v LEFT JOIN servers s ON s.id = v.server_id "
                "WHERE v.id = ?",
                (config_id,),
            ).fetchone()

    def count_vpn_configs_for_fqdn(self, fqdn: str) -> int:
        with self.connect() as conn:
            row = conn.execute(
                "SELECT COUNT(*) AS total FROM vpn_configs WHERE fqdn = ?",
                (fqdn,),
            ).fetchone()
            return int(row["total"])

    def get_bound_server_id_for_fqdn(self, fqdn: str) -> int | None:
        with self.connect() as conn:
            row = conn.execute(
                "SELECT server_id FROM vpn_configs "
                "WHERE fqdn = ? AND server_id IS NOT NULL "
                "ORDER BY id ASC LIMIT 1",
                (fqdn,),
            ).fetchone()
            if row is None or row["server_id"] is None:
                return None
            return int(row["server_id"])

    def remove_vpn_config(self, config_id: int) -> bool:
        with self.connect() as conn:
            cur = conn.execute("DELETE FROM vpn_configs WHERE id = ?", (config_id,))
            if cur.rowcount > 0:
                remaining = conn.execute(
                    "SELECT COUNT(*) AS total FROM vpn_configs"
                ).fetchone()
                if remaining and int(remaining["total"]) == 0:
                    conn.execute(
                        "DELETE FROM sqlite_sequence WHERE name = 'vpn_configs'"
                    )
            return cur.rowcount > 0
