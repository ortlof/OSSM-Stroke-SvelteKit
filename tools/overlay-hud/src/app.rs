//! The egui overlay window.
//!
//! Frameless, transparent, always-on-top. The panel is a dark translucent
//! rounded rectangle drawn by egui itself; the window behind it is transparent,
//! so the rounded corners and the area outside the panel let the desktop show
//! through. Dragging the top strip moves the window.
//!
//! Click-through (mouse passthrough) is toggled by the keyboard shortcut named
//! in the config (default `Ctrl+Shift+O`) and, for when the overlay cannot get
//! focus, by a second process running `overlay-hud --toggle-click-through`, which
//! flips the flag in the config file; this app re-reads the file about once a
//! second and applies the change.

use crate::api::FALLBACK_PATTERNS;
use crate::config::Config;
use crate::worker::{Command, Snapshot, Worker};
use eframe::egui::{self, Color32, RichText};
use std::path::PathBuf;
use std::sync::mpsc::Sender;
use std::time::{Duration, Instant};

const OK: Color32 = Color32::from_rgb(120, 220, 140);
const WARN: Color32 = Color32::from_rgb(240, 200, 90);
const ERR: Color32 = Color32::from_rgb(240, 110, 110);
const DIM: Color32 = Color32::from_rgb(165, 170, 185);

pub struct OverlayApp {
    worker: Worker,
    snapshot: Snapshot,

    // Editable values. These mirror the device except while the user is
    // touching the widget or a command is in flight; see `refresh_from_device`.
    speed: f32,
    speed_active: bool,
    depth: f32,
    depth_active: bool,
    stroke: f32,
    stroke_active: bool,
    sensation: f32,
    sensation_active: bool,
    keep_start: bool,
    pattern: i32,
    user_speed_limit: f32,
    limit_active: bool,
    home_margin: f32,
    margin_active: bool,
    reversal: f32,
    reversal_active: bool,

    patterns: Vec<String>,
    seeded: bool,
    last_send: Instant,

    click_through: bool,
    shortcut: (egui::Modifiers, egui::Key),
    shortcut_label: String,
    config: Config,
    config_path: PathBuf,
    last_config_check: Instant,
}

impl OverlayApp {
    pub fn new(cc: &eframe::CreationContext<'_>, config: Config, config_path: PathBuf) -> Self {
        let ctx = cc.egui_ctx.clone();
        ctx.set_visuals(egui::Visuals::dark());
        ctx.all_styles_mut(|style| {
            style.spacing.item_spacing = egui::vec2(6.0, 4.0);
            style.spacing.slider_width = 104.0;
            style.spacing.interact_size.y = 16.0;
            style.spacing.button_padding = egui::vec2(6.0, 3.0);
        });

        let (mods, key) = parse_shortcut(&config.click_through_shortcut);
        let worker = crate::worker::spawn(config.clone(), ctx);

        Self {
            worker,
            snapshot: Snapshot::default(),
            speed: 100.0,
            speed_active: false,
            depth: 300.0,
            depth_active: false,
            stroke: 50.0,
            stroke_active: false,
            sensation: 0.0,
            sensation_active: false,
            keep_start: false,
            pattern: 0,
            user_speed_limit: 500.0,
            limit_active: false,
            home_margin: 15.0,
            margin_active: false,
            reversal: 0.35,
            reversal_active: false,
            patterns: Vec::new(),
            seeded: false,
            last_send: Instant::now()
                .checked_sub(Duration::from_secs(10))
                .unwrap_or_else(Instant::now),
            click_through: config.click_through,
            shortcut: (mods, key),
            shortcut_label: config.click_through_shortcut.clone(),
            config,
            config_path,
            last_config_check: Instant::now(),
        }
    }

    fn send_state(&mut self, patch: serde_json::Value) {
        let _ = self.worker.cmd_tx.send(Command::Patch(patch));
        self.last_send = Instant::now();
    }

