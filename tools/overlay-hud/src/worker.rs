//! The polling thread.
//!
//! Lesson from this project, load-bearing: do NOT stream the WebSocket and make
//! blocking HTTP calls from the same thread. The device's sends back up, the
//! HTTP request stalls for its whole timeout, and it looks exactly like a
//! firmware hang. So v1 polls REST from one dedicated thread and never opens
//! the event socket.
//!
//! The thread owns the device client and the JWT. The UI thread only pushes
//! [`Command`]s in and drains [`Snapshot`]s out.

use crate::api::{ApiError, DeviceClient, StrokeSettings, StrokeState};
use crate::config::Config;
use std::sync::mpsc::{Receiver, Sender, TryRecvError};
use std::time::{Duration, Instant};

/// A request from the UI to the device.
pub enum Command {
    /// Subset to POST to `/rest/strokeState`.
    Patch(serde_json::Value),
    /// Subset to POST to `/rest/strokeSettings`.
    /// Part of the worker's protocol; the overlay's tuning controls were
    /// removed in favour of the browser page, which is the better place for
    /// them.
    #[allow(dead_code)]
    SettingsPatch(serde_json::Value),
}

/// Everything the UI needs for one frame.
#[derive(Clone, Default)]
pub struct Snapshot {
    pub state: StrokeState,
    pub settings: StrokeSettings,
    pub connected: bool,
    #[allow(dead_code)]
    pub error: Option<String>,
}

pub struct Worker {
    pub cmd_tx: Sender<Command>,
    pub snap_rx: Receiver<Snapshot>,
}

/// Start the polling thread. `ctx` repaints the UI whenever a new snapshot
/// lands, so the readout stays live without the UI polling on its own.
pub fn spawn(cfg: Config, ctx: eframe::egui::Context) -> Worker {
    let (cmd_tx, cmd_rx) = std::sync::mpsc::channel();
    let (snap_tx, snap_rx) = std::sync::mpsc::channel();
    std::thread::Builder::new()
        .name("ossm-poll".to_string())
        .spawn(move || run(cfg, ctx, cmd_rx, snap_tx))
        .expect("failed to spawn polling thread");
    Worker { cmd_tx, snap_rx }
}

fn run(
    cfg: Config,
    ctx: eframe::egui::Context,
    cmd_rx: Receiver<Command>,
    snap_tx: Sender<Snapshot>,
) {
    let client = match DeviceClient::new(&cfg.device, &cfg.username, &cfg.password) {
        Ok(c) => c,
        Err(e) => {
            // Bad config: report forever, do not spin.
            loop {
                let _ = snap_tx.send(Snapshot {
                    error: Some(e.clone()),
                    ..Default::default()
                });
                ctx.request_repaint();
                std::thread::sleep(Duration::from_secs(1));
            }
        }
    };

    let poll = Duration::from_secs_f32(1.0 / cfg.poll_hz.clamp(1.0, 20.0));
    let settings_period = Duration::from_secs_f32(cfg.settings_poll_secs.clamp(0.5, 60.0));

    let mut token: Option<String> = None;
    let mut state = StrokeState::default();
    let mut settings = StrokeSettings::default();
    let mut last_settings: Option<Instant> = None;

    loop {
        let mut connected = false;
        let mut error: Option<String> = None;

        if token.is_none() {
            match client.sign_in() {
                Ok(t) => token = Some(t),
                Err(e) => error = Some(format!("sign in: {e}")),
            }
        }

        if let Some(tok) = token.clone() {
            // Apply everything the UI queued since the last tick.
            loop {
                match cmd_rx.try_recv() {
                    Ok(Command::Patch(patch)) => match client.post_state(&tok, &patch) {
                        Ok(s) => {
                            state = s;
                            connected = true;
                        }
                        Err(ApiError::Unauthorized) => {
                            token = None;
                            error = Some("session expired, re-authenticating".to_string());
                            break;
                        }
                        Err(ApiError::Other(e)) => error = Some(e),
                    },
                    Ok(Command::SettingsPatch(patch)) => match client.post_settings(&tok, &patch) {
                        Ok(s) => settings = s,
                        Err(ApiError::Unauthorized) => {
                            token = None;
                            error = Some("session expired, re-authenticating".to_string());
                            break;
                        }
                        Err(ApiError::Other(e)) => error = Some(e),
                    },
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => return,
                }
            }

            if token.is_some() {
                match client.get_state(&tok) {
                    Ok(s) => {
                        state = s;
                        connected = true;
                    }
                    Err(ApiError::Unauthorized) => {
                        token = None;
                        error = Some("session expired, re-authenticating".to_string());
                    }
                    Err(ApiError::Other(e)) => error = Some(e),
                }

                let due = last_settings.is_none_or(|t| t.elapsed() >= settings_period);
                if due && token.is_some() {
                    last_settings = Some(Instant::now());
                    match client.get_settings(&tok) {
                        Ok(s) => settings = s,
                        Err(ApiError::Unauthorized) => token = None,
                        Err(ApiError::Other(_)) => {}
                    }
                }
            }
        }

        let _ = snap_tx.send(Snapshot {
            state: state.clone(),
            settings: settings.clone(),
            connected,
            error,
        });
        ctx.request_repaint();

        // Poll fast while healthy; back off to 2 Hz while the device is away so
        // a missing machine does not flood the network with sign-in attempts.
        std::thread::sleep(if connected { poll } else { Duration::from_millis(500) });
    }
}
