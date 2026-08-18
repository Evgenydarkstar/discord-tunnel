//! Process-lifetime protocol-free loopback bridges backed by [`Runtime`].

use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::net::{Ipv4Addr, Shutdown, TcpListener, TcpStream, UdpSocket};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::queue::{Command, Event};
use crate::runtime::Runtime;
use crate::session::RestartPolicy;
use crate::transport::{TransportConfig, TransportError};

const IO_TIMEOUT: Duration = Duration::from_millis(5);
const OPEN_TIMEOUT: Duration = Duration::from_secs(20);
const REMOTE_EOF_GRACE: Duration = Duration::from_secs(2);

static SERVICE: OnceLock<EmbeddedService> = OnceLock::new();
static START_LOCK: Mutex<()> = Mutex::new(());
static NEXT_FLOW: AtomicU32 = AtomicU32::new(1);

struct EmbeddedService {
    runtime: Arc<Runtime>,
    routes: Routes,
    bridges: Mutex<HashMap<u64, mpsc::Sender<()>>>,
}

type Routes = Arc<Mutex<HashMap<u64, mpsc::Sender<Event>>>>;

struct SessionRoute {
    runtime: Arc<Runtime>,
    routes: Routes,
    id: u64,
}

impl Drop for SessionRoute {
    fn drop(&mut self) {
        cleanup(&self.runtime, &self.routes, self.id);
    }
}

pub(crate) enum StartError {
    Io,
    Transport(TransportError),
}

pub(crate) fn start(
    server_host: String,
    server_port: u16,
    token: String,
    ca_pem: Option<PathBuf>,
    insecure: bool,
    listen_port: u16,
) -> Result<(), StartError> {
    let _guard = START_LOCK.lock().map_err(|_| StartError::Io)?;
    if SERVICE.get().is_some() {
        return Ok(());
    }

    let runtime = Arc::new(Runtime::new(1024));
    runtime
        .start_transport(TransportConfig {
            server_host,
            server_port,
            token,
            ca_pem,
            insecure,
        })
        .map_err(StartError::Transport)?;

    let routes: Routes = Default::default();
    spawn_dispatcher(runtime.clone(), routes.clone()).map_err(|_| StartError::Io)?;
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, listen_port)).map_err(|_| StartError::Io)?;
    spawn_listener(listener, runtime.clone(), routes.clone()).map_err(|_| StartError::Io)?;
    SERVICE
        .set(EmbeddedService {
            runtime,
            routes,
            bridges: Mutex::new(HashMap::new()),
        })
        .map_err(|_| StartError::Io)
}

pub(crate) fn is_ready() -> bool {
    SERVICE.get().is_some()
}

pub(crate) fn open_tcp_bridge(host: String, port: u16) -> io::Result<(u64, u16)> {
    validate_target(&host, port)?;
    let service = SERVICE
        .get()
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "runtime is not ready"))?;
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))?;
    listener.set_nonblocking(true)?;
    let local_port = listener.local_addr()?.port();
    let (id, rx) = open_session(service, Command::OpenTcp { id: 0, host, port })?;
    let (cancel_tx, cancel_rx) = mpsc::channel();
    service.bridges.lock().unwrap().insert(id, cancel_tx);

    let runtime = service.runtime.clone();
    let routes = service.routes.clone();
    std::thread::Builder::new()
        .name("dr-raw-tcp".into())
        .spawn(move || {
            let deadline = Instant::now() + OPEN_TIMEOUT;
            let stream = loop {
                if cancel_rx.try_recv().is_ok() || Instant::now() >= deadline {
                    finish_bridge(id);
                    return;
                }
                match listener.accept() {
                    Ok((stream, _)) => break stream,
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                        std::thread::sleep(IO_TIMEOUT);
                    }
                    Err(_) => {
                        finish_bridge(id);
                        return;
                    }
                }
            };
            let _ = pump_raw_tcp(stream, runtime, id, rx, cancel_rx);
            routes.lock().unwrap().remove(&id);
            finish_bridge(id);
        })?;
    Ok((id, local_port))
}