    /// Drain snapshots, keeping only the newest, and seed the editable values
    /// on the first connected snapshot.
    fn drain_snapshots(&mut self) {
        while let Ok(snap) = self.worker.snap_rx.try_recv() {
            self.snapshot = snap;
        }
        if !self.seeded && self.snapshot.connected {
            let s = &self.snapshot.state;
            let t = &self.snapshot.settings;
            self.speed = s.speed;
            self.depth = s.depth;
            self.stroke = s.stroke;
            self.sensation = s.sensation;
            self.pattern = s.pattern;
            self.keep_start = s.keep_stroke_start;
            self.user_speed_limit = t.user_speed_limit;
            self.home_margin = t.home_margin;
            self.reversal = t.reversal_acceleration_factor;
            if self.patterns.is_empty() {
                self.patterns = FALLBACK_PATTERNS.iter().map(|s| s.to_string()).collect();
            }
            self.seeded = true;
        }
    }

    /// Copy the device's view of the commands back into the widgets — but never
    /// while the user is dragging something or a command is still in flight,
    /// otherwise a slider springs back under the finger. The firmware clamps
    /// every command, so what comes back is what the machine is really doing.
    fn refresh_from_device(&mut self, ctx: &egui::Context) {
        if !self.snapshot.connected {
            return;
        }
        let interacting = ctx.input(|i| i.pointer.any_down());
        let in_flight = self.last_send.elapsed() < Duration::from_millis(600);
        if interacting || in_flight {
            return;
        }
        let s = &self.snapshot.state;
        let t = &self.snapshot.settings;
        if !self.speed_active {
            self.speed = s.speed;
        }
        if !self.depth_active {
            self.depth = s.depth;
        }
        if !self.stroke_active {
            self.stroke = s.stroke;
        }
        if !self.sensation_active {
            self.sensation = s.sensation;
        }
        self.pattern = s.pattern;
        self.keep_start = s.keep_stroke_start;
        if !self.limit_active {
            self.user_speed_limit = t.user_speed_limit;
        }
        if !self.margin_active {
            self.home_margin = t.home_margin;
        }
        if !self.reversal_active {
            self.reversal = t.reversal_acceleration_factor;
        }
    }

    /// A second process can flip `click_through` in the config file; pick it up.
    fn poll_config_file(&mut self, ctx: &egui::Context) {
        if self.last_config_check.elapsed() < Duration::from_secs(1) {
            return;
        }
        self.last_config_check = Instant::now();
        let text = match std::fs::read_to_string(&self.config_path) {
            Ok(t) => t,
            Err(_) => return,
        };
        let file: Config = match toml::from_str(&text) {
            Ok(c) => c,
            Err(_) => return,
        };
        if file.click_through != self.click_through {
            self.click_through = file.click_through;
            ctx.send_viewport_cmd(egui::ViewportCommand::MousePassthrough(self.click_through));
        }
    }

    fn handle_shortcut(&mut self, ctx: &egui::Context) {
        let (mods, key) = self.shortcut;
        let pressed = ctx.input(|i| {
            i.key_pressed(key)
                && i.modifiers.ctrl == mods.ctrl
                && i.modifiers.shift == mods.shift
                && i.modifiers.alt == mods.alt
        });
        if pressed {
            self.toggle_click_through(ctx);
        }
    }

    fn toggle_click_through(&mut self, ctx: &egui::Context) {
        self.click_through = !self.click_through;
        ctx.send_viewport_cmd(egui::ViewportCommand::MousePassthrough(self.click_through));
        // Persist so a restart and the external toggle agree. Re-read first so
        // edits made elsewhere (device address, etc.) are not clobbered.
        let mut cfg = std::fs::read_to_string(&self.config_path)
            .ok()
            .and_then(|t| toml::from_str::<Config>(&t).ok())
            .unwrap_or_else(|| self.config.clone());
        cfg.click_through = self.click_through;
        let _ = cfg.save(&self.config_path);
    }

