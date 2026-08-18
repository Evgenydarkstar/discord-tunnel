//! HTTP/3 QUIC transport to the tunnel server (discord_http3.py).
//!
//! Blocking-friendly facade over an async (tokio + quinn + h3) stack.
//! Each worker pump thread drives one [`Transport`]; the transport owns the
//! async runtime and exposes synchronous, non-blocking entry points:
//!
//! - [`Transport::open_tcp`]: opens a CONNECT stream in the background; the
//!   resulting status and streamed payloads arrive as [`TransportEvent`]s.
//! - [`Transport::send_tcp`] / [`Transport::close`]: feed outbound bytes /
//!   half-close into a previously opened stream.
//! - [`Transport::send_udp`]: fire-and-forget QUIC datagram (synchronous).
//! - [`Transport::poll_events`]: drain one event.
//!
//! This module is independent of `queue`/`runtime`: events carry a session
//! `id` (the tunnel id passed to `open_tcp`), which the worker routes.

use std::collections::HashMap;
use std::fs::File;
use std::io::{self, BufReader};
use std::net::{SocketAddr, ToSocketAddrs};
use std::path::PathBuf;
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;

use bytes::{Buf, Bytes};
use h3::client::SendRequest;
use h3_quinn::OpenStreams;
use http::{Method, Request, StatusCode};
use quinn::{Connection, Endpoint, SendDatagramError};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use tokio::runtime::{Builder, Runtime};
use tokio::sync::mpsc::{unbounded_channel, UnboundedReceiver, UnboundedSender};
use tokio::time::timeout;

use crate::protocol::{
    encode_datagram, fragment_datagram, reassemble_datagram, Address, DatagramPacket,
    FragmentState, ProtocolError,
};

const AUTH_TIMEOUT: Duration = Duration::from_secs(10);
const STREAM_STATUS_TIMEOUT: Duration = Duration::from_secs(15);
const REALTIME_PAYLOAD_MAX: usize = 512;
const REALTIME_DATAGRAM_RESERVE: usize = 8 * 1024;
const DATAGRAM_SEND_BUFFER_SIZE: usize = 64 * 1024;

fn datagram_batch_fits(buffer_space: usize, wire_len: usize, realtime: bool) -> bool {
    realtime || buffer_space >= wire_len.saturating_add(REALTIME_DATAGRAM_RESERVE)
}

/// Endpoint configuration for a [`Transport`].
#[derive(Debug, Clone)]
pub struct TransportConfig {
    pub server_host: String,
    pub server_port: u16,
    pub token: String,
    /// Optional PEM bundle of trusted CA certificates.
    pub ca_pem: Option<PathBuf>,
    /// Skip TLS certificate verification entirely.
    pub insecure: bool,
}

impl TransportConfig {
    pub fn new(server_host: impl Into<String>, server_port: u16, token: impl Into<String>) -> Self {
        Self {
            server_host: server_host.into(),
            server_port,
            token: token.into(),
            ca_pem: None,
            insecure: false,
        }
    }
}

/// Errors surfaced by the transport layer.
#[derive(Debug)]
pub enum TransportError {
    Io(io::Error),   // dns / file / bind
    Resolve(String), // host not resolvable
    Connect(quinn::ConnectError),
    Connection(quinn::ConnectionError),
    H3Connection(h3::error::ConnectionError),
    H3Stream(h3::error::StreamError),
    Http(http::Error),
    Rustls(rustls::Error),
    QuicCrypto(quinn::crypto::rustls::NoInitialCipherSuite),
    Protocol(ProtocolError),
    Datagram(SendDatagramError),
    Timeout,
    Auth {
        status: u16,
    },
    StreamNotFound(u64),
    /// The owning driver thread is gone (channel closed).
    DriverLost,
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}

impl std::error::Error for TransportError {}

impl From<io::Error> for TransportError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}
impl From<quinn::ConnectError> for TransportError {
    fn from(e: quinn::ConnectError) -> Self {
        Self::Connect(e)
    }
}
impl From<quinn::ConnectionError> for TransportError {
    fn from(e: quinn::ConnectionError) -> Self {
        Self::Connection(e)
    }
}
impl From<h3::error::ConnectionError> for TransportError {
    fn from(e: h3::error::ConnectionError) -> Self {
        Self::H3Connection(e)
    }
}
impl From<h3::error::StreamError> for TransportError {
    fn from(e: h3::error::StreamError) -> Self {
        Self::H3Stream(e)
    }
}
impl From<http::Error> for TransportError {
    fn from(e: http::Error) -> Self {
        Self::Http(e)
    }
}
impl From<rustls::Error> for TransportError {
    fn from(e: rustls::Error) -> Self {
        Self::Rustls(e)
    }
}
impl From<ProtocolError> for TransportError {
    fn from(e: ProtocolError) -> Self {
        Self::Protocol(e)
    }
}
impl From<SendDatagramError> for TransportError {
    fn from(e: SendDatagramError) -> Self {
        Self::Datagram(e)
    }
}

