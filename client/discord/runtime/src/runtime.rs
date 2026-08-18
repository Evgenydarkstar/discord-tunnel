//! Runtime worker: owns all sessions, drains bounded queues on dedicated
//! threads, and dispatches events back to the hook (runtime→hook direction).
//! The outward transport (QUIC/HTTP3) is a single [`Transport`] owned by one
//! "xport-driver" thread: session workers send [`TransportOp`]s over a channel,
//! and the driver translates inbound [`TransportEvent`]s into the egress
//! router. `Transport` itself is !Sync (h3's request handle), so it never
//! leaves the driver thread. Without a started transport the runtime runs in
//! loopback mode (commands echo back) so tests and smoke runs can exercise the
//! whole FFI surface offline.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use crate::queue::{BoundedQueue, Command, Event};
use crate::session::{RestartPolicy, RuntimeSession, SessionError};
use crate::transport::{Transport, TransportConfig, TransportError, TransportEvent};

type SessionHandle = Arc<RuntimeSession>;

/// Ops queued by session workers for the transport driver thread.
#[derive(Debug)]
enum TransportOp {
    OpenTcp { id: u64, host: String, port: u16 },
    SendTcp { id: u64, data: Vec<u8>, eof: bool },
    SendUdp { id: u64, flow: u32, host: String, port: u16, data: Vec<u8> },
    Close { id: u64 },
}

/// `Send + Sync` proxy for the transport: a channel into the owning driver
/// thread. All methods are non-blocking.
#[derive(Clone)]
struct TransportHandle {
    ops_tx: mpsc::Sender<TransportOp>,
}

impl TransportHandle {
    fn open_tcp(&self, id: u64, host: String, port: u16) -> Result<(), TransportError> {
        self.send(TransportOp::OpenTcp { id, host, port })
    }
    fn send_tcp(&self, id: u64, data: Vec<u8>, eof: bool) -> Result<(), TransportError> {
        self.send(TransportOp::SendTcp { id, data, eof })
    }
    fn send_udp(&self, id: u64, flow: u32, host: String, port: u16, data: Vec<u8>) -> Result<(), TransportError> {
        self.send(TransportOp::SendUdp { id, flow, host, port, data })
    }
    fn close(&self, id: u64) {
        let _ = self.ops_tx.send(TransportOp::Close { id });
    }
    fn send(&self, op: TransportOp) -> Result<(), TransportError> {
        self.ops_tx.send(op).map_err(|_| TransportError::DriverLost)
    }
}

pub struct Runtime {
    sessions: Mutex<HashMap<u64, SessionHandle>>,
    workers: Mutex<HashMap<u64, JoinHandle<()>>>,
    egress: Arc<EgressRouter>,
    poll_data: Mutex<Vec<u8>>,
    transport: Arc<Mutex<Option<TransportHandle>>>,
    transport_driver: Mutex<Option<JoinHandle<()>>>,
    transport_shutdown: Arc<AtomicBool>,
    next_session: AtomicU64,
    max_outstanding: usize,
    worker_tick: Duration,
}

#[derive(Clone)]
struct EgressRouter {
    tx: mpsc::Sender<Event>,
    rx: Arc<Mutex<mpsc::Receiver<Event>>>,
}

impl EgressRouter {
    fn new() -> Self {
        let (tx, rx) = mpsc::channel();
        Self { tx, rx: Arc::new(Mutex::new(rx)) }
    }
    fn send(&self, event: Event) {
        // Channels here are MPSC and unbounded. The bound lives upstream: each
        // session's Command queue is bounded, so at most queue-capacity events
        // can be in flight per session between ticks. The poller drains 1:1.
        let _ = self.tx.send(event);
    }
    fn try_recv(&self) -> Result<Event, mpsc::TryRecvError> {
        self.rx.lock().unwrap().try_recv()
    }
}