    fn header(&mut self, ui: &mut egui::Ui) {
        let width = ui.available_width();
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(width, 22.0), egui::Sense::drag());
        if resp.drag_started() {
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::StartDrag);
        }
        if resp.hovered() {
            ui.ctx().set_cursor_icon(egui::CursorIcon::Grab);
        }

        let s = &self.snapshot.state;
        let homing = s.servo_state == "HOMING";
        let servo = if s.servo_state.is_empty() { "-" } else { s.servo_state.as_str() };
        let homed_txt = if homing {
            "HOMING"
        } else if s.homed {
            "homed"
        } else {
            "not homed"
        };
        let servo_color = if homing || !s.homed { WARN } else { DIM };
        let servo_str = format!("{servo} · {homed_txt}");
        let (conn_txt, conn_color) = if self.snapshot.connected {
            ("● online", OK)
        } else {
            ("● offline", ERR)
        };
        let click_through = self.click_through;

        let painter = ui.painter();
        painter.text(
            rect.left_center() + egui::vec2(2.0, 0.0),
            egui::Align2::LEFT_CENTER,
            "OSSM Stroke",
            egui::FontId::proportional(14.0),
            Color32::from_rgb(228, 230, 242),
        );
        if click_through {
            painter.text(
                rect.left_center() + egui::vec2(96.0, 1.0),
                egui::Align2::LEFT_CENTER,
                format!("click-through · {}", self.shortcut_label),
                egui::FontId::proportional(10.5),
                WARN,
            );
        }
        let servo_rect = painter.text(
            rect.right_center() - egui::vec2(2.0, 0.0),
            egui::Align2::RIGHT_CENTER,
            servo_str,
            egui::FontId::proportional(12.0),
            servo_color,
        );
        painter.text(
            egui::pos2(servo_rect.left() - 10.0, rect.center().y),
            egui::Align2::RIGHT_CENTER,
            conn_txt,
            egui::FontId::proportional(12.0),
            conn_color,
        );
    }

    fn readout(&mut self, ui: &mut egui::Ui) {
        let s = &self.snapshot.state;
        let travel = if s.usable_travel > 0.0 { s.usable_travel } else { 347.0 };
        let pattern_name = if s.pattern_name.is_empty() { "-" } else { s.pattern_name.as_str() };
        ui.horizontal_wrapped(|ui| {
            ui.label(RichText::new(format!("pos {:.1} mm", s.position)).color(Color32::from_rgb(210, 214, 228)));
            ui.label(
                RichText::new(format!("· {:.0} of {:.0} mm/s", s.speed_measured.abs(), s.speed))
                    .color(Color32::from_rgb(210, 214, 228)),
            );
            ui.label(
                RichText::new(format!(
                    "· sweep {:.0}→{:.0} of {:.0} mm",
                    s.sweep_start, s.depth, travel
                ))
                .color(DIM),
            );
            ui.label(RichText::new(format!("· {pattern_name}")).color(DIM));
            if s.clipping {
                ui.label(RichText::new("· CLIPPING").color(ERR).strong());
            }
            if s.setup != 0 {
                ui.label(RichText::new(format!("· SETUP {}", s.setup)).color(WARN));
            }
        });
        if let Some(err) = &self.snapshot.error {
            ui.label(RichText::new(err.as_str()).color(ERR).small());
        }
    }

    fn controls(&mut self, ui: &mut egui::Ui) {
        let running = self.snapshot.state.run;
        let homed = self.snapshot.state.homed;
        ui.horizontal(|ui| {
            if ui
                .add_enabled(!running, egui::Button::new("▶ Start").fill(Color32::from_rgb(40, 122, 74)))
                .clicked()
            {
                self.send_state(serde_json::json!({ "run": true }));
            }
            if ui
                .add_enabled(running, egui::Button::new("■ Stop").fill(Color32::from_rgb(140, 52, 52)))
                .clicked()
            {
                self.send_state(serde_json::json!({ "run": false }));
            }
            if ui.button("⌂ Home").clicked() {
                self.send_state(serde_json::json!({ "home": true }));
            }
            if !homed {
                ui.label(RichText::new("not homed").color(WARN).small());
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let label = if self.click_through {
                    RichText::new("click-through ON").color(WARN)
                } else {
                    RichText::new("click-through")
                };
                if ui.button(label).on_hover_text("Toggle mouse click-through").clicked() {
                    let ctx = ui.ctx().clone();
                    self.toggle_click_through(&ctx);
                }
            });
        });
    }

    fn sliders(&mut self, ui: &mut egui::Ui) {
        let limit = if self.snapshot.settings.user_speed_limit >= 1.0 {
            self.snapshot.settings.user_speed_limit
        } else {
            600.0
        };
        let travel = if self.snapshot.state.usable_travel > 0.0 {
            self.snapshot.state.usable_travel
        } else {
            347.0
        };
        let cmd = self.worker.cmd_tx.clone();
        let mut sent = false;
        ui.columns(2, |cols| {
            sent |= value_slider(&mut cols[0], &mut self.speed, &mut self.speed_active, &cmd, "Speed mm/s", 1.0, limit, 1.0, "speed");
            sent |= value_slider(&mut cols[0], &mut self.depth, &mut self.depth_active, &cmd, "Depth mm", 0.0, travel, 5.0, "depth");
            sent |= value_slider(&mut cols[0], &mut self.stroke, &mut self.stroke_active, &cmd, "Stroke mm", 0.0, travel, 5.0, "stroke");
            sent |= value_slider(&mut cols[1], &mut self.sensation, &mut self.sensation_active, &cmd, "Sensation", -100.0, 100.0, 5.0, "sensation");
            sent |= pattern_combo(&mut cols[1], &mut self.pattern, &cmd, &self.patterns);
            sent |= keep_toggle(&mut cols[1], &mut self.keep_start, &cmd);
        });
        if sent {
            self.last_send = Instant::now();
        }
    }

    fn tuning(&mut self, ui: &mut egui::Ui) {
        let cmd = self.worker.cmd_tx.clone();
        let mut sent = false;
        egui::CollapsingHeader::new("Tuning and limits")
            .id_salt("tuning")
            .default_open(false)
            .show(ui, |ui| {
                sent |= settings_slider(ui, &mut self.user_speed_limit, &mut self.limit_active, &cmd, "Speed slider limit mm/s", 50.0, 700.0, 10.0, "user_speed_limit");
                sent |= settings_slider(ui, &mut self.home_margin, &mut self.margin_active, &cmd, "Home margin mm", 0.0, 50.0, 5.0, "home_margin");
                sent |= settings_slider(ui, &mut self.reversal, &mut self.reversal_active, &cmd, "Reversal accel factor", 0.05, 1.0, 0.05, "reversal_acceleration_factor");
            });
        if sent {
            self.last_send = Instant::now();
        }
    }
}