/// Events flowing from the transport back to a worker thread.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TransportEvent {
    /// CONNECT reply for a stream (`id` = tunnel/session id).
    Status { id: u64, status: u16 },
    /// Payload received on a TCP stream. `eof` marks the read half close.
    TcpData { id: u64, data: Vec<u8>, eof: bool },
    /// Payload received on a UDP flow (`id` = session id).
    UdpData {
        id: u64,
        flow: u32,
        host: Address,
        port: u16,
        data: Vec<u8>,
    },
    /// Stream-level failure (connection closed, RST, cancelled).
    Error { id: u64 },
}

type StreamSender = UnboundedSender<(Bytes, bool)>;
type StreamReceiver = UnboundedReceiver<(Bytes, bool)>;
type H3SendRequest = SendRequest<OpenStreams, Bytes>;

/// A single HTTP/3 (QUIC) connection used to tunnel TCP streams and UDP relays.
///
/// Methods are safe to call from synchronous worker threads; internal async
/// work runs on the owned tokio runtime.
pub struct Transport {
    rt: Runtime,
    _endpoint: Endpoint, // keep the endpoint driver alive
    conn: Connection,
    send: H3SendRequest,
    cfg: Arc<TransportConfig>,
    streams: Arc<Mutex<HashMap<u64, StreamSender>>>,
    flows: Arc<Mutex<HashMap<u32, u64>>>, // udp flow -> session id
    events_tx: mpsc::Sender<TransportEvent>,
    events_rx: mpsc::Receiver<TransportEvent>,
    next_udp_message_id: std::sync::atomic::AtomicU32,
}

impl Transport {
    /// Establish the QUIC connection, negotiate HTTP/3 and authenticate.
    ///
    /// Blocks until connected (or a connect/auth error); afterwards all
    /// returned methods are non-blocking.
    pub fn connect(cfg: TransportConfig) -> Result<Self, TransportError> {
        let rt = Builder::new_multi_thread()
            .enable_all()
            .thread_name("dr-xport")
            .build()?;
        let cfg = Arc::new(cfg);

        let (events_tx, events_rx) = mpsc::channel();
        let streams: Arc<Mutex<HashMap<u64, StreamSender>>> = Default::default();
        let flows: Arc<Mutex<HashMap<u32, u64>>> = Default::default();
        let fragments: Arc<Mutex<FragmentState>> = Default::default();

        let (endpoint, conn, send) = rt.block_on(connect_async(cfg.clone(), events_tx.clone()))?;

        // Relay inbound UDP datagrams to the owning session.
        let (conn2, flows2, fragments2, events2) = (
            conn.clone(),
            flows.clone(),
            fragments.clone(),
            events_tx.clone(),
        );
        rt.spawn(async move {
            loop {
                match conn2.read_datagram().await {
                    Ok(datagram) => {
                        if let Ok(pkt) = crate::protocol::decode_datagram(&datagram) {
                            let Ok(Some(pkt)) =
                                reassemble_datagram(pkt, &mut fragments2.lock().unwrap())
                            else {
                                continue;
                            };
                            if let Some(&id) = flows2.lock().unwrap().get(&pkt.flow_id) {
                                let _ = events2.send(TransportEvent::UdpData {
                                    id,
                                    flow: pkt.flow_id,
                                    host: pkt.host,
                                    port: pkt.port,
                                    data: pkt.payload,
                                });
                            }
                        }
                    }
                    Err(..) => break,
                }
            }
        });

        Ok(Self {
            rt,
            _endpoint: endpoint,
            conn,
            send,
            cfg,
            streams,
            flows,
            events_tx,
            events_rx,
            next_udp_message_id: std::sync::atomic::AtomicU32::new(1),
        })
    }

    /// Open a TCP tunnel. `id` must be stable for the lifetime of the stream
    /// and is stamped onto all events returned for it.
    pub fn open_tcp(&self, id: u64, host: String, port: u16) -> Result<(), TransportError> {
        if self.streams.lock().unwrap().contains_key(&id) {
            return Ok(());
        }
        let (tx, rx) = unbounded_channel();
        self.streams.lock().unwrap().insert(id, tx);

        let (events, cfg, send) = (
            self.events_tx.clone(),
            self.cfg.clone(),
            self.send.clone(),
        );
        let streams = self.streams.clone();
        self.rt.spawn(async move {
            pump_tcp_h3(id, host, port, rx, events, streams, cfg, send).await;
        });
        Ok(())
    }