pub(crate) fn open_udp_bridge(host: String, port: u16) -> io::Result<(u64, u16)> {
    validate_target(&host, port)?;
    let service = SERVICE
        .get()
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "runtime is not ready"))?;
    let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0))?;
    socket.set_read_timeout(Some(IO_TIMEOUT))?;
    let local_port = socket.local_addr()?.port();
    let id = service.runtime.allocate_id();
    let flow = NEXT_FLOW.fetch_add(1, Ordering::Relaxed).max(1);
    let rx = register_route(&service.routes, id);
    service
        .runtime
        .open(id, RestartPolicy::default_backoff())
        .map_err(session_io)?;
    service
        .runtime
        .enqueue(id, Command::OpenUdp { id })
        .map_err(session_io)?;
    if wait_status(&rx) != Some(200) {
        cleanup(&service.runtime, &service.routes, id);
        return Err(io::Error::new(io::ErrorKind::ConnectionRefused, "UDP bridge rejected"));
    }

    let (cancel_tx, cancel_rx) = mpsc::channel();
    service.bridges.lock().unwrap().insert(id, cancel_tx);
    let runtime = service.runtime.clone();
    let routes = service.routes.clone();
    std::thread::Builder::new()
        .name("dr-raw-udp".into())
        .spawn(move || {
            let mut client = None;
            let mut packet = [0u8; 65535];
            loop {
                if cancel_rx.try_recv().is_ok() {
                    break;
                }
                match socket.recv_from(&mut packet) {
                    Ok((size, source)) => {
                        client = Some(source);
                        let _ = runtime.enqueue(
                            id,
                            Command::SendUdp {
                                id,
                                flow,
                                host: host.clone(),
                                port,
                                data: packet[..size].to_vec(),
                            },
                        );
                    }
                    Err(error)
                        if matches!(
                            error.kind(),
                            io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                        ) => {}
                    Err(_) => break,
                }
                while let Ok(event) = rx.try_recv() {
                    match event {
                        Event::UdpData {
                            flow: event_flow,
                            data,
                            ..
                        } if event_flow == flow => {
                            if let Some(destination) = client {
                                let _ = socket.send_to(&data, destination);
                            }
                        }
                        Event::Error { .. } => break,
                        _ => {}
                    }
                }
            }
            routes.lock().unwrap().remove(&id);
            finish_bridge(id);
        })?;
    Ok((id, local_port))
}

pub(crate) fn close_bridge(id: u64) -> io::Result<()> {
    let service = SERVICE
        .get()
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "runtime is not ready"))?;
    let cancel = service.bridges.lock().unwrap().remove(&id);
    let Some(cancel) = cancel else {
        return Err(io::Error::new(io::ErrorKind::NotFound, "bridge not found"));
    };
    let _ = cancel.send(());
    Ok(())
}

fn open_session(service: &EmbeddedService, command: Command) -> io::Result<(u64, mpsc::Receiver<Event>)> {
    let id = service.runtime.allocate_id();
    let command = match command {
        Command::OpenTcp { host, port, .. } => Command::OpenTcp { id, host, port },
        _ => return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid bridge command")),
    };
    let rx = register_route(&service.routes, id);
    service
        .runtime
        .open(id, RestartPolicy::default_backoff())
        .map_err(session_io)?;
    service.runtime.enqueue(id, command).map_err(session_io)?;
    if wait_status(&rx) != Some(200) {
        cleanup(&service.runtime, &service.routes, id);
        return Err(io::Error::new(io::ErrorKind::ConnectionRefused, "TCP bridge rejected"));
    }
    Ok((id, rx))
}

fn pump_raw_tcp(
    mut stream: TcpStream,
    runtime: Arc<Runtime>,
    id: u64,
    rx: mpsc::Receiver<Event>,
    cancel: mpsc::Receiver<()>,
) -> io::Result<()> {
    stream.set_read_timeout(Some(IO_TIMEOUT))?;
    stream.set_write_timeout(Some(OPEN_TIMEOUT))?;
    let stop = Arc::new(AtomicBool::new(false));
    let remote_eof = Arc::new(AtomicBool::new(false));
    let mut writer = stream.try_clone()?;
    let writer_stop = stop.clone();
    let writer_remote_eof = remote_eof.clone();
    let writer_thread = std::thread::spawn(move || {
        while !writer_stop.load(Ordering::Relaxed) {
            match rx.recv_timeout(IO_TIMEOUT) {
                Ok(Event::TcpData { data, eof, .. }) => {
                    if !data.is_empty() && writer.write_all(&data).is_err() {
                        break;
                    }
                    if eof {
                        writer_remote_eof.store(true, Ordering::Relaxed);
                        let _ = writer.shutdown(Shutdown::Write);
                        break;
                    }
                }
                Ok(Event::Error { .. }) | Err(mpsc::RecvTimeoutError::Disconnected) => {
                    writer_remote_eof.store(true, Ordering::Relaxed);
                    break;
                }
                Err(mpsc::RecvTimeoutError::Timeout) | Ok(_) => {}
            }
        }
    });

    let mut packet = [0u8; 16 * 1024];
    let mut remote_eof_seen_at = None;
    while cancel.try_recv().is_err() {
        match stream.read(&mut packet) {
            Ok(0) => {
                let _ = runtime.enqueue(id, Command::SendTcp { id, data: Vec::new(), eof: true });
                break;
            }
            Ok(size) => {
                if runtime
                    .enqueue(id, Command::SendTcp { id, data: packet[..size].to_vec(), eof: false })
                    .is_err()
                {
                    break;
                }
            }
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {
                    if remote_eof.load(Ordering::Relaxed) {
                        let seen_at = remote_eof_seen_at.get_or_insert_with(Instant::now);
                        if seen_at.elapsed() >= REMOTE_EOF_GRACE {
                            break;
                        }
                    }
                }
            Err(_) => break,
        }
    }
    stop.store(true, Ordering::Relaxed);
    let _ = writer_thread.join();
    Ok(())
}