impl eframe::App for OverlayApp {
    fn clear_color(&self, _visuals: &egui::Visuals) -> [f32; 4] {
        // Fully transparent: the rounded panel is drawn by egui, everything
        // outside it is the desktop showing through.
        [0.0, 0.0, 0.0, 0.0]
    }

    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.drain_snapshots();
        self.refresh_from_device(&ctx);
        self.poll_config_file(&ctx);
        self.handle_shortcut(&ctx);

        let panel = egui::Frame::NONE
            .fill(Color32::from_rgba_unmultiplied(16, 16, 22, 214))
            .stroke(egui::Stroke::new(1.0, Color32::from_rgba_unmultiplied(130, 135, 160, 110)))
            .corner_radius(egui::CornerRadius::same(14))
            .inner_margin(egui::Margin::same(9));

        egui::CentralPanel::default().frame(panel).show(ui, |ui| {
            egui::ScrollArea::vertical()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    self.header(ui);
                    ui.add_space(2.0);
                    self.readout(ui);
                    ui.add_space(3.0);
                    self.controls(ui);
                    ui.add_space(3.0);
                    self.sliders(ui);
                    ui.add_space(2.0);
                    self.tuning(ui);
                });
        });

        // Keep the readout ticking even when the pointer is idle.
        ctx.request_repaint_after(Duration::from_millis(120));
    }
}

