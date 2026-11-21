//! Minimal blocking HTTP/1.1 client for the OSSM-Stroke device REST API.
//!
//! The device serves plain HTTP, so a TLS-capable HTTP crate (and its C
//! dependencies such as `ring`) is not needed. Keeping this hand-rolled keeps
//! the dependency tree small, which matters for the mingw cross build.
//!
//! Endpoints used, all verified against the merged firmware and against
//! `interface/src/routes/stroke/Stroke.svelte`:
//!   POST /rest/signIn        {"username","password"} -> {"access_token"}
//!   GET  /rest/strokeState   -> full state
//!   POST /rest/strokeState   subset -> state *after* the firmware clamped it
//!   GET  /rest/strokeSettings
//!   POST /rest/strokeSettings subset -> settings
//!
//! Every request carries `Authorization: Bearer <jwt>`.

use serde::Deserialize;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

/// Pattern names the firmware ships with, in firmware index order
/// (`lib/StrokeEngine/src/pattern.h`, the `patternTable` array). Index == the
/// value the firmware expects in `{"pattern": N}` and the value it reports in
/// `state.pattern`. The patterns commented out there (Wave, Jack Hammer, Stroke
/// Nibbler) are not compiled in, so they are not in this list either.
///
/// The list is fixed in firmware, so the overlay's pattern selector reads it
/// straight from here; nothing is fetched.
pub const FALLBACK_PATTERNS: &[&str] = &[
    "Simple Stroke",
    "Teasing or Pounding",
    "Robo Stroke",
    "Half'n'Half",
    "Deeper",
    "Stop'n'Go",
    "Insist",
    "Knot",
];

/// Full `/rest/strokeState` payload: commands plus telemetry.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct StrokeState {
    pub run: bool,
    pub setup: i32,
    pub speed: f32,
    pub depth: f32,
    pub stroke: f32,
    pub sensation: f32,
    pub pattern: i32,
    pub pattern_name: String,
    pub servo_state: String,
    pub homed: bool,
    pub position: f32,
    pub speed_measured: f32,
    pub physical_travel: f32,
    pub usable_travel: f32,
    pub sweep_start: f32,
    pub clipping: bool,
    pub keep_stroke_start: bool,
}

/// `/rest/strokeSettings`: the motion scale and the slider limits.
#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct StrokeSettings {
    pub keepout: f32,
    pub physical_travel: f32,
    pub steps_per_mm: f32,
    pub max_speed: f32,
    pub max_acceleration: f32,
    pub user_speed_limit: f32,
    pub invert_direction: bool,
    pub sensorless_homing: bool,
    pub reversal_acceleration_factor: f32,
    pub home_margin: f32,
    pub far_margin: f32,
}

impl Default for StrokeSettings {
    fn default() -> Self {
        // Sensible rack-and-pinion values, replaced by the device on first poll.
        Self {
            keepout: 15.0,
            physical_travel: 377.0,
            steps_per_mm: 84.88,
            max_speed: 600.0,
            max_acceleration: 10_000.0,
            user_speed_limit: 500.0,
            invert_direction: true,
            sensorless_homing: true,
            reversal_acceleration_factor: 0.35,
            home_margin: 15.0,
            far_margin: 15.0,
        }
    }
}

#[derive(Debug)]
pub enum ApiError {
    /// 401/403: the JWT is missing, stale or wrong — sign in again.
    Unauthorized,
    Other(String),
}

impl std::fmt::Display for ApiError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ApiError::Unauthorized => write!(f, "unauthorized (session expired)"),
            ApiError::Other(e) => write!(f, "{e}"),
        }
    }
}

pub struct DeviceClient {
    host: String,
    port: u16,
    base_path: String,
    username: String,
    password: String,
    timeout: Duration,
}

impl DeviceClient {
    pub fn new(base_url: &str, username: &str, password: &str) -> Result<Self, String> {
        let trimmed = base_url.trim().trim_end_matches('/');
        let rest = trimmed
            .strip_prefix("http://")
            .ok_or_else(|| format!("device URL must start with http:// (got '{base_url}')"))?;
        let (authority, base_path) = match rest.split_once('/') {
            Some((a, p)) => (a, format!("/{p}")),
            None => (rest, String::new()),
        };
        let (host, port) = match authority.rsplit_once(':') {
            Some((h, p)) => (
                h.to_string(),
                p.parse::<u16>().map_err(|_| format!("bad port in '{authority}'"))?,
            ),
            None => (authority.to_string(), 80),
        };
        if host.is_empty() {
            return Err(format!("no host in device URL '{base_url}'"));
        }
        Ok(Self {
            host,
            port,
            base_path,
            username: username.to_string(),
            password: password.to_string(),
            timeout: Duration::from_millis(4000),
        })
    }

    pub fn sign_in(&self) -> Result<String, String> {
        let body = serde_json::json!({
            "username": self.username,
            "password": self.password,
        })
        .to_string();
        let (status, text) = self.request("POST", "/rest/signIn", None, Some(&body))?;
        if status != 200 {
            return Err(format!("sign in failed: HTTP {status}"));
        }
        let value: serde_json::Value =
            serde_json::from_str(&text).map_err(|e| format!("sign in response not JSON: {e}"))?;
        value
            .get("access_token")
            .and_then(|t| t.as_str())
            .map(str::to_string)
            .ok_or_else(|| "sign in response had no access_token".to_string())
    }

    pub fn get_state(&self, token: &str) -> Result<StrokeState, ApiError> {
        let (status, text) = self
            .request("GET", "/rest/strokeState", Some(token), None)
            .map_err(ApiError::Other)?;
        decode(status, &text)
    }