fn finish_bridge(id: u64) {
    if let Some(service) = SERVICE.get() {
        service.bridges.lock().unwrap().remove(&id);
        cleanup(&service.runtime, &service.routes, id);
    }
}

fn validate_target(host: &str, port: u16) -> io::Result<()> {
    if host.trim().is_empty() || host.as_bytes().contains(&0) || port == 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid bridge target"));
    }
    Ok(())
}

fn spawn_dispatcher(runtime: Arc<Runtime>, routes: Routes) -> io::Result<()> {
    std::thread::Builder::new()
        .name("dr-event-dispatch".into())
        .spawn(move || loop {
            let mut found = false;
            while runtime
                .poll_events(|id, event| {
                    found = true;
                    let sender = routes.lock().unwrap().get(&id).cloned();
                    if sender.as_ref().is_some_and(|tx| tx.send(event).is_err()) {
                        routes.lock().unwrap().remove(&id);
                    }
                    true
                })
                .unwrap_or(false)
            {}
            if !found {
                std::thread::sleep(Duration::from_millis(2));
            }
        })?;
    Ok(())
}

fn spawn_listener(listener: TcpListener, runtime: Arc<Runtime>, routes: Routes) -> io::Result<()> {
    std::thread::Builder::new()
        .name("dr-http-listener".into())
        .spawn(move || {
            for stream in listener.incoming() {
                let Ok(stream) = stream else { continue };
                let runtime = runtime.clone();
                let routes = routes.clone();
                let _ = std::thread::Builder::new()
                    .name("dr-http-client".into())
                    .spawn(move || {
                        let _ = handle_http_client(stream, runtime, routes);
                    });
            }
        })?;
    Ok(())
}

fn handle_http_client(
    mut stream: TcpStream,
    runtime: Arc<Runtime>,
    routes: Routes,
) -> io::Result<()> {
    let mut request = Vec::new();
    let mut byte = [0u8; 1];
    while !request.ends_with(b"\r\n\r\n") {
        if request.len() >= 64 * 1024 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "HTTP proxy request is too large"));
        }
        stream.read_exact(&mut byte)?;
        request.push(byte[0]);
    }
    let request = std::str::from_utf8(&request)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid HTTP proxy request"))?;
    let first_line = request.lines().next().unwrap_or_default();
    let mut parts = first_line.split_whitespace();
    if parts.next() != Some("CONNECT") {
        stream.write_all(b"HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n")?;
        return Ok(());
    }
    let (host, port) = parse_http_authority(parts.next().unwrap_or_default())?;
    handle_connect(stream, runtime, routes, host, port)
}

fn parse_http_authority(authority: &str) -> io::Result<(String, u16)> {
    let (host, port) = if let Some(rest) = authority.strip_prefix('[') {
        let end = rest.find(']').ok_or(io::ErrorKind::InvalidData)?;
        let host = &rest[..end];
        let port = rest[end + 1..]
            .strip_prefix(':')
            .ok_or(io::ErrorKind::InvalidData)?;
        (host, port)
    } else {
        authority.rsplit_once(':').ok_or(io::ErrorKind::InvalidData)?
    };
    let port = port
        .parse::<u16>()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid CONNECT port"))?;
    if host.is_empty() || port == 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid CONNECT target"));
    }
    Ok((host.to_owned(), port))
}

