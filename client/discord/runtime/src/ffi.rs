//! C ABI for the in-DLL runtime, usable from C, C++ (the Discord Tunnel hook), and
//! Python ctypes. Every entry point is `catch_unwind`-wrapped so a panic in
//! the runtime degrades to an error code instead of unwinding across the
//! extern boundary (UB for C callers and fatal for Python hooks).

use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;

use crate::queue::Command;
use crate::protocol::Address;
use crate::runtime::Runtime;
use crate::session::{RestartPolicy, SessionError};
use crate::transport::{TransportConfig, TransportError};

pub const DT_OK: c_int = 0;
pub const DT_ERR_QUEUE_FULL: c_int = -1;
pub const DT_ERR_DISCONNECTED: c_int = -2;
pub const DT_ERR_STALE_EPOCH: c_int = -3;
pub const DT_ERR_PANIC: c_int = -4;
pub const DT_ERR_INVALID: c_int = -5;
pub const DT_ERR_TRANSPORT: c_int = -6;
pub const DT_ERR_TRANSPORT_AUTH: c_int = -7;

// Opaque handle: a caller-owned raw pointer into a heap box. C passes it
// back to every subsequent call; null means "not initialized".
pub type RuntimeHandle = Runtime;

#[no_mangle]
pub extern "C" fn dt_runtime_new(max_outstanding: usize) -> *mut RuntimeHandle {
    let result = catch_unwind(AssertUnwindSafe(|| -> *mut RuntimeHandle {
        let runtime = Runtime::new(max_outstanding);
        Box::into_raw(Box::new(runtime))
    }));
    match result {
        Ok(handle) => handle,
        Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn dt_runtime_free(handle: *mut RuntimeHandle) {
    if handle.is_null() {
        return;
    }
    catch_unwind(AssertUnwindSafe(|| {
        unsafe { drop(Box::from_raw(handle)) };
    }))
    .unwrap_or(());
}

#[no_mangle]
pub extern "C" fn dt_session_open(handle: *mut RuntimeHandle, id: u64) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    match runtime.open(id, RestartPolicy::default_backoff()) {
        Ok(()) => DT_OK,
        Err(e) => err_code(e),
    }
}

#[no_mangle]
pub extern "C" fn dt_session_close(handle: *mut RuntimeHandle, id: u64) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    match runtime.close(id) {
        Ok(()) => DT_OK,
        Err(e) => err_code(e),
    }
}

#[no_mangle]
pub extern "C" fn dt_enqueue_tcp(handle: *mut RuntimeHandle, id: u64, host: *const c_char, port: u16) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    let host = unsafe { cstr_to_string(host) };
    match runtime.enqueue(id, Command::OpenTcp { id, host, port }) {
        Ok(()) => DT_OK,
        Err(e) => err_code(e),
    }
}

#[no_mangle]
pub extern "C" fn dt_send_tcp(handle: *mut RuntimeHandle, id: u64, data: *const u8, len: usize, eof: c_int) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    let data = match unsafe { copy_data(data, len) } {
        Some(data) => data,
        None => return DT_ERR_INVALID,
    };
    match runtime.enqueue(id, Command::SendTcp { id, data, eof: eof != 0 }) {
        Ok(()) => DT_OK,
        Err(e) => err_code(e),
    }
}

#[no_mangle]
pub extern "C" fn dt_send_udp(handle: *mut RuntimeHandle, id: u64, flow: u32, host: *const c_char, port: u16, data: *const u8, len: usize) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    let host = unsafe { cstr_to_string(host) };
    let data = match unsafe { copy_data(data, len) } {
        Some(data) => data,
        None => return DT_ERR_INVALID,
    };
    match runtime.enqueue(id, Command::SendUdp { id, flow, host, port, data }) {
        Ok(()) => DT_OK,
        Err(e) => err_code(e),
    }
}

/// Non-blocking poll for runtime→hook events (TCP data / errors).
/// Returns `DT_OK` and fills `*out` on a fresh event, `DT_EVENT_EMPTY` otherwise.
/// Caller frees nothing; `Event.ptr` points into runtime-owned storage valid until
/// the next poll on the same session.
pub const DT_EVENT_EMPTY: c_int = 1;

#[repr(C)]
pub struct EventOut {
    pub session: u64,
    pub kind: u8, // 0=status, 1=tcp_data, 2=error, 3=udp_data
    pub status: u16,
    /// UDP flow id (kind=3 only); ignored otherwise.
    pub flow: u32,
    pub ptr: *const u8,
    pub len: usize,
    pub eof: u8,
    /// RFC 1928 address type and bytes for UDP source endpoints.
    pub source_atyp: u8,
    pub source_addr_len: u16,
    pub source_port: u16,
    pub source_addr: [u8; 255],
}