impl Runtime {
    pub fn new(max_outstanding: usize) -> Self {
        Self {
            sessions: Mutex::new(HashMap::new()),
            workers: Mutex::new(HashMap::new()),
            egress: Arc::new(EgressRouter::new()),
            poll_data: Mutex::new(Vec::new()),
            transport: Arc::new(Mutex::new(None)),
            transport_driver: Mutex::new(None),
            transport_shutdown: Arc::new(AtomicBool::new(false)),
            next_session: AtomicU64::new(1),
            max_outstanding: max_outstanding.max(1),
            worker_tick: Duration::from_millis(2),
        }
    }

    pub fn session(&self, id: u64) -> Option<SessionHandle> {
        self.sessions.lock().unwrap().get(&id).cloned()
    }

    pub fn open(&self, id: u64, policy: RestartPolicy) -> Result<(), SessionError> {
        if self.sessions.lock().unwrap().contains_key(&id) {
            return Err(SessionError::Invalid);
        }
        let session = RuntimeSession::new(id, policy);
        self.sessions.lock().unwrap().insert(id, session.clone());
        let worker = self.spawn_worker(session);
        self.workers.lock().unwrap().insert(id, worker);
        Ok(())
    }

    /// Close the session and stop its worker.
    pub fn close(&self, id: u64) -> Result<(), SessionError> {
        if let Some(transport) = self.transport.lock().unwrap().clone() {
            transport.close(id);
        }
        if let Some(session) = self.sessions.lock().unwrap().remove(&id) {
            session.close();
            // Worker exits because session.queue() sender is dropped once all
            // sessions stop referencing it; it also checks state each tick.
            let _ = session;
        }
        if let Some(worker) = self.workers.lock().unwrap().remove(&id) {
            let _ = worker.join();
        }
        Ok(())
    }

    /// Allocate a fresh session id (monotonic).
    pub fn allocate_id(&self) -> u64 {
        self.next_session.fetch_add(1, Ordering::Relaxed)
    }

    pub fn max_outstanding(&self) -> usize {
        self.max_outstanding
    }

    /// Non-blocking enqueue. `Id` alone would be ambiguous; we require the
    /// owner to pass the session id they hold.
    pub fn enqueue(&self, id: u64, command: Command) -> Result<(), SessionError> {
        let session = self.session(id).ok_or(SessionError::Invalid)?;
        match session.queue().try_push(command) {
            Ok(()) => Ok(()),
            Err(crate::queue::QueueError::Full) => Err(SessionError::QueueFull),
            Err(crate::queue::QueueError::Disconnected) => Err(SessionError::Disconnected),
        }
    }

    /// Poll one runtime→hook event. Returns Ok(true) if an event was seen.
    pub fn poll_events<F>(&self, mut callback: F) -> Result<bool, crate::queue::QueueError>
    where
        F: FnMut(u64, Event) -> bool,
    {
        let event = match self.egress.try_recv() {
            Ok(event) => event,
            Err(mpsc::TryRecvError::Empty) | Err(mpsc::TryRecvError::Disconnected) => {
                return Ok(false);
            }
        };
        let id = match event {
            Event::Status { id, .. } => id,
            Event::TcpData { id, .. } => id,
            Event::UdpData { id, .. } => id,
            Event::Error { id } => id,
        };
        callback(id, event);
        Ok(true)
    }

    /// Keep FFI event payload alive until it is replaced by a later poll.
    pub(crate) fn retain_poll_data(&self, data: Vec<u8>) -> (*const u8, usize) {
        let mut storage = self.poll_data.lock().unwrap();
        *storage = data;
        (storage.as_ptr(), storage.len())
    }