    /// Feed outbound bytes into an open stream. `eof` closes the write half
    /// (the server stops reading from this tunnel).
    pub fn send_tcp(&self, id: u64, data: Vec<u8>, eof: bool) -> Result<(), TransportError> {
        let tx = self
            .streams
            .lock()
            .unwrap()
            .get(&id)
            .cloned()
            .ok_or(TransportError::StreamNotFound(id))?;
        if tx.send((Bytes::from(data), eof)).is_err() {
            return Err(TransportError::StreamNotFound(id));
        }
        Ok(())
    }

    /// Drop the stream: the write half finishes and any buffered outbound data
    /// is flushed best-effort by the pump task.
    pub fn close(&self, id: u64) {
        self.streams.lock().unwrap().remove(&id);
        self.flows
            .lock()
            .unwrap()
            .retain(|_, session| *session != id);
    }

    /// Send a UDP relay packet as a QUIC datagram (synchronous).
    ///
    /// Registers `flow -> session id` so inbound datagrams can be routed back.
    pub fn send_udp(
        &self,
        id: u64,
        flow: u32,
        host: &str,
        port: u16,
        data: Vec<u8>,
    ) -> Result<(), TransportError> {
        self.flows.lock().unwrap().insert(flow, id);
        let host = match host.parse::<std::net::IpAddr>() {
            Ok(std::net::IpAddr::V4(ip)) => Address::Ipv4(ip.octets()),
            Ok(std::net::IpAddr::V6(ip)) => Address::Ipv6(ip.octets()),
            Err(_) => Address::Domain(host.to_string()),
        };
        let packet = DatagramPacket {
            flow_id: flow,
            host,
            port,
            payload: data,
        };
        let realtime = packet.payload.len() <= REALTIME_PAYLOAD_MAX;
        let message_id = self
            .next_udp_message_id
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let wires = fragment_datagram(&packet, message_id)?
            .into_iter()
            .map(|fragment| encode_datagram(&fragment).map(Bytes::from))
            .collect::<Result<Vec<_>, _>>()?;
        let wire_len = wires.iter().map(Bytes::len).sum();
        if !datagram_batch_fits(self.conn.datagram_send_buffer_space(), wire_len, realtime) {
            return Ok(());
        }
        for wire in wires {
            self.conn.send_datagram(wire)?;
        }
        Ok(())
    }

    /// Non-blocking drain of one pending event.
    pub fn poll_events(&self) -> Option<TransportEvent> {
        self.events_rx.try_recv().ok()
    }

    /// True until the QUIC connection is lost.
    pub fn is_connected(&self) -> bool {
        !matches!(self.conn.close_reason(), Some(_))
    }
}

async fn pump_tcp_h3(
    id: u64,
    host: String,
    port: u16,
    rx: StreamReceiver,
    events: mpsc::Sender<TransportEvent>,
    streams: Arc<Mutex<HashMap<u64, StreamSender>>>,
    cfg: Arc<TransportConfig>,
    mut send: H3SendRequest,
) {
    if run_tcp_h3(id, &host, port, rx, &events, &cfg, &mut send)
        .await
        .is_err()
    {
        let _ = events.send(TransportEvent::Error { id });
    }
    streams.lock().unwrap().remove(&id);
}

async fn run_tcp_h3(
    id: u64,
    host: &str,
    port: u16,
    mut rx: StreamReceiver,
    events: &mpsc::Sender<TransportEvent>,
    cfg: &TransportConfig,
    send: &mut H3SendRequest,
) -> Result<(), TransportError> {
    let request = build_request(Method::CONNECT, "/v1/tcp", Some((host, port)), cfg)?;
    let mut stream = send.send_request(request).await?;
    let response = timeout(STREAM_STATUS_TIMEOUT, stream.recv_response())
        .await
        .map_err(|_| TransportError::Timeout)??;
    let status = response.status().as_u16();
    let _ = events.send(TransportEvent::Status { id, status });
    if status != StatusCode::OK.as_u16() {
        return Ok(());
    }

    let (mut send_stream, mut recv_stream) = stream.split();
    let mut outbound_open = true;
    let mut inbound_open = true;
    loop {
        if !inbound_open && !outbound_open {
            return Ok(());
        }
        tokio::select! {
            outbound = rx.recv(), if outbound_open => {
                match outbound {
                    Some((data, eof)) => {
                        if !data.is_empty() {
                            send_stream.send_data(data).await?;
                        }
                        if eof {
                            send_stream.finish().await?;
                            outbound_open = false;
                        }
                    }
                    None => {
                        send_stream.finish().await?;
                        outbound_open = false;
                    }
                }
            }
            inbound = recv_stream.recv_data(), if inbound_open => {
                match inbound? {
                    Some(mut data) => {
                        let len = data.remaining();
                        let _ = events.send(TransportEvent::TcpData {
                            id,
                            data: data.copy_to_bytes(len).to_vec(),
                            eof: false,
                        });
                    }
                    None => {
                        inbound_open = false;
                        let _ = events.send(TransportEvent::TcpData {
                            id,
                            data: Vec::new(),
                            eof: true,
                        });
                    }
                }
            }
        }
    }
}