/// Connect the runtime to the tunnel server. `ca_pem` is an optional path to
/// a PEM bundle; pass NULL (or "") with `insecure=1` to skip TLS verification.
/// Returns `DT_OK` once the QUIC connection is established and authenticated,
/// `DT_ERR_TRANSPORT_AUTH` on bad credentials, `DT_ERR_TRANSPORT` on any other
/// connect/resolve/TLS failure. This call blocks for up to ~10s.
#[no_mangle]
pub extern "C" fn dt_transport_start(
    handle: *mut RuntimeHandle,
    server_host: *const c_char,
    server_port: u16,
    token: *const c_char,
    ca_pem: *const c_char,
    insecure: c_int,
) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    let cfg = TransportConfig {
        server_host: unsafe { cstr_to_string(server_host) },
        server_port,
        token: unsafe { cstr_to_string(token) },
        ca_pem: {
            let ca = unsafe { cstr_to_string(ca_pem) };
            if ca.is_empty() { None } else { Some(PathBuf::from(ca)) }
        },
        insecure: insecure != 0,
    };
    let result = catch_unwind(AssertUnwindSafe(|| runtime.start_transport(cfg)));
    match result {
        Ok(Ok(())) => DT_OK,
        Ok(Err(TransportError::Auth { .. })) => DT_ERR_TRANSPORT_AUTH,
        Ok(Err(_)) => DT_ERR_TRANSPORT,
        Err(_) => DT_ERR_PANIC,
    }
}

/// Start the process-lifetime loopback bridge and its authenticated
/// transport. Safe to call repeatedly; later calls return success once started.
#[no_mangle]
pub extern "C" fn dt_embedded_start(
    server_host: *const c_char,
    server_port: u16,
    token: *const c_char,
    ca_pem: *const c_char,
    insecure: c_int,
    listen_port: u16,
) -> c_int {
    let cfg = (
        unsafe { cstr_to_string(server_host) },
        unsafe { cstr_to_string(token) },
        unsafe { cstr_to_string(ca_pem) },
    );
    let result = catch_unwind(AssertUnwindSafe(|| {
        crate::embedded::start(
            cfg.0,
            server_port,
            cfg.1,
            if cfg.2.is_empty() { None } else { Some(PathBuf::from(cfg.2)) },
            insecure != 0,
            listen_port,
        )
    }));
    match result {
        Ok(Ok(())) => DT_OK,
        Ok(Err(crate::embedded::StartError::Transport(TransportError::Auth { .. }))) => {
            DT_ERR_TRANSPORT_AUTH
        }
        Ok(Err(crate::embedded::StartError::Transport(_))) => DT_ERR_TRANSPORT,
        Ok(Err(crate::embedded::StartError::Io)) => DT_ERR_INVALID,
        Err(_) => DT_ERR_PANIC,
    }
}

#[no_mangle]
pub extern "C" fn dt_bridge_is_ready() -> c_int {
    if crate::embedded::is_ready() { 1 } else { 0 }
}

#[no_mangle]
pub extern "C" fn dt_bridge_tcp_open(
    host: *const c_char,
    port: u16,
    out_bridge_id: *mut u64,
    out_loopback_port: *mut u16,
) -> c_int {
    open_bridge(host, port, out_bridge_id, out_loopback_port, true)
}

#[no_mangle]
pub extern "C" fn dt_bridge_udp_open(
    host: *const c_char,
    port: u16,
    out_bridge_id: *mut u64,
    out_loopback_port: *mut u16,
) -> c_int {
    open_bridge(host, port, out_bridge_id, out_loopback_port, false)
}

#[no_mangle]
pub extern "C" fn dt_bridge_close(bridge_id: u64) -> c_int {
    let result = catch_unwind(AssertUnwindSafe(|| crate::embedded::close_bridge(bridge_id)));
    match result {
        Ok(Ok(())) => DT_OK,
        Ok(Err(error)) => bridge_error_code(&error),
        Err(_) => DT_ERR_PANIC,
    }
}

