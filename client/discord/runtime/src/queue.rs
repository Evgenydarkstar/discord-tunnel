//! Bounded MPSC queue between hook→runtime and runtime→hook.

use std::sync::mpsc::{self, RecvTimeoutError, SyncSender, TryRecvError, TrySendError};

use crate::protocol::Address;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    OpenTcp { id: u64, host: String, port: u16 },
    SendTcp { id: u64, data: Vec<u8>, eof: bool },
    OpenUdp { id: u64 },
    SendUdp { id: u64, flow: u32, host: String, port: u16, data: Vec<u8> },
    Close { id: u64 },
    Ping { nonce: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    Status { id: u64, status: u16 },
    TcpData { id: u64, data: Vec<u8>, eof: bool },
    /// Payload received on a UDP flow (`flow` identifies the socket that sent).
    UdpData { id: u64, flow: u32, host: Address, port: u16, data: Vec<u8> },
    Error { id: u64 },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueueError {
    Full,
    Disconnected,
}

pub struct BoundedQueue {
    sender: SyncSender<Command>,
    receiver: std::sync::Mutex<std::sync::mpsc::Receiver<Command>>,
}

const DEFAULT_CAPACITY: usize = 1024;

impl BoundedQueue {
    pub fn new() -> Self {
        Self::with_capacity(DEFAULT_CAPACITY)
    }

    pub fn with_capacity(capacity: usize) -> Self {
        let (sender, receiver) = mpsc::sync_channel(capacity.max(1));
        Self { sender, receiver: std::sync::Mutex::new(receiver) }
    }

    pub fn sender(&self) -> SyncSender<Command> {
        self.sender.clone()
    }

    /// Non-blocking push from hooks: never wait on the runtime.
    pub fn try_push(&self, command: Command) -> Result<(), QueueError> {
        match self.sender.try_send(command) {
            Ok(()) => Ok(()),
            Err(TrySendError::Full(_)) => Err(QueueError::Full),
            Err(TrySendError::Disconnected(_)) => Err(QueueError::Disconnected),
        }
    }

    pub fn try_recv(&self) -> Result<Command, TryRecvError> {
        self.receiver.lock().unwrap().try_recv()
    }

    pub fn recv_timeout(&self, timeout: std::time::Duration) -> Result<Command, RecvTimeoutError> {
        self.receiver.lock().unwrap().recv_timeout(timeout)
    }
}

impl std::fmt::Debug for BoundedQueue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("BoundedQueue").finish_non_exhaustive()
    }
}

impl Default for BoundedQueue {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn push_and_recv() {
        let queue = BoundedQueue::with_capacity(4);
        queue.try_push(Command::Ping { nonce: 7 }).unwrap();
        queue.try_push(Command::Close { id: 3 }).unwrap();
        let first = queue.try_recv().unwrap();
        assert_eq!(first, Command::Ping { nonce: 7 });
        let second = queue.try_recv().unwrap();
        assert_eq!(second, Command::Close { id: 3 });
        assert!(matches!(queue.try_recv(), Err(TryRecvError::Empty)));
    }

    #[test]
    fn bounded_full() {
        let queue = BoundedQueue::with_capacity(2);
        assert!(queue.try_push(Command::Ping { nonce: 0 }).is_ok());
        assert!(queue.try_push(Command::Ping { nonce: 1 }).is_ok());
        let first = queue.try_recv().unwrap();
        assert_eq!(first, Command::Ping { nonce: 0 });
        assert!(queue.try_push(Command::Ping { nonce: 2 }).is_ok());
        assert!(matches!(queue.try_push(Command::Ping { nonce: 3 }), Err(QueueError::Full)));
    }

    #[test]
    fn blocking_recv_respects_timeout() {
        let queue = BoundedQueue::new();
        let result = queue.recv_timeout(std::time::Duration::from_millis(5));
        assert!(matches!(result, Err(RecvTimeoutError::Timeout)));
    }
}