async fn connect_async(
    cfg: Arc<TransportConfig>,
    events: mpsc::Sender<TransportEvent>,
) -> Result<(Endpoint, Connection, H3SendRequest), TransportError> {
    // Resolve and pick a local bind address matching that family.
    let addr = resolve(&cfg)?;
    let bind: SocketAddr = if addr.is_ipv4() {
        "0.0.0.0:0".parse().unwrap()
    } else {
        "[::]:0".parse().unwrap()
    };

    let endpoint = Endpoint::client(bind)?;
    let client_config = build_quinn_client_config(&cfg)?;
    let conn = endpoint
        .connect_with(client_config, addr, &cfg.server_host)?
        .await?;

    // Hand the quinn connection to h3 and keep the request half.
    let (driver, send) = h3::client::new(h3_quinn::Connection::new(conn.clone())).await?;
    let handle = tokio::runtime::Handle::current();
    handle.spawn(async move {
        let mut driver = driver;
        let _ = std::future::poll_fn(|cx| driver.poll_close(cx)).await;
    });

    authenticate(&send, cfg.clone()).await?;
    let _ = events; // reserved for future connection-level events
    Ok((endpoint, conn, send))
}

fn resolve(cfg: &TransportConfig) -> Result<SocketAddr, TransportError> {
    (cfg.server_host.as_str(), cfg.server_port)
        .to_socket_addrs()
        .map_err(|_| TransportError::Resolve(format!("{}:{}", cfg.server_host, cfg.server_port)))?
        .next()
        .ok_or_else(|| TransportError::Resolve(format!("{}:{}", cfg.server_host, cfg.server_port)))
}

/// GET /v1/session with a Bearer token; returns an error unless 200.
async fn authenticate(
    send: &H3SendRequest,
    cfg: Arc<TransportConfig>,
) -> Result<(), TransportError> {
    let req = build_request(Method::GET, "/v1/session", None, &cfg)?;
    let mut send = send.clone();
    let mut stream = send.send_request(req).await?;
    let resp = timeout(AUTH_TIMEOUT, stream.recv_response())
        .await
        .map_err(|_| TransportError::Timeout)??;
    let status = resp.status().as_u16();
    if status != StatusCode::OK.as_u16() {
        return Err(TransportError::Auth { status });
    }
    while let Some(..) = stream.recv_data().await? {}
    Ok(())
}

fn build_request(
    method: Method,
    path: &str,
    target: Option<(&str, u16)>,
    cfg: &TransportConfig,
) -> Result<Request<()>, TransportError> {
    // Full URI with scheme+authority so h3 can derive the pseudo-headers.
    // Path is forwarded verbatim (already starts with "/"): trim double slashes.
    let path = if path.starts_with('/') {
        &path[1..]
    } else {
        path
    };
    let authority = if cfg.server_host.parse::<std::net::Ipv6Addr>().is_ok() {
        format!("[{}]", cfg.server_host)
    } else {
        cfg.server_host.clone()
    };
    let uri = format!("https://{}/{}", authority, path);
    let mut builder = Request::builder()
        .method(method)
        .uri(uri)
        .header("authorization", format!("Bearer {}", cfg.token));
    if let Some((host, port)) = target {
        builder = builder
            .header("x-target-host", host)
            .header("x-target-port", port.to_string());
    }
    Ok(builder.body(())?)
}

/// Skip-all TLS verifier used when `TransportConfig::insecure` is set.
#[derive(Debug)]
struct NoCertificateVerification(Arc<rustls::crypto::CryptoProvider>);

