//! The egui overlay window.
//!
//! Frameless, transparent, always-on-top. The panel is a translucent rounded
//! rectangle drawn by egui; the window itself is transparent, so everything
//! outside the panel lets the desktop through.
//!
//! The layout is deliberately sparse, because it is aimed at a pointer far from
//! the screen (a VR controller, a laser pointer, a hand on a touch panel):
//!
//!   - one full-width Start/Stop button, 54 px tall
//!   - three tall vertical sliders, each about a third of the width
//!   - the value is printed inside the slider thumb, so no readout row is needed
//!   - one small status dot, no status text
//!
//! Vertical rather than horizontal because a vertical target can be made tall and
//! fat without widening the window, and because a hand that is not resting on
//! anything moves up and down more predictably than sideways.
//!
//! Click-through (mouse passthrough) is toggled by the keyboard shortcut in the
//! config (default `Ctrl+Shift+O`) and, for when the overlay cannot take focus,
//! by a second process running `overlay-hud --toggle-click-through`, which flips
//! the flag in the config file; this app re-reads that file about once a second.

use crate::config::Config;
use crate::worker::{Command, Snapshot, Worker};
use eframe::egui::{self, Align2, Color32, CornerRadius, FontId, Rect, Sense};
use std::path::PathBuf;
use std::time::{Duration, Instant};

// Translucent throughout: the panel is roughly 43 % opaque, so the video or VR
// window behind it stays readable while the controls stay legible.
fn panel_alpha_of(alpha: u8) -> Color32 {
    Color32::from_rgba_unmultiplied(12, 12, 16, alpha)
}
fn border() -> Color32 {
    Color32::from_rgba_unmultiplied(160, 165, 190, 70)
}
fn rail_c() -> Color32 {
    Color32::from_rgba_unmultiplied(0, 0, 0, 80)
}
fn fill_c() -> Color32 {
    Color32::from_rgba_unmultiplied(96, 186, 255, 205)
}
fn thumb_c() -> Color32 {
    Color32::from_rgba_unmultiplied(238, 242, 255, 210)
}
fn thumb_hot_c() -> Color32 {
    Color32::from_rgba_unmultiplied(255, 255, 255, 245)
}
fn thumb_text() -> Color32 {
    Color32::from_rgba_unmultiplied(12, 14, 20, 255)
}
fn label_c() -> Color32 {
    Color32::from_rgba_unmultiplied(226, 230, 244, 215)
}
fn grip_c() -> Color32 {
    Color32::from_rgba_unmultiplied(200, 205, 225, 90)
}
fn start_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(42, 140, 92, 170)
}
fn stop_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(172, 55, 55, 170)
}
fn disabled_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(72, 74, 86, 110)
}

const OK: Color32 = Color32::from_rgb(120, 220, 140);
const WARN: Color32 = Color32::from_rgb(240, 200, 90);
const ERR: Color32 = Color32::from_rgb(240, 110, 110);

const BUTTON_H: f32 = 54.0;
const THUMB_H: f32 = 34.0;
const GAP: f32 = 10.0;

pub struct OverlayApp {
    worker: Worker,
    snapshot: Snapshot,

    // Editable values. These mirror the device except while the user is touching
    // the widget or a command is in flight; see `refresh_from_device`.
    speed: f32,
    speed_active: bool,
    depth: f32,
    depth_active: bool,
    stroke: f32,
    stroke_active: bool,

    seeded: bool,
    last_send: Instant,

    click_through: bool,
    shortcut: (egui::Modifiers, egui::Key),
    config: Config,
    config_path: PathBuf,
    last_config_check: Instant,
}

impl OverlayApp {
    pub fn new(cc: &eframe::CreationContext<'_>, config: Config, config_path: PathBuf) -> Self {
        let ctx = cc.egui_ctx.clone();
        ctx.set_visuals(egui::Visuals::dark());
        ctx.all_styles_mut(|style| {
            style.spacing.item_spacing = egui::vec2(GAP, 6.0);
            style.spacing.button_padding = egui::vec2(8.0, 4.0);
        });

        let (mods, key) = parse_shortcut(&config.click_through_shortcut);
        let worker = crate::worker::spawn(config.clone(), ctx);

        Self {
            worker,
            snapshot: Snapshot::default(),
            speed: 100.0,
            speed_active: false,
            depth: 150.0,
            depth_active: false,
            stroke: 30.0,
            stroke_active: false,
            seeded: false,
            last_send: Instant::now()
                .checked_sub(Duration::from_secs(10))
                .unwrap_or_else(Instant::now),
            click_through: config.click_through,
            shortcut: (mods, key),
            config,
            config_path,
            last_config_check: Instant::now(),
        }
    }

