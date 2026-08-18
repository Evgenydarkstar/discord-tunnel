//! Session with generation counter and restart/backoff.
//!
//! Everything the hooks touch lives behind two gates:
//!   * a bounded `Command` queue (never blocks the caller);
//!   * an epoch counter — a stale/restarting session is detected by the
//!     generation number and the caller observes `SessionError::Stale`
//!     instead of touching deallocated state.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::queue::BoundedQueue;

pub type SessionId = u64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionError {
    Stale,
    QueueFull,
    Disconnected,
    Invalid,
    Network,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    Connecting,
    Ready,
    Restarting,
    Closed,
}

#[derive(Debug, Clone)]
pub struct RestartPolicy {
    pub base_delay: Duration,
    pub max_delay: Duration,
    pub factor: f64,
}

impl RestartPolicy {
    pub fn default_backoff() -> Self {
        Self {
            base_delay: Duration::from_secs(1),
            max_delay: Duration::from_secs(60),
            factor: 2.0,
        }
    }
}

#[derive(Debug)]
pub struct RuntimeSession {
    id: SessionId,
    epoch: Arc<AtomicU64>,
    state: Arc<std::sync::atomic::AtomicU8>,
    retry: Arc<std::sync::Mutex<RetryState>>,
    queue: Arc<BoundedQueue>,
    policy: RestartPolicy,
}

#[derive(Debug)]
struct RetryState {
    attempt: usize,
    next_attempt_at: Option<Instant>,
    generation: u64,
}

impl RuntimeSession {
    pub fn new(id: SessionId, policy: RestartPolicy) -> Arc<Self> {
        let session = Self {
            id,
            epoch: Arc::new(AtomicU64::new(0)),
            state: Arc::new(std::sync::atomic::AtomicU8::new(SessionState::Connecting as u8)),
            retry: Arc::new(std::sync::Mutex::new(RetryState {
                attempt: 0,
                next_attempt_at: None,
                generation: 0,
            })),
            queue: Arc::new(BoundedQueue::new()),
            policy,
        };
        Arc::new(session)
    }

    pub fn id(&self) -> SessionId {
        self.id
    }

    pub fn epoch(&self) -> u64 {
        self.epoch.load(Ordering::SeqCst)
    }

    pub fn state(&self) -> SessionState {
        self.state.load(Ordering::SeqCst).into()
    }

    pub fn queue(&self) -> Arc<BoundedQueue> {
        self.queue.clone()
    }

    /// Signals a fatal error: bump epoch, transition to Restarting so every
    /// live hook call discovers `Stale` on the next attempt and stops
    /// referencing old session-owned memory.
pub fn signal_fault(&self) -> SessionError {
        self.epoch.fetch_add(1, Ordering::SeqCst);
        self.state.store(SessionState::Restarting as u8, Ordering::SeqCst);
        SessionError::Stale
    }

    /// Called once the new transport session (generation `g`) is established.
    pub fn mark_ready(&self, generation: u64) {
        self.retry.lock().unwrap().generation = generation;
        self.state.store(SessionState::Ready as u8, Ordering::SeqCst);
    }

    /// Returns `Some` when the backoff permits a fresh connect attempt.
    pub fn due_for_restart(&self) -> bool {
        let now = Instant::now();
        let retry = self.retry.lock().unwrap();
        match retry.next_attempt_at {
            Some(at) => now >= at,
            None => true,
        }
    }

    /// Records a failed attempt and schedules the next one per policy.
    pub fn register_retry(&self, failed: bool) -> Duration {
        let mut retry = self.retry.lock().unwrap();
        if failed {
            retry.attempt += 1;
        }
        let attempt = retry.attempt;
        let delay = self
            .policy
            .base_delay
            .mul_f32(self.policy.factor.powi(attempt as i32) as f32)
            .min(self.policy.max_delay);
        if failed {
            retry.next_attempt_at = Some(Instant::now() + delay);
        } else {
            retry.attempt = 0;
            retry.next_attempt_at = None;
        }
        delay
    }

    /// Reset backoff after a successful connection.
    pub fn reset_backoff(&self) {
        let mut retry = self.retry.lock().unwrap();
        retry.attempt = 0;
        retry.next_attempt_at = None;
    }

    pub fn close(&self) {
        self.state.store(SessionState::Closed as u8, Ordering::SeqCst);
    }
}

impl From<u8> for SessionState {
    fn from(value: u8) -> Self {
        match value {
            x if x == SessionState::Ready as u8 => SessionState::Ready,
            x if x == SessionState::Restarting as u8 => SessionState::Restarting,
            x if x == SessionState::Closed as u8 => SessionState::Closed,
            _ => SessionState::Connecting,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::queue::Command;

    #[test]
    fn epoch_bumps_on_fatal_error() {
        let session = RuntimeSession::new(1, RestartPolicy::default_backoff());
        assert_eq!(session.epoch(), 0);
        session.signal_fault();
        assert_eq!(session.epoch(), 1);
        session.signal_fault();
        assert_eq!(session.epoch(), 2);
        assert_eq!(session.state(), SessionState::Restarting);
    }

    #[test]
    fn backoff_grows_exponentially_and_resets() {
        let s = RuntimeSession::new(1, RestartPolicy {
            base_delay: Duration::from_millis(100),
            max_delay: Duration::from_secs(10),
            factor: 2.0,
        });
        let d1 = s.register_retry(true);
        let d2 = s.register_retry(true);
        let d3 = s.register_retry(true);
        assert!(d1 < d2);
        assert!(d2 < d3);
        assert!(d3 <= Duration::from_secs(10));
        assert!(!s.due_for_restart()); // still in backoff
        s.reset_backoff();
        assert!(s.due_for_restart());
        let d_ok = s.register_retry(false);
        assert_eq!(d_ok, s.policy.base_delay); // backoff reset on success
    }

    #[test]
    fn command_queue_is_bounded() {
        let s = RuntimeSession::new(1, RestartPolicy::default_backoff());
        for _ in 0..1024 {
            assert!(s.queue.try_push(Command::Ping { nonce: 0 }).is_ok());
        }
        assert!(matches!(
            s.queue.try_push(Command::Ping { nonce: 0 }),
            Err(crate::queue::QueueError::Full)
        ));
    }
}