    /// Establish the shared QUIC transport (blocks until connected or failed)
    /// and start the driver thread that owns it. Idempotent: a second call
    /// while connected is a no-op.
    pub fn start_transport(&self, cfg: TransportConfig) -> Result<(), TransportError> {
        if self.transport.lock().unwrap().is_some() {
            return Ok(());
        }
        let transport = Transport::connect(cfg)?;
        let (ops_tx, ops_rx) = mpsc::channel();
        *self.transport.lock().unwrap() = Some(TransportHandle { ops_tx });

        let egress = self.egress.clone();
        let shutdown = self.transport_shutdown.clone();
        let tick = self.worker_tick;
        let handle = std::thread::Builder::new()
            .name("dr-xport-driver".into())
            .spawn(move || transport_driver(transport, ops_rx, egress, shutdown, tick))
            .map_err(TransportError::Io)?;
        *self.transport_driver.lock().unwrap() = Some(handle);
        Ok(())
    }

    /// True once a transport has been started and handed to its driver.
    pub fn has_transport(&self) -> bool {
        self.transport.lock().unwrap().is_some()
    }

    /// The worker thread pump. Exits when the session is closed or the command
    /// queue sender is gone (all clones dropped).
    fn spawn_worker(&self, session: SessionHandle) -> JoinHandle<()> {
        let egress = self.egress.clone();
        let tick = self.worker_tick;
        let transport = self.transport.clone();
        let queue = session.queue();
        std::thread::Builder::new()
            .name(format!("dr-session-{}", session.id()))
            .spawn(move || worker_pump(session, queue, egress, transport, tick))
            .expect("failed to spawn runtime worker")
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        // Stop the driver thread so dt_runtime_free never leaks a thread. Session
        // workers self-terminate once their queue sender is dropped with the
        // sessions map below.
        self.transport_shutdown.store(true, Ordering::Relaxed);
        if let Some(handle) = self.transport_driver.lock().unwrap().take() {
            let _ = handle.join();
        }
    }
}

/// Sole owner of the [`Transport`]: applies queued ops and forwards inbound
/// events into the egress router, where they reach the FFI poller.
fn transport_driver(
    transport: Transport,
    ops_rx: mpsc::Receiver<TransportOp>,
    egress: Arc<EgressRouter>,
    shutdown: Arc<AtomicBool>,
    tick: Duration,
) {
    const MAX_OPS_PER_TICK: usize = 64;
    const MAX_EVENTS_PER_TICK: usize = 64;
    loop {
        if shutdown.load(Ordering::Relaxed) {
            break;
        }
        let mut did_work = false;
        // Bound each side of the pump. A sustained video stream must not keep
        // inbound RTC discovery replies behind an unbounded batch of sends.
        for _ in 0..MAX_OPS_PER_TICK {
            let Ok(op) = ops_rx.try_recv() else { break };
            did_work = true;
            dispatch_op(&transport, op, &egress);
        }
        for _ in 0..MAX_EVENTS_PER_TICK {
            let Some(event) = transport.poll_events() else { break };
            did_work = true;
            egress.send(to_queue_event(event));
        }
        if !did_work {
            if !transport.is_connected() {
                break;
            }
            std::thread::sleep(tick);
        }
    }
}

fn dispatch_op(transport: &Transport, op: TransportOp, egress: &EgressRouter) {
    match op {
        TransportOp::OpenTcp { id, host, port } => {
            if transport.open_tcp(id, host, port).is_err() {
                egress.send(Event::Error { id });
            }
        }
        TransportOp::SendTcp { id, data, eof } => {
            if transport.send_tcp(id, data, eof).is_err() {
                egress.send(Event::Error { id });
            }
        }
        TransportOp::SendUdp { id, flow, host, port, data } => {
            // Fire-and-forget; the flow is registered inside send_udp so
            // inbound replies are routed back as UdpData events.
            let _ = transport.send_udp(id, flow, &host, port, data);
        }
        TransportOp::Close { id } => transport.close(id),
    }
}

fn to_queue_event(event: TransportEvent) -> Event {
    match event {
        TransportEvent::Status { id, status } => Event::Status { id, status },
        TransportEvent::TcpData { id, data, eof } => Event::TcpData { id, data, eof },
        TransportEvent::UdpData { id, flow, host, port, data } => {
            Event::UdpData { id, flow, host, port, data }
        }
        TransportEvent::Error { id } => Event::Error { id },
    }
}