fn handle_connect(
    mut stream: TcpStream,
    runtime: Arc<Runtime>,
    routes: Routes,
    host: String,
    port: u16,
) -> io::Result<()> {
    let id = runtime.allocate_id();
    let rx = register_route(&routes, id);
    if runtime.open(id, RestartPolicy::default_backoff()).is_err() {
        routes.lock().unwrap().remove(&id);
        write_connect_reply(&mut stream, 1)?;
        return Ok(());
    }
    let _route = SessionRoute {
        runtime: runtime.clone(),
        routes: routes.clone(),
        id,
    };
    if runtime
        .enqueue(
            id,
            Command::OpenTcp {
                id,
                host,
                port,
            },
        )
        .is_err()
    {
        write_connect_reply(&mut stream, 1)?;
        return Ok(());
    }

    let status = wait_status(&rx);
    if status != Some(200) {
        write_connect_reply(&mut stream, 5)?;
        return Ok(());
    }
    write_connect_reply(&mut stream, 0)?;
    stream.set_read_timeout(Some(IO_TIMEOUT))?;
    stream.set_write_timeout(Some(OPEN_TIMEOUT))?;

    let stop = Arc::new(AtomicBool::new(false));
    let remote_eof = Arc::new(AtomicBool::new(false));
    let mut writer = stream.try_clone()?;
    let writer_stop = stop.clone();
    let writer_remote_eof = remote_eof.clone();
    let writer_thread = std::thread::spawn(move || {
        while !writer_stop.load(Ordering::Relaxed) {
            let event = match rx.recv_timeout(IO_TIMEOUT) {
                Ok(event) => event,
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    writer_remote_eof.store(true, Ordering::Relaxed);
                    break;
                }
            };
            match event {
                Event::TcpData { data, eof, .. } => {
                    if !data.is_empty() {
                        if writer.write_all(&data).is_err() {
                            break;
                        }
                    }
                    if eof {
                        writer_remote_eof.store(true, Ordering::Relaxed);
                        break;
                    }
                }
                Event::Error { .. } => {
                    writer_remote_eof.store(true, Ordering::Relaxed);
                    break;
                }
                _ => {}
            }
        }
    });

    let mut buffer = [0u8; 16 * 1024];
    let mut local_eof = false;
    let mut remote_eof_seen_at = None;
    while !stop.load(Ordering::Relaxed) {
        match stream.read(&mut buffer) {
            Ok(0) => {
                let _ = runtime.enqueue(
                    id,
                    Command::SendTcp {
                        id,
                        data: Vec::new(),
                        eof: true,
                    },
                );
                local_eof = true;
                let _ = stream.shutdown(Shutdown::Read);
                break;
            }
            Ok(n) => {
                if runtime
                    .enqueue(
                        id,
                        Command::SendTcp {
                            id,
                            data: buffer[..n].to_vec(),
                            eof: false,
                        },
                    )
                    .is_err()
                {
                    break;
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {
                    if remote_eof.load(Ordering::Relaxed) {
                        let seen_at = remote_eof_seen_at.get_or_insert_with(Instant::now);
                        if seen_at.elapsed() >= REMOTE_EOF_GRACE {
                            break;
                        }
                    }
                }
            Err(_) => break,
        }
    }
    if local_eof {
        let deadline = Instant::now() + OPEN_TIMEOUT;
        while !remote_eof.load(Ordering::Relaxed) && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    if remote_eof.load(Ordering::Relaxed) {
        let _ = stream.shutdown(Shutdown::Write);
    }
    stop.store(true, Ordering::Relaxed);
    let _ = writer_thread.join();
    Ok(())
}

fn write_connect_reply(stream: &mut TcpStream, status: u8) -> io::Result<()> {
    let response = if status == 0 {
        b"HTTP/1.1 200 Connection Established\r\n\r\n".as_slice()
    } else {
        b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n".as_slice()
    };
    stream.write_all(response)
}

fn register_route(routes: &Routes, id: u64) -> mpsc::Receiver<Event> {
    let (tx, rx) = mpsc::channel();
    routes.lock().unwrap().insert(id, tx);
    rx
}

fn cleanup(runtime: &Runtime, routes: &Routes, id: u64) {
    routes.lock().unwrap().remove(&id);
    let _ = runtime.close(id);
}

fn wait_status(rx: &mpsc::Receiver<Event>) -> Option<u16> {
    let deadline = Instant::now() + OPEN_TIMEOUT;
    while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
        match rx.recv_timeout(remaining) {
            Ok(Event::Status { status, .. }) => return Some(status),
            Ok(Event::Error { .. }) | Err(_) => return None,
            Ok(_) => {}
        }
    }
    None
}

fn session_io(error: crate::session::SessionError) -> io::Error {
    io::Error::new(
        io::ErrorKind::Other,
        format!("runtime session error: {error:?}"),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_http_connect_authorities() {
        assert_eq!(
            parse_http_authority("updates.discord.com:443").unwrap(),
            ("updates.discord.com".into(), 443)
        );
        assert_eq!(
            parse_http_authority("[2001:db8::1]:8443").unwrap(),
            ("2001:db8::1".into(), 8443)
        );
        assert!(parse_http_authority("missing-port").is_err());
    }
}