fn open_bridge(
    host: *const c_char,
    port: u16,
    out_bridge_id: *mut u64,
    out_loopback_port: *mut u16,
    tcp: bool,
) -> c_int {
    if out_bridge_id.is_null() || out_loopback_port.is_null() {
        return DT_ERR_INVALID;
    }
    let host = unsafe { cstr_to_string(host) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        if tcp {
            crate::embedded::open_tcp_bridge(host, port)
        } else {
            crate::embedded::open_udp_bridge(host, port)
        }
    }));
    match result {
        Ok(Ok((bridge_id, loopback_port))) => {
            unsafe {
                *out_bridge_id = bridge_id;
                *out_loopback_port = loopback_port;
            }
            DT_OK
        }
        Ok(Err(error)) => bridge_error_code(&error),
        Err(_) => DT_ERR_PANIC,
    }
}

fn bridge_error_code(error: &std::io::Error) -> c_int {
    match error.kind() {
        std::io::ErrorKind::InvalidInput | std::io::ErrorKind::NotFound => DT_ERR_INVALID,
        std::io::ErrorKind::NotConnected => DT_ERR_DISCONNECTED,
        std::io::ErrorKind::ConnectionRefused => DT_ERR_TRANSPORT,
        _ => DT_ERR_TRANSPORT,
    }
}

#[no_mangle]
pub extern "C" fn dt_poll(handle: *mut RuntimeHandle, out: *mut EventOut) -> c_int {
    let runtime = match safe_runtime(handle) {
        Some(r) => r,
        None => return DT_ERR_INVALID,
    };
    if out.is_null() {
        return DT_ERR_INVALID;
    }
    // SAFETY: `out` must point to at least one initialized EventOut; the C
    // caller allocates it. Single poller per runtime is an ABI contract.
    let result = catch_unwind(AssertUnwindSafe(|| unsafe {
        runtime.poll_events(|_session_id, event| {
            clear_source(out);
            match event {
                crate::queue::Event::Status { id, status } => {
                    (*out).session = id;
                    (*out).kind = 0;
                    (*out).status = status;
                    (*out).flow = 0;
                    (*out).ptr = std::ptr::null();
                    (*out).len = 0;
                    (*out).eof = 0;
                }
                crate::queue::Event::TcpData { id, data, eof } => {
                    let (ptr, len) = runtime.retain_poll_data(data);
                    (*out).session = id;
                    (*out).kind = 1;
                    (*out).status = 0;
                    (*out).flow = 0;
                    (*out).ptr = ptr;
                    (*out).len = len;
                    (*out).eof = if eof { 1 } else { 0 };
                }
                crate::queue::Event::UdpData { id, flow, host, port, data } => {
                    let (ptr, len) = runtime.retain_poll_data(data);
                    (*out).session = id;
                    (*out).kind = 3;
                    (*out).status = 0;
                    (*out).flow = flow;
                    (*out).ptr = ptr;
                    (*out).len = len;
                    (*out).eof = 0;
                    set_source(out, &host, port);
                }
                crate::queue::Event::Error { id } => {
                    (*out).session = id;
                    (*out).kind = 2;
                    (*out).status = 0;
                    (*out).flow = 0;
                    (*out).ptr = std::ptr::null();
                    (*out).len = 0;
                    (*out).eof = 0;
                }
            }
            true
        })
    }));
    match result {
        Ok(Ok(true)) => DT_OK,
        Ok(Ok(false)) => DT_EVENT_EMPTY,
        Ok(Err(_)) => DT_ERR_INVALID,
        Err(_) => DT_ERR_PANIC,
    }
}

fn err_code(err: SessionError) -> c_int {
    match err {
        SessionError::Stale => DT_ERR_STALE_EPOCH,
        SessionError::QueueFull => DT_ERR_QUEUE_FULL,
        SessionError::Disconnected => DT_ERR_DISCONNECTED,
        SessionError::Invalid => DT_ERR_INVALID,
        SessionError::Network => DT_ERR_PANIC, // placeholder mapping
    }
}

unsafe fn set_source(out: *mut EventOut, host: &Address, port: u16) {
    (*out).source_addr = [0; 255];
    (*out).source_port = port;
    let bytes: &[u8] = match host {
        Address::Ipv4(value) => {
            (*out).source_atyp = 1;
            value
        }
        Address::Ipv6(value) => {
            (*out).source_atyp = 4;
            value
        }
        Address::Domain(value) => {
            (*out).source_atyp = 3;
            value.as_bytes()
        }
    };
    let len = bytes.len().min((*out).source_addr.len());
    (&mut (*out).source_addr)[..len].copy_from_slice(&bytes[..len]);
    (*out).source_addr_len = len as u16;
}

unsafe fn clear_source(out: *mut EventOut) {
    (*out).source_atyp = 0;
    (*out).source_addr_len = 0;
    (*out).source_port = 0;
    (*out).source_addr = [0; 255];
}

