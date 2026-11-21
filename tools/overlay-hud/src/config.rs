//! Runtime configuration, loaded from a TOML file next to the executable (or
//! the current directory, or `$OSSM_HUD_CONFIG`).
//!
//! The device credentials are factory defaults (`admin` / `admin`) and are a
//! known security issue in the firmware. They live here, in the config file,
//! never in the source.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    /// Base URL of the OSSM-Stroke device, without a trailing slash.
    pub device: String,
    /// Framework login. Factory default is `admin` / `admin` — change it.
    pub username: String,
    pub password: String,
    /// Telemetry poll rate in Hz. 5–10 is the useful range; the firmware
    /// answer rate, not the network, is the limit.
    pub poll_hz: f32,
    /// How often the slider limits (`/rest/strokeSettings`) are re-read.
    pub settings_poll_secs: f32,
    /// Start with mouse click-through enabled. Also flipped at runtime by the
    /// keyboard shortcut and by a second process (see `--toggle-click-through`).
    pub click_through: bool,
    /// Keyboard shortcut that toggles click-through, e.g. `"Ctrl+Shift+O"`.
    pub click_through_shortcut: String,
    /// Initial window size in logical points.
    pub window_width: f32,
    pub window_height: f32,

    /// How opaque the panel is: 0 is invisible, 255 is solid. Around 96 keeps a
    /// video or VR window readable behind the controls while the text stays
    /// legible.
    pub panel_alpha: u8,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            device: "http://192.168.2.115".to_string(),
            username: "admin".to_string(),
            password: "admin".to_string(),
            poll_hz: 8.0,
            settings_poll_secs: 2.0,
            click_through: false,
            click_through_shortcut: "Ctrl+Shift+O".to_string(),
            window_width: 240.0,
            window_height: 480.0,
            panel_alpha: 96,
        }
    }
}

impl Config {
    /// Resolve the config path and load it, writing the defaults out if the
    /// file does not exist yet.
    pub fn load() -> Result<(Config, PathBuf), String> {
        let path = Self::resolve_path();
        let cfg = match std::fs::read_to_string(&path) {
            Ok(text) => toml::from_str(&text).map_err(|e| format!("parse {}: {e}", path.display()))?,
            Err(_) => {
                let cfg = Config::default();
                cfg.save(&path)?;
                cfg
            }
        };
        Ok((cfg, path))
    }

    /// Where the config lives. Order: `$OSSM_HUD_CONFIG`, `./config.toml`,
    /// `<exe dir>/config.toml`. If none exists, the exe directory is used so a
    /// double-clicked binary finds its own file even though the working
    /// directory is elsewhere.
    pub fn resolve_path() -> PathBuf {
        if let Ok(p) = std::env::var("OSSM_HUD_CONFIG") {
            if !p.is_empty() {
                return PathBuf::from(p);
            }
        }
        let cwd = PathBuf::from("config.toml");
        if cwd.exists() {
            return cwd;
        }
        if let Ok(exe) = std::env::current_exe() {
            if let Some(dir) = exe.parent() {
                let p = dir.join("config.toml");
                if p.exists() {
                    return p;
                }
                return p;
            }
        }
        cwd
    }

    pub fn save(&self, path: &Path) -> Result<(), String> {
        let text = toml::to_string_pretty(self).map_err(|e| format!("serialise config: {e}"))?;
        std::fs::write(path, text).map_err(|e| format!("write {}: {e}", path.display()))
    }
}

/// Flip `click_through` in the config file and return the new value. Used by
/// `overlay-hud --toggle-click-through`: a second process (Stream Deck,
/// AutoHotkey, a shell one-liner) runs this to leave click-through mode while
/// the overlay itself cannot receive the mouse.
pub fn toggle_click_through() -> Result<bool, String> {
    let path = Config::resolve_path();
    let mut cfg = match std::fs::read_to_string(&path) {
        Ok(text) => toml::from_str(&text).map_err(|e| format!("parse {}: {e}", path.display()))?,
        Err(_) => Config::default(),
    };
    cfg.click_through = !cfg.click_through;
    cfg.save(&path)?;
    Ok(cfg.click_through)
}