fn worker_pump(
    session: SessionHandle,
    queue: Arc<BoundedQueue>,
    egress: Arc<EgressRouter>,
    transport: Arc<Mutex<Option<TransportHandle>>>,
    tick: Duration,
) {
    let q = queue;
    let mut last_poll = std::time::Instant::now();
    loop {
        // Exit conditions: explicit close, or the queue disconnected (all
        // senders dropped), or session marked closed by close().
        if session.state() == crate::session::SessionState::Closed {
            break;
        }
        match q.try_recv() {
            Ok(Command::OpenTcp { id, host, port }) => match transport.lock().unwrap().clone() {
                Some(tr) => {
                    if tr.open_tcp(id, host, port).is_err() {
                        egress.send(Event::Error { id });
                    }
                }
                None => {
                    // Loopback mode: pretend the tunnel opened.
                    egress.send(Event::Status { id, status: 200 });
                }
            },
            Ok(Command::SendTcp { id, data, eof }) => match transport.lock().unwrap().clone() {
                Some(tr) => {
                    if tr.send_tcp(id, data, eof).is_err() {
                        egress.send(Event::Error { id });
                    }
                }
                None => {
                    // Loopback mode: echo the bytes back as if from the server.
                    egress.send(Event::TcpData { id, data, eof });
                }
            },
            Ok(Command::OpenUdp { id }) => {
                egress.send(Event::Status { id, status: 200 });
            }
            Ok(Command::SendUdp { id, flow, host, port, data }) => {
                if let Some(tr) = transport.lock().unwrap().clone() {
                    // Fire-and-forget; the flow is registered inside send_udp
                    // so inbound replies are routed back as UdpData events.
                    let _ = tr.send_udp(id, flow, host, port, data);
                }
            }
            Ok(Command::Close { id }) => {
                if let Some(tr) = transport.lock().unwrap().clone() {
                    tr.close(id);
                }
                egress.send(Event::Status { id, status: 0 });
                let _ = id;
            }
            Ok(Command::Ping { nonce }) => {
                egress.send(Event::Status { id: 0, status: nonce as u16 });
            }
            Err(mpsc::TryRecvError::Empty) => {
                if last_poll.elapsed() >= tick {
                    last_poll = std::time::Instant::now();
                    // periodic housekeeping (reconnect logic lands with transport)
                }
                std::thread::sleep(tick);
            }
            Err(mpsc::TryRecvError::Disconnected) => break,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_enqueue_poll_close_roundtrip() {
        let runtime = Runtime::new(16);
        runtime.open(1, RestartPolicy::default_backoff()).unwrap();
        runtime.enqueue(1, Command::OpenTcp { id: 1, host: "example.com".into(), port: 443 }).unwrap();
        runtime.enqueue(1, Command::SendTcp { id: 1, data: b"hello".to_vec(), eof: false }).unwrap();

        let deadline = std::time::Instant::now() + Duration::from_secs(2);
        let mut saw_status = false;
        let mut saw_data = false;
        while std::time::Instant::now() < deadline && !(saw_status && saw_data) {
            runtime
                .poll_events(|_, event| {
                    match event {
                        Event::Status { .. } => saw_status = true,
                        Event::TcpData { .. } => saw_data = true,
                        Event::UdpData { .. } => {}
                        Event::Error { .. } => {}
                    }
                    true
                })
                .unwrap();
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(saw_status, "expected status event from OpenTcp");
        assert!(saw_data, "expected TcpData event from SendTcp");
        runtime.close(1).unwrap();
    }

    #[test]
    fn allocate_ids_monotonic() {
        let r = Runtime::new(4);
        let a = r.allocate_id();
        let b = r.allocate_id();
        assert!(b > a);
    }

    #[test]
    fn duplicate_open_rejected() {
        let r = Runtime::new(4);
        r.open(9, RestartPolicy::default_backoff()).unwrap();
        assert!(r.open(9, RestartPolicy::default_backoff()).is_err());
        r.close(9).unwrap();
    }
}