// keep `c_void` referenced for the ABI docs
fn safe_runtime(handle: *mut RuntimeHandle) -> Option<&'static Runtime> {
    if handle.is_null() {
        return None;
    }
    // SAFETY: the raw pointer is dereferenced only after a null check, and the
    // caller contract ensures the handle lives for the whole call duration.
    Some(unsafe { &*handle })
}

#[allow(dead_code)]
unsafe fn cstr_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let mut len = 0usize;
    while *ptr.add(len) != 0 {
        len += 1;
    }
    let slice = std::slice::from_raw_parts(ptr as *const u8, len);
    String::from_utf8_lossy(slice).into_owned()
}

unsafe fn copy_data(ptr: *const u8, len: usize) -> Option<Vec<u8>> {
    if len == 0 {
        return Some(Vec::new());
    }
    if ptr.is_null() {
        return None;
    }
    Some(std::slice::from_raw_parts(ptr, len).to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn new_and_free_handle() {
        let h = dt_runtime_new(64);
        assert!(!h.is_null());
        dt_runtime_free(h);
    }

    #[test]
    fn null_handle_returns_invalid() {
        assert_eq!(dt_session_open(std::ptr::null_mut(), 1), DT_ERR_INVALID);
    }

    #[test]
    fn bridge_ffi_rejects_invalid_outputs_and_targets() {
        let host = b"discord.com\0";
        let mut id = 0;
        let mut port = 0;
        assert_eq!(
            dt_bridge_tcp_open(
                host.as_ptr() as *const c_char,
                443,
                std::ptr::null_mut(),
                &mut port,
            ),
            DT_ERR_INVALID
        );
        assert_eq!(
            dt_bridge_udp_open(std::ptr::null(), 0, &mut id, &mut port),
            DT_ERR_INVALID
        );
    }

    #[test]
    fn transport_start_null_host_is_transport_error() {
        let h = dt_runtime_new(8);
        assert!(!h.is_null());
        let token = b"t\0";
        assert_eq!(
            dt_transport_start(h, std::ptr::null(), 8443, token.as_ptr() as *const c_char, std::ptr::null(), 1),
            DT_ERR_TRANSPORT
        );
        dt_runtime_free(h);
    }

    #[test]
    fn polled_payload_remains_valid_after_return() {
        let h = dt_runtime_new(8);
        assert!(!h.is_null());
        assert_eq!(dt_session_open(h, 1), DT_OK);
        let payload = b"payload";
        assert_eq!(dt_send_tcp(h, 1, payload.as_ptr(), payload.len(), 0), DT_OK);

        let mut out = EventOut {
            session: 0,
            kind: 0,
            status: 0,
            flow: 0,
            ptr: std::ptr::null(),
            len: 0,
            eof: 0,
            source_atyp: 0,
            source_addr_len: 0,
            source_port: 0,
            source_addr: [0; 255],
        };
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        while dt_poll(h, &mut out) == DT_EVENT_EMPTY {
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(std::time::Duration::from_millis(10));
        }

        assert_eq!(out.kind, 1);
        assert_eq!(unsafe { std::slice::from_raw_parts(out.ptr, out.len) }, payload);
        dt_runtime_free(h);
    }

    #[test]
    fn data_pointer_may_be_null_only_when_empty() {
        let h = dt_runtime_new(8);
        assert_eq!(dt_session_open(h, 1), DT_OK);
        assert_eq!(dt_send_tcp(h, 1, std::ptr::null(), 1, 0), DT_ERR_INVALID);
        assert_eq!(dt_send_udp(h, 1, 1, std::ptr::null(), 53, std::ptr::null(), 1), DT_ERR_INVALID);
        assert_eq!(dt_send_tcp(h, 1, std::ptr::null(), 0, 1), DT_OK);
        assert_eq!(dt_send_udp(h, 1, 1, std::ptr::null(), 53, std::ptr::null(), 0), DT_OK);
        dt_runtime_free(h);
    }

    #[test]
    fn ffi_udp_source_preserves_endpoint() {
        let mut out = EventOut {
            session: 0,
            kind: 0,
            status: 0,
            flow: 0,
            ptr: std::ptr::null(),
            len: 0,
            eof: 0,
            source_atyp: 0,
            source_addr_len: 0,
            source_port: 0,
            source_addr: [0; 255],
        };
        unsafe { set_source(&mut out, &Address::Domain("relay.example".into()), 5353) };
        assert_eq!(out.source_atyp, 3);
        assert_eq!(out.source_port, 5353);
        assert_eq!(
            &out.source_addr[..out.source_addr_len as usize],
            b"relay.example"
        );
    }
}