impl ServerCertVerifier for NoCertificateVerification {
    fn verify_server_cert(
        &self,
        _: &CertificateDer<'_>,
        _: &[CertificateDer<'_>],
        _: &ServerName<'_>,
        _: &[u8],
        _: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

/// Build the rustls client config: custom CA bundle if given, else native
/// roots (or nothing when `insecure`).
fn build_tls(
    cfg: &TransportConfig,
    alpn_protocols: Vec<Vec<u8>>,
) -> Result<rustls::ClientConfig, TransportError> {
    let mut roots = rustls::RootCertStore::empty();
    if let Some(ca) = &cfg.ca_pem {
        let mut reader = BufReader::new(File::open(ca)?);
        let certs = rustls_pemfile::certs(&mut reader).collect::<Result<Vec<_>, _>>()?;
        roots.add_parsable_certificates(certs);
    } else if !cfg.insecure {
        roots.add_parsable_certificates(rustls_native_certs::load_native_certs().certs);
    }

    let mut config = rustls::ClientConfig::builder()
        .with_root_certificates(roots)
        .with_no_client_auth();
    config.alpn_protocols = alpn_protocols;
    if cfg.insecure {
        config
            .dangerous()
            .set_certificate_verifier(Arc::new(NoCertificateVerification(Arc::new(
                rustls::crypto::ring::default_provider(),
            ))));
    }
    Ok(config)
}

fn build_quinn_client_config(cfg: &TransportConfig) -> Result<quinn::ClientConfig, TransportError> {
    let tls = build_tls(cfg, vec![b"h3".to_vec()])?;
    let crypto = quinn::crypto::rustls::QuicClientConfig::try_from(tls)
        .map_err(TransportError::QuicCrypto)?;
    let mut config = quinn::ClientConfig::new(Arc::new(crypto));
    let mut transport_cfg = quinn::TransportConfig::default();
    // PINGs keep the connection alive across idle periods; without this a
    // quiet tunnel is torn down by the client-side idle timeout mid-flight.
    transport_cfg.keep_alive_interval(Some(Duration::from_secs(10)));
    transport_cfg.max_idle_timeout(Some(Duration::from_secs(30).try_into().unwrap()));
    // Video can briefly outpace the uplink. Quinn drops older datagrams
    // when this buffer fills, which can otherwise discard RTC discovery and
    // make the next call fail until Discord restarts.
    // Keep latency bounded and reserve room for voice/RTC packets. Video is
    // unreliable and may be dropped under congestion rather than queueing for
    // seconds and turning the voice RTT into Discord's 5000 ms sentinel.
    transport_cfg.datagram_send_buffer_size(DATAGRAM_SEND_BUFFER_SIZE);
    config.transport_config(Arc::new(transport_cfg));
    Ok(config)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_cfg() -> TransportConfig {
        TransportConfig::new("127.0.0.1", 4433, "test-token")
    }

    #[test]
    fn request_headers_include_auth_and_target() {
        let cfg = test_cfg();
        let req =
            build_request(Method::CONNECT, "/v1/tcp", Some(("discord.com", 443)), &cfg).unwrap();
        assert_eq!(req.method(), Method::CONNECT);
        assert_eq!(
            req.headers().get("authorization").unwrap(),
            "Bearer test-token"
        );
        assert_eq!(req.headers().get("x-target-host").unwrap(), "discord.com");
        assert_eq!(req.headers().get("x-target-port").unwrap(), "443");
        // Authority present so h3 can build pseudo-headers.
        assert!(req.uri().authority().is_some());
    }

    #[test]
    fn session_request_has_no_target() {
        let cfg = test_cfg();
        let req = build_request(Method::GET, "/v1/session", None, &cfg).unwrap();
        assert_eq!(req.method(), Method::GET);
        assert!(req.headers().get("x-target-host").is_none());
        assert_eq!(req.uri().path(), "/v1/session");
    }

    #[test]
    fn invalid_host_is_a_resolve_error() {
        let mut cfg = test_cfg();
        cfg.server_host = "\u{0000}invalid".into();
        assert!(matches!(resolve(&cfg), Err(TransportError::Resolve(_))));
    }

    #[test]
    fn fragmented_video_batch_is_never_partially_admitted() {
        let first_fragment = 1150;
        let whole_packet = first_fragment * 3;
        let space_for_only_one_fragment = REALTIME_DATAGRAM_RESERVE + first_fragment;

        assert!(!datagram_batch_fits(
            space_for_only_one_fragment,
            whole_packet,
            false
        ));
        assert!(datagram_batch_fits(
            REALTIME_DATAGRAM_RESERVE + whole_packet,
            whole_packet,
            false
        ));
        assert!(datagram_batch_fits(0, whole_packet, true));
    }
}