    fn send_state(&mut self, patch: serde_json::Value) {
        let _ = self.worker.cmd_tx.send(Command::Patch(patch));
        self.last_send = Instant::now();
    }

    /// Drain snapshots, keeping only the newest, and seed the editable values on
    /// the first connected snapshot.
    fn drain_snapshots(&mut self) {
        while let Ok(snap) = self.worker.snap_rx.try_recv() {
            self.snapshot = snap;
        }
        if !self.seeded && self.snapshot.connected {
            let s = &self.snapshot.state;
            self.speed = s.speed;
            self.depth = s.depth;
            self.stroke = s.stroke;
            self.seeded = true;
        }
    }

    /// Copy the device's view of the commands back into the widgets - but never
    /// while the user is dragging something or a command is still in flight,
    /// otherwise a slider springs back under the hand. The firmware clamps every
    /// command, so what comes back is what the machine is really doing.
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
        if !self.speed_active {
            self.speed = s.speed;
        }
        if !self.depth_active {
            self.depth = s.depth;
        }
        if !self.stroke_active {
            self.stroke = s.stroke;
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
        // edits made elsewhere (device address, and so on) are not clobbered.
        let mut cfg = std::fs::read_to_string(&self.config_path)
            .ok()
            .and_then(|t| toml::from_str::<Config>(&t).ok())
            .unwrap_or_else(|| self.config.clone());
        cfg.click_through = self.click_through;
        let _ = cfg.save(&self.config_path);
    }

    /// A thin grip across the top: the only place that moves the window.
    fn grip(&mut self, ui: &mut egui::Ui) {
        let w = ui.available_width();
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, 22.0), Sense::drag());
        let painter = ui.painter();
        let pill = Rect::from_center_size(rect.center(), egui::vec2(64.0, 5.0));
        painter.rect_filled(pill, CornerRadius::same(2), grip_c());
        if resp.drag_started() {
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::StartDrag);
        }
        if resp.hovered() {
            ui.ctx().set_cursor_icon(egui::CursorIcon::Grab);
        }
    }

    /// One button, taller than a normal one, that starts and stops.
    fn big_button(&mut self, ui: &mut egui::Ui) {
        let w = ui.available_width();
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, BUTTON_H), Sense::click());
        let running = self.snapshot.state.run;
        let homed = self.snapshot.state.homed;
        let connected = self.snapshot.connected;
        let can_act = connected && (running || homed);

        let fill = if !can_act {
            disabled_fill()
        } else if running {
            stop_fill()
        } else {
            start_fill()
        };
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(12), fill);
        if resp.hovered() && can_act {
            painter.rect_filled(rect, CornerRadius::same(12), Color32::from_white_alpha(20));
        }
        painter.text(
            rect.center(),
            Align2::CENTER_CENTER,
            if running { "STOP" } else { "START" },
            FontId::proportional(22.0),
            Color32::from_rgb(242, 244, 252),
        );

        let dot_color = if !connected {
            ERR
        } else if running || homed {
            OK
        } else {
            WARN
        };
        let dot = egui::pos2(rect.left() + 16.0, rect.center().y);
        painter.circle_filled(dot, 7.0, Color32::from_rgba_unmultiplied(10, 12, 16, 150));
        painter.circle_filled(dot, 5.0, dot_color);

        if resp.clicked() && can_act {
            self.send_state(serde_json::json!({ "run": !running }));
        }
        if resp.hovered() {
            let hint = if !connected {
                "no connection to the machine"
            } else if !homed && !running {
                "the machine is not homed - start it from the browser once"
            } else {
                "start / stop"
            };
            resp.on_hover_text(hint);
        }
    }

    fn sliders_row(&mut self, ui: &mut egui::Ui) {
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
        let margin = self.snapshot.settings.home_margin.max(0.0);
        // The firmware holds the near end of a sweep this far off the home point,
        // so a stroke can never be as long as the depth.
        let stroke_max = (self.depth - margin).clamp(5.0, travel);

        let h = (ui.available_height() - 22.0).max(70.0);
        let w = ((ui.available_width() - GAP * 2.0) / 3.0).floor().max(48.0);

        let mut patch = serde_json::Map::new();
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = GAP;
            if v_slider(ui, &mut self.speed, &mut self.speed_active, 1.0, limit, 1.0, "SPEED", w, h) {
                patch.insert("speed".into(), serde_json::Value::from(self.speed));
            }
            if v_slider(ui, &mut self.depth, &mut self.depth_active, 0.0, travel, 5.0, "DEPTH", w, h) {
                patch.insert("depth".into(), serde_json::Value::from(self.depth));
            }
            if v_slider(ui, &mut self.stroke, &mut self.stroke_active, 0.0, stroke_max, 5.0, "STROKE", w, h) {
                patch.insert("stroke".into(), serde_json::Value::from(self.stroke));
            }
        });
        if !patch.is_empty() {
            self.send_state(serde_json::Value::Object(patch));
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
            .fill(panel_alpha_of(self.config.panel_alpha))
            .stroke(egui::Stroke::new(1.0, border()))
            .corner_radius(CornerRadius::same(16))
            .inner_margin(egui::Margin::same(10));

        egui::CentralPanel::default().frame(panel).show(ui, |ui| {
            self.grip(ui);
            ui.add_space(4.0);
            self.big_button(ui);
            ui.add_space(8.0);
            self.sliders_row(ui);
        });

        // Keep the status dot and the values alive while the pointer is idle.
        ctx.request_repaint_after(Duration::from_millis(150));
    }
}