    /// POST a subset of the state. The firmware clamps and returns the state it
    /// actually applied, which is what the UI must display.
    pub fn post_state(&self, token: &str, patch: &serde_json::Value) -> Result<StrokeState, ApiError> {
        let body = patch.to_string();
        let (status, text) = self
            .request("POST", "/rest/strokeState", Some(token), Some(&body))
            .map_err(ApiError::Other)?;
        decode(status, &text)
    }

    pub fn get_settings(&self, token: &str) -> Result<StrokeSettings, ApiError> {
        let (status, text) = self
            .request("GET", "/rest/strokeSettings", Some(token), None)
            .map_err(ApiError::Other)?;
        decode(status, &text)
    }

    pub fn post_settings(
        &self,
        token: &str,
        patch: &serde_json::Value,
    ) -> Result<StrokeSettings, ApiError> {
        let body = patch.to_string();
        let (status, text) = self
            .request("POST", "/rest/strokeSettings", Some(token), Some(&body))
            .map_err(ApiError::Other)?;
        decode(status, &text)
    }

    fn request(
        &self,
        method: &str,
        path: &str,
        token: Option<&str>,
        body: Option<&str>,
    ) -> Result<(u16, String), String> {
        let addr = (self.host.as_str(), self.port)
            .to_socket_addrs()
            .map_err(|e| format!("resolve {}: {e}", self.host))?
            .next()
            .ok_or_else(|| format!("no address for {}", self.host))?;
        let mut stream = TcpStream::connect_timeout(&addr, self.timeout)
            .map_err(|e| format!("connect {addr}: {e}"))?;
        let _ = stream.set_read_timeout(Some(self.timeout));
        let _ = stream.set_write_timeout(Some(self.timeout));
        let _ = stream.set_nodelay(true);

        let mut req = String::new();
        req.push_str(&format!("{method} {}{path} HTTP/1.1\r\n", self.base_path));
        req.push_str(&format!("Host: {}:{}\r\n", self.host, self.port));
        req.push_str("Connection: close\r\n");
        req.push_str("Accept: application/json\r\n");
        if let Some(t) = token {
            req.push_str(&format!("Authorization: Bearer {t}\r\n"));
        }
        if let Some(b) = body {
            req.push_str("Content-Type: application/json\r\n");
            req.push_str(&format!("Content-Length: {}\r\n", b.len()));
        }
        req.push_str("\r\n");

        stream.write_all(req.as_bytes()).map_err(|e| format!("write request: {e}"))?;
        if let Some(b) = body {
            stream.write_all(b.as_bytes()).map_err(|e| format!("write body: {e}"))?;
        }
        stream.flush().map_err(|e| format!("flush: {e}"))?;

        let mut reader = BufReader::new(stream);
        read_response(&mut reader)
    }
}

fn decode<T: for<'de> Deserialize<'de>>(status: u16, text: &str) -> Result<T, ApiError> {
    if status == 401 || status == 403 {
        return Err(ApiError::Unauthorized);
    }
    if status != 200 {
        return Err(ApiError::Other(format!("HTTP {status}: {}", text.trim())));
    }
    serde_json::from_str(text).map_err(|e| ApiError::Other(format!("bad JSON: {e}")))
}

fn read_response(reader: &mut BufReader<TcpStream>) -> Result<(u16, String), String> {
    let mut status_line = String::new();
    reader
        .read_line(&mut status_line)
        .map_err(|e| format!("read status line: {e}"))?;
    if status_line.is_empty() {
        return Err("empty response".to_string());
    }
    let status: u16 = status_line
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| format!("bad status line: {}", status_line.trim()))?;

    let mut content_length: Option<usize> = None;
    let mut chunked = false;
    loop {
        let mut header = String::new();
        let n = reader
            .read_line(&mut header)
            .map_err(|e| format!("read header: {e}"))?;
        if n == 0 {
            break;
        }
        let header = header.trim_end_matches(['\r', '\n']);
        if header.is_empty() {
            break;
        }
        let lower = header.to_ascii_lowercase();
        if let Some(v) = lower.strip_prefix("content-length:") {
            content_length = v.trim().parse().ok();
        } else if lower.starts_with("transfer-encoding:") && lower.contains("chunked") {
            chunked = true;
        }
    }

    let body = if chunked {
        read_chunked(reader)?
    } else if let Some(len) = content_length {
        let mut buf = vec![0u8; len];
        reader.read_exact(&mut buf).map_err(|e| format!("read body: {e}"))?;
        String::from_utf8_lossy(&buf).into_owned()
    } else {
        let mut text = String::new();
        let _ = reader.read_to_string(&mut text);
        text
    };
    Ok((status, body))
}

fn read_chunked(reader: &mut BufReader<TcpStream>) -> Result<String, String> {
    let mut out = String::new();
    loop {
        let mut size_line = String::new();
        reader
            .read_line(&mut size_line)
            .map_err(|e| format!("read chunk size: {e}"))?;
        let size_hex = size_line
            .trim()
            .split(';')
            .next()
            .unwrap_or("")
            .trim();
        let size = usize::from_str_radix(size_hex, 16)
            .map_err(|_| format!("bad chunk size '{size_hex}'"))?;
        if size == 0 {
            // trailing CRLF after the final chunk
            let mut trailer = String::new();
            let _ = reader.read_line(&mut trailer);
            break;
        }
        let mut buf = vec![0u8; size];
        reader.read_exact(&mut buf).map_err(|e| format!("read chunk: {e}"))?;
        out.push_str(&String::from_utf8_lossy(&buf));
        let mut crlf = [0u8; 2];
        reader.read_exact(&mut crlf).map_err(|e| format!("read chunk CRLF: {e}"))?;
    }
    Ok(out)
}