fn patch_f32(key: &str, value: f32) -> serde_json::Value {
    let mut map = serde_json::Map::new();
    map.insert(key.to_string(), serde_json::Value::from(value));
    serde_json::Value::Object(map)
}

fn patch_bool(key: &str, value: bool) -> serde_json::Value {
    let mut map = serde_json::Map::new();
    map.insert(key.to_string(), serde_json::Value::from(value));
    serde_json::Value::Object(map)
}

/// A labelled slider bound to a `f32`. Returns true when it sent a command.
#[allow(clippy::too_many_arguments)]
fn value_slider(
    ui: &mut egui::Ui,
    value: &mut f32,
    active: &mut bool,
    cmd: &Sender<Command>,
    label: &str,
    min: f32,
    max: f32,
    step: f64,
    key: &str,
) -> bool {
    let resp = ui.add(
        egui::Slider::new(&mut *value, min..=max)
            .step_by(step)
            .text(label),
    );
    *active = resp.dragged() || resp.has_focus();
    if resp.drag_stopped() || (resp.changed() && !resp.dragged()) {
        let _ = cmd.send(Command::Patch(patch_f32(key, *value)));
        true
    } else {
        false
    }
}

#[allow(clippy::too_many_arguments)]
fn settings_slider(
    ui: &mut egui::Ui,
    value: &mut f32,
    active: &mut bool,
    cmd: &Sender<Command>,
    label: &str,
    min: f32,
    max: f32,
    step: f64,
    key: &str,
) -> bool {
    let resp = ui.add(
        egui::Slider::new(&mut *value, min..=max)
            .step_by(step)
            .text(label),
    );
    *active = resp.dragged() || resp.has_focus();
    if resp.drag_stopped() || (resp.changed() && !resp.dragged()) {
        let _ = cmd.send(Command::SettingsPatch(patch_f32(key, *value)));
        true
    } else {
        false
    }
}

fn pattern_combo(
    ui: &mut egui::Ui,
    pattern: &mut i32,
    cmd: &Sender<Command>,
    patterns: &[String],
) -> bool {
    let selected = patterns
        .get(*pattern as usize)
        .map(String::as_str)
        .unwrap_or("—");
    let mut changed = false;
    egui::ComboBox::from_id_salt("pattern_combo")
        .width(ui.available_width().min(150.0))
        .selected_text(selected)
        .show_ui(ui, |ui| {
            for (i, name) in patterns.iter().enumerate() {
                if ui.selectable_value(pattern, i as i32, name.as_str()).clicked() {
                    changed = true;
                }
            }
        });
    ui.label(RichText::new("Pattern").small().color(DIM));
    if changed {
        let mut map = serde_json::Map::new();
        map.insert("pattern".to_string(), serde_json::Value::from(*pattern));
        let _ = cmd.send(Command::Patch(serde_json::Value::Object(map)));
        true
    } else {
        false
    }
}

fn keep_toggle(ui: &mut egui::Ui, keep: &mut bool, cmd: &Sender<Command>) -> bool {
    let resp = ui.checkbox(keep, "Keep stroke start");
    if resp.changed() {
        let _ = cmd.send(Command::Patch(patch_bool("keep_stroke_start", *keep)));
        true
    } else {
        false
    }
}

/// Parse `"Ctrl+Shift+O"` into egui modifiers plus a key. Unknown key names
/// fall back to `O`, which keeps the escape hatch usable even if the config is
/// mistyped.
fn parse_shortcut(spec: &str) -> (egui::Modifiers, egui::Key) {
    let mut mods = egui::Modifiers::default();
    let mut key = egui::Key::O;
    for part in spec.split('+') {
        match part.trim().to_ascii_lowercase().as_str() {
            "ctrl" | "control" => mods.ctrl = true,
            "shift" => mods.shift = true,
            "alt" => mods.alt = true,
            "cmd" | "command" | "super" | "win" | "meta" => mods.command = true,
            other => {
                if let Some(k) = egui::Key::from_name(other) {
                    key = k;
                }
            }
        }
    }
    (mods, key)
}