/// A tall, fat vertical slider that prints its value in the thumb.
///
/// Written by hand instead of using `egui::Slider` because that widget fixes the
/// rail thickness and the handle size; this one needs a rail wide enough to hit
/// from across a room and a thumb big enough to read at a glance.
fn v_slider(
    ui: &mut egui::Ui,
    value: &mut f32,
    active: &mut bool,
    min: f32,
    max: f32,
    step: f32,
    label: &str,
    width: f32,
    height: f32,
) -> bool {
    let mut sent = false;
    ui.vertical(|ui| {
        let (label_rect, _) = ui.allocate_exact_size(egui::vec2(width, 18.0), Sense::hover());
        let galley = ui.painter().layout_no_wrap(
            label.to_owned(),
            FontId::proportional(14.5),
            label_c(),
        );
        let pill = Rect::from_center_size(label_rect.center(), galley.size() + egui::vec2(14.0, 6.0));
        ui.painter().rect_filled(pill, CornerRadius::same(7), Color32::from_rgba_unmultiplied(8, 10, 14, 150));
        ui.painter().galley(label_rect.center() - galley.size() / 2.0, galley, label_c());

        let (rect, resp) = ui.allocate_exact_size(egui::vec2(width, height), Sense::click_and_drag());
        *active = resp.dragged() || resp.is_pointer_button_down_on();

        let rail_w = (rect.width() * 0.72).max(24.0);
        let rail = Rect::from_center_size(rect.center(), egui::vec2(rail_w, rect.height()));
        let radius = CornerRadius::same((rail_w / 2.0).round().clamp(0.0, 255.0) as u8);

        // The thumb travels between the rail ends, so map the pointer over that
        // same range and the thumb always lands under it.
        let travel_top = rail.top() + THUMB_H / 2.0;
        let travel_bot = rail.bottom() - THUMB_H / 2.0;
        let span = (travel_bot - travel_top).max(1.0);

        if (resp.dragged() || resp.clicked()) && max > min {
            if let Some(p) = resp.interact_pointer_pos() {
                let t = ((travel_bot - p.y) / span).clamp(0.0, 1.0);
                let raw = min + t * (max - min);
                let quantised = ((raw / step).round() * step).clamp(min, max);
                if (quantised - *value).abs() > f32::EPSILON {
                    *value = quantised;
                }
            }
        }

        let t = if max > min {
            ((*value - min) / (max - min)).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let thumb_y = travel_bot - span * t;

        let painter = ui.painter();
        painter.rect_filled(rail, radius, rail_c());
        if t > 0.001 {
            let filled = Rect::from_min_max(egui::pos2(rail.left(), thumb_y), rail.max);
            painter.rect_filled(filled, radius, fill_c());
        }

        let thumb = Rect::from_center_size(
            egui::pos2(rect.center().x, thumb_y),
            egui::vec2(rect.width() * 0.94, THUMB_H),
        );
        let hot = resp.hovered() || resp.dragged();
        painter.rect_filled(thumb, CornerRadius::same(11), if hot { thumb_hot_c() } else { thumb_c() });
        painter.rect_stroke(
            thumb,
            CornerRadius::same(11),
            egui::Stroke::new(1.0, Color32::from_rgba_unmultiplied(10, 12, 18, 90)),
            egui::StrokeKind::Inside,
        );
        painter.text(
            thumb.center(),
            Align2::CENTER_CENTER,
            format!("{:.0}", *value),
            FontId::proportional(17.0),
            thumb_text(),
        );

        if hot {
            ui.ctx().set_cursor_icon(egui::CursorIcon::ResizeVertical);
        }
        if resp.drag_stopped() || (resp.clicked() && !resp.dragged()) {
            sent = true;
        }
    });
    sent
}

/// Parse `"Ctrl+Shift+O"` into egui modifiers plus a key. Unknown key names fall
/// back to `O`, which keeps the escape hatch usable even if the config is
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
