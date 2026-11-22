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
//!   - four tall vertical sliders (Speed, Depth, Stroke, Sensation), each a
//!     quarter of the width
//!   - the value is printed inside the slider thumb, so no readout row is needed
//!   - one small status dot, no status text
//!   - one slim bottom row: a compact pattern selector and a small square
//!     depth-setup button, both kept small and clear of Start/Stop
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
    // A whisper of tint, nothing more: the machine's own view should not be dimmed.
    Color32::from_rgba_unmultiplied(18, 20, 24, alpha)
}
fn border() -> Color32 {
    Color32::from_rgba_unmultiplied(70, 74, 82, 34)
}
fn rail_c() -> Color32 {
    // Barely a shade from the panel: the fader is found by its outline, not its mass.
    Color32::from_rgba_unmultiplied(54, 58, 66, 48)
}
fn fill_c() -> Color32 {
    Color32::from_rgba_unmultiplied(96, 100, 110, 70)
}
fn thumb_c() -> Color32 {
    Color32::from_rgba_unmultiplied(42, 45, 52, 132)
}
fn thumb_hot_c() -> Color32 {
    Color32::from_rgba_unmultiplied(30, 32, 38, 190)
}
fn thumb_text() -> Color32 {
    Color32::from_rgba_unmultiplied(210, 214, 222, 255)
}
fn label_c() -> Color32 {
    // Very dark grey, not white: over a bright view this reads as a shadow, not a label.
    Color32::from_rgba_unmultiplied(34, 36, 42, 235)
}
fn grip_c() -> Color32 {
    Color32::from_rgba_unmultiplied(46, 49, 56, 60)
}
fn start_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(42, 140, 92, 150)
}
fn stop_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(172, 55, 55, 150)
}
fn disabled_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(72, 74, 86, 110)
}
// The compact selectors in the bottom row: translucent so the desktop keeps
// showing through, like every other surface in this panel.
fn combo_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(40, 43, 50, 62)
}
fn combo_hot() -> Color32 {
    Color32::from_rgba_unmultiplied(56, 60, 68, 96)
}
// Depth-setup mode is amber, deliberately neither the Start green nor the Stop
// red, so the small button can never be read as Start/Stop.
fn setup_on_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(150, 112, 34, 190)
}
fn setup_off_fill() -> Color32 {
    Color32::from_rgba_unmultiplied(60, 63, 70, 70)
}
fn glyph_c() -> Color32 {
    Color32::from_rgba_unmultiplied(32, 34, 40, 235)
}

const OK: Color32 = Color32::from_rgb(120, 220, 140);
const WARN: Color32 = Color32::from_rgb(240, 200, 90);
const ERR: Color32 = Color32::from_rgb(240, 110, 110);

const BUTTON_H: f32 = 44.0;
const THUMB_H: f32 = 34.0;
const GAP: f32 = 10.0;
/// Height of the slim bottom row. Also reserved by `sliders_row` so the row can
/// never push the sliders off the bottom of the panel.
const COMBO_ROW_H: f32 = 34.0;
/// The depth-setup button is deliberately small: it is a mode toggle, not a
/// second Start/Stop.
const SETUP_SIZE: f32 = 32.0;

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
    /// Sensation is bipolar: -100 .. 100, step 5.
    sensation: f32,
    sensation_active: bool,
    /// The selected pattern index, mirrored from `state.pattern`. The combo shows
    /// `pattern_name` from the device; this is only the local selection.
    pattern: i32,

    seeded: bool,
    last_send: Instant,

    click_through: bool,
    shortcut: (egui::Modifiers, egui::Key),
    config: Config,
    config_path: PathBuf,
    last_config_check: Instant,
    /// The Win32 window handle (0 elsewhere), so the no-activate style can be put
    /// back after winit rewrites the window styles.
    hwnd: isize,
    /// While the grip is being dragged on Windows: the offset from the pointer to
    /// the window's top-left corner. `None` when no drag of ours is running.
    grip_grab: Option<(i32, i32)>,
}

impl OverlayApp {
    pub fn new(
        cc: &eframe::CreationContext<'_>,
        config: Config,
        config_path: PathBuf,
        hwnd: isize,
    ) -> Self {
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
            sensation: 0.0,
            sensation_active: false,
            pattern: 0,
            seeded: false,
            last_send: Instant::now()
                .checked_sub(Duration::from_secs(10))
                .unwrap_or_else(Instant::now),
            click_through: config.click_through,
            shortcut: (mods, key),
            config,
            config_path,
            last_config_check: Instant::now(),
            hwnd,
            grip_grab: None,
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
            self.sensation = s.sensation;
            self.pattern = s.pattern;
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
        if !self.sensation_active {
            self.sensation = s.sensation;
        }
        // The combo has no drag state: the in-flight guard above is what stops a
        // just-chosen pattern from snapping back to the old device echo.
        self.pattern = s.pattern;
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
    ///
    /// On Windows the overlay drags itself (`begin_grip_drag`); everywhere else
    /// the window system does it. See `begin_grip_drag` for why the split exists:
    /// the OS move loop does nothing for a window that must never be activated.
    fn grip(&mut self, ui: &mut egui::Ui) {
        let w = ui.available_width();
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, 22.0), Sense::drag());
        let painter = ui.painter();
        let pill = Rect::from_center_size(rect.center(), egui::vec2(64.0, 5.0));
        painter.rect_filled(pill, CornerRadius::same(2), grip_c());
        if resp.drag_started() {
            self.grip_grab = crate::begin_grip_drag(self.hwnd);
            if self.grip_grab.is_none() {
                ui.ctx().send_viewport_cmd(egui::ViewportCommand::StartDrag);
            }
        }
        if resp.dragged() {
            if let Some(grab) = self.grip_grab {
                crate::follow_grip_drag(self.hwnd, grab);
            }
        }
        if resp.drag_stopped() && self.grip_grab.take().is_some() {
            crate::end_grip_drag(self.hwnd);
        }
        if resp.hovered() {
            ui.ctx().set_cursor_icon(egui::CursorIcon::Grab);
        }
    }

    /// One button, taller than a normal one, that starts and stops.
    fn big_button(&mut self, ui: &mut egui::Ui) {
        // A small square like the depth-setup button: icon only, no word.
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(SETUP_SIZE, SETUP_SIZE), Sense::click());
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
        painter.rect_filled(rect, CornerRadius::same(8), fill);
        if resp.hovered() && can_act {
            painter.rect_filled(rect, CornerRadius::same(8), Color32::from_white_alpha(24));
        }
        let ink = Color32::from_rgb(242, 244, 252);
        let c = rect.center();
        if running {
            let r = Rect::from_center_size(c, egui::vec2(SETUP_SIZE * 0.34, SETUP_SIZE * 0.34));
            painter.rect_filled(r, CornerRadius::same(2), ink);
        } else {
            let t = SETUP_SIZE * 0.28;
            painter.add(egui::Shape::convex_polygon(
                vec![
                    egui::pos2(c.x - t * 0.7, c.y - t),
                    egui::pos2(c.x - t * 0.7, c.y + t),
                    egui::pos2(c.x + t, c.y),
                ],
                ink,
                egui::Stroke::NONE,
            ));
        }

        let dot_color = if !connected {
            ERR
        } else if running || homed {
            OK
        } else {
            WARN
        };
        let dot = egui::pos2(rect.right() + 12.0, rect.center().y);
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

        // Reserve the label row above the sliders and the slim selector row
        // below, so the sliders can never push that row off the panel.
        let L = self.config.layout.clone();
        let gap = L.gap;
        let btn = L.button_size;
        let h = if L.slider_height > 1.0 {
            L.slider_height
        } else {
            (ui.available_height() - 2.0 * (btn + gap) - 24.0).max(70.0)
        };
        let w = if L.slider_width > 1.0 {
            L.slider_width
        } else {
            ((ui.available_width() - gap * 3.0) / 4.0).floor().max(30.0)
        };
        let THUMB = L.thumb_height;
        let CAPS = L.cap_size;

        let mut patch = serde_json::Map::new();
        let pad = ((w - btn) * 0.5).max(0.0);
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = gap;
            for col in 0..4usize {
                ui.vertical(|ui| {
                    // Each column owns its own controls: column 1 carries Start and
                    // Pattern, column 3 carries T and the depth crosshair, and the
                    // middle columns stay clear so the faders line up.
                    match col {
                        0 => {
                            ui.horizontal(|ui| {
                                ui.add_space(pad);
                                self.big_button(ui);
                            });
                        }
                        2 => {
                            ui.horizontal(|ui| {
                                ui.add_space(pad);
                                self.keep_start_button(ui);
                            });
                        }
                        _ => {
                            ui.add_space(btn);
                        }
                    }
                    ui.add_space(GAP);
                    match col {
                        0 => {
                            if v_slider(ui, &mut self.speed, &mut self.speed_active, 1.0, limit, 1.0, "SPEED", w, h, THUMB, CAPS, L.label_font) {
                                patch.insert("speed".into(), serde_json::Value::from(self.speed));
                            }
                        }
                        1 => {
                            if v_slider(ui, &mut self.depth, &mut self.depth_active, 0.0, travel, 5.0, "DEPTH", w, h, THUMB, CAPS, L.label_font) {
                                patch.insert("depth".into(), serde_json::Value::from(self.depth));
                            }
                        }
                        2 => {
                            if v_slider(ui, &mut self.stroke, &mut self.stroke_active, 0.0, stroke_max, 5.0, "STROKE", w, h, THUMB, CAPS, L.label_font) {
                                patch.insert("stroke".into(), serde_json::Value::from(self.stroke));
                            }
                        }
                        _ => {
                            // Sensation is bipolar (-100 .. 100), same widget.
                            if v_slider(ui, &mut self.sensation, &mut self.sensation_active, -100.0, 100.0, 5.0, "SENSATION", w, h, THUMB, CAPS, L.label_font) {
                                patch.insert("sensation".into(), serde_json::Value::from(self.sensation));
                            }
                        }
                    }
                    ui.add_space(GAP);
                    match col {
                        0 => {
                            ui.horizontal(|ui| {
                                ui.add_space(pad);
                                let _ = self.pattern_button(ui);
                            });
                        }
                        2 => {
                            ui.horizontal(|ui| {
                                ui.add_space(pad);
                                self.setup_button(ui);
                            });
                        }
                        _ => {
                            ui.add_space(btn);
                        }
                    }
                });
            }
        });
        if !patch.is_empty() {
            self.send_state(serde_json::Value::Object(patch));
        }
    }

    /// The slim bottom row: a compact pattern selector on the left and, at the
    /// far end, the small square depth-setup button. Both are kept small and
    /// clear of the full-width Start/Stop button so they cannot be confused
    /// with it.
    fn controls_row(&mut self, ui: &mut egui::Ui) {
        // While a command is in flight, show the pattern we just chose instead of
        // the device's previous echo, or the combo flickers back for a frame.
        let in_flight = self.last_send.elapsed() < Duration::from_millis(600);
        let device_name = self.snapshot.state.pattern_name.clone();
        let selected = if !in_flight && !device_name.is_empty() {
            device_name
        } else {
            pattern_name(self.pattern).to_owned()
        };

        let mut chosen = self.pattern;
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = GAP;
            ui.spacing_mut().interact_size.y = COMBO_ROW_H - 8.0;
            // Small font and a translucent fill: it is a selector, not a feature.
            ui.style_mut()
                .text_styles
                .insert(egui::TextStyle::Button, FontId::proportional(13.0));
            let visuals = ui.visuals_mut();
            visuals.widgets.inactive.weak_bg_fill = combo_fill();
            visuals.widgets.hovered.weak_bg_fill = combo_hot();
            visuals.widgets.active.weak_bg_fill = combo_hot();
            visuals.window_fill = Color32::from_rgba_unmultiplied(16, 18, 24, 246);

            // No label: the pattern name in the box says what it is.
            let combo_w = (ui.available_width() - 2.0 * SETUP_SIZE - 2.0 * GAP).max(60.0);
            egui::ComboBox::from_id_salt("pattern")
                .width(combo_w)
                .selected_text(selected)
                .show_ui(ui, |ui| {
                    for (i, name) in crate::api::FALLBACK_PATTERNS.iter().enumerate() {
                        ui.selectable_value(&mut chosen, i as i32, *name);
                    }
                });

            self.setup_button(ui);
            self.keep_start_button(ui);
        });

        if chosen != self.pattern {
            self.pattern = chosen;
            self.send_state(serde_json::json!({ "pattern": self.pattern }));
        }
    }

    /// A small square button that enters and leaves the firmware's interactive
    /// depth-setup mode (`{"setup": 1}` / `{"setup": 0}`). In that mode the
    /// machine creeps to the depth position at 10 mm/s and holds, so this button
    /// *does* move the machine; the tooltip says so. It lights up amber while
    /// `state.setup != 0`, and it is a glyph rather than a word so it stays
    /// small and can never be read as Start/Stop.
    /// Pattern selection without a combo box: a small square that opens a menu.
    fn pattern_button(&mut self, ui: &mut egui::Ui) -> egui::Response {
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(SETUP_SIZE, SETUP_SIZE), Sense::click());
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(8), combo_fill());
        if resp.hovered() {
            painter.rect_filled(rect, CornerRadius::same(8), combo_hot());
        }
        let ink = Color32::from_rgb(226, 230, 244);
        let c = rect.center();
        let t = SETUP_SIZE * 0.28;
        for i in -1..=1 {
            let y = c.y + i as f32 * t * 0.62;
            painter.line_segment(
                [egui::pos2(c.x - t, y), egui::pos2(c.x + t, y)],
                egui::Stroke::new(2.0, ink),
            );
        }
        let current = self.pattern;
        let mut chosen = current;
        egui::Popup::menu(&resp).show(|ui| {
            ui.style_mut().text_styles.insert(
                egui::TextStyle::Button,
                FontId::proportional(12.0),
            );
            for (i, name) in crate::api::FALLBACK_PATTERNS.iter().enumerate() {
                if ui.selectable_label(chosen == i as i32, *name).clicked() {
                    chosen = i as i32;
                }
            }
        });
        if chosen != current {
            self.pattern = chosen;
            self.send_state(serde_json::json!({ "pattern": chosen }));
        }
        resp.on_hover_text("pattern")
    }

    fn keep_start_button(&mut self, ui: &mut egui::Ui) {
        use std::sync::atomic::{AtomicBool, Ordering};
        static ON: AtomicBool = AtomicBool::new(false);
        let on = ON.load(Ordering::Relaxed);
        let (rect, resp) = ui.allocate_exact_size(egui::vec2(SETUP_SIZE, SETUP_SIZE), Sense::click());
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(8),
            if on { Color32::from_rgba_unmultiplied(206, 209, 216, 105) } else { combo_fill() });
        let ink = if on { Color32::from_rgb(18, 20, 26) } else { Color32::from_rgb(226, 230, 244) };
        let c = rect.center();
        let t = SETUP_SIZE * 0.26;
        // a bar with a stem: the stroke start is pinned
        painter.line_segment([egui::pos2(c.x - t, c.y - t * 0.8), egui::pos2(c.x + t, c.y - t * 0.8)],
                             egui::Stroke::new(2.0, ink));
        painter.line_segment([egui::pos2(c.x, c.y - t * 0.8), egui::pos2(c.x, c.y + t)],
                             egui::Stroke::new(2.0, ink));
        if resp.clicked() {
            ON.store(!on, Ordering::Relaxed);
            self.send_state(serde_json::json!({ "keep_stroke_start": !on }));
        }
        resp.on_hover_text(if on { "stroke start is pinned - click to release" }
                           else { "pin the stroke start" });
    }

    fn setup_button(&mut self, ui: &mut egui::Ui) {
        let connected = self.snapshot.connected;
        let active = self.snapshot.state.setup != 0;
        let (rect, resp) =
            ui.allocate_exact_size(egui::vec2(SETUP_SIZE, SETUP_SIZE), Sense::click());

        let fill = if active {
            setup_on_fill()
        } else if connected {
            setup_off_fill()
        } else {
            disabled_fill()
        };
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(9), fill);
        if resp.hovered() && connected {
            painter.rect_filled(rect, CornerRadius::same(9), Color32::from_white_alpha(26));
        }

        // A crosshair, drawn by hand: the crate bundles no icon font, so a text
        // glyph like U+2316 could render as a blank box. Drawing it is safe.
        let c = rect.center();
        let r = SETUP_SIZE * 0.26;
        let arm = r + 4.0;
        let col = glyph_c();
        painter.circle_stroke(c, r, egui::Stroke::new(1.6, col));
        painter.line_segment(
            [egui::pos2(c.x - arm, c.y), egui::pos2(c.x + arm, c.y)],
            egui::Stroke::new(1.6, col),
        );
        painter.line_segment(
            [egui::pos2(c.x, c.y - arm), egui::pos2(c.x, c.y + arm)],
            egui::Stroke::new(1.6, col),
        );
        painter.circle_filled(c, 1.8, col);

        if resp.clicked() && connected {
            self.send_state(serde_json::json!({ "setup": if active { 0 } else { 1 } }));
        }
        if resp.hovered() {
            let hint = if !connected {
                "depth setup: no connection to the machine"
            } else if active {
                "Depth setup: ON. The machine is creeping to the depth position and \
                 holding there. Press to stop and release it."
            } else {
                "Set depth: the machine moves - it creeps to the current depth \
                 position at 10 mm/s and holds there so you can look at it. Press \
                 again to stop. This is not Start/Stop."
            };
            resp.on_hover_text(hint);
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
        // winit rebuilds the window styles whenever one of its own flags changes
        // (a click-through toggle does) and its style set knows nothing about the
        // no-activate bit, so it is put back here. Cheap: one GetWindowLongW.
        if crate::keep_unactivated(self.hwnd) {
            eprintln!("overlay-hud: no-activate style restored after a style rewrite");
        }
        self.drain_snapshots();
        self.refresh_from_device(&ctx);
        self.poll_config_file(&ctx);
        self.handle_shortcut(&ctx);

        // Responsive: the panel is laid out in DESIGN points and scaled to whatever
        // size the window is, so resizing keeps it proportionate and legible instead
        // of clipping or stranding small text.
        // Scaling zooms the whole UI, which changes content_rect, which changes the
        // zoom: without a dead band that is a feedback loop and the panel jitters.
        // Only rescale when the size has genuinely changed by more than 12 percent.
        // Two shapes: a small strip of two buttons, and the full panel. The strip
        // opens when the pointer arrives and closes again when it leaves, so the
        // overlay is out of the way by default and one hover away when wanted.
        use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
        static EXPANDED: AtomicBool = AtomicBool::new(false);
        static LEFT_AT: AtomicU64 = AtomicU64::new(0);
        // Set while drawing the three-line button: only that button opens the panel,
        // so the power button stays put and stays clickable when the panel is shut.
        static OVER_OPENER: AtomicBool = AtomicBool::new(false);

        const OPEN_W: f32 = 250.0;
        const OPEN_H: f32 = 800.0;
        const SHUT_W: f32 = 250.0;
        const SHUT_H: f32 = 62.0;

        let now_ms = (ctx.input(|i| i.time) * 1000.0) as u64;
        let hovered = ctx.input(|i| i.pointer.hover_pos().is_some());
        let dragging = self.speed_active
            || self.depth_active
            || self.stroke_active
            || self.sensation_active;

        if hovered || dragging {
            LEFT_AT.store(0, Ordering::Relaxed);
        } else if LEFT_AT.load(Ordering::Relaxed) == 0 {
            LEFT_AT.store(now_ms, Ordering::Relaxed);
        }

        let mut expanded = EXPANDED.load(Ordering::Relaxed);
        if !expanded && OVER_OPENER.load(Ordering::Relaxed) {
            expanded = true;
        }
        let left = LEFT_AT.load(Ordering::Relaxed);
        if expanded && !hovered && !dragging && left != 0 && now_ms.saturating_sub(left) > 400 {
            expanded = false;
        }
        EXPANDED.store(expanded, Ordering::Relaxed);

        let want = if expanded {
            egui::vec2(OPEN_W, OPEN_H)
        } else {
            egui::vec2(SHUT_W, SHUT_H)
        };
        if let Some(sz) = ctx.input(|i| i.viewport().inner_rect.map(|r| r.size())) {
            if (sz.x - want.x).abs() > 2.0 || (sz.y - want.y).abs() > 2.0 {
                ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(want));
            }
        }

        const DESIGN_W: f32 = 320.0;
        let cur = ctx.zoom_factor();
        let win_w = ctx.content_rect().width().max(1.0) * cur;
        // Shrink when the window is small, but never enlarge: 1.0 is the ceiling.
        let want_zoom = (win_w / DESIGN_W).clamp(0.55, 1.0);
        if (cur - want_zoom).abs() > 0.12 * cur {
            ctx.set_zoom_factor(want_zoom);
        }

        let panel = egui::Frame::NONE.shadow(egui::epaint::Shadow::NONE)
            .fill(panel_alpha_of(self.config.panel_alpha))
            .stroke(egui::Stroke::new(1.0, border()))
            .corner_radius(CornerRadius::same(16))
            .inner_margin(egui::Margin::same(10));

        egui::CentralPanel::default().frame(panel).show(ui, |ui| {
            if expanded {
                OVER_OPENER.store(false, Ordering::Relaxed);
                self.grip(ui);
                ui.add_space(4.0);
                self.sliders_row(ui);
            } else {
                // Shut: the power button and the three-line opener, nothing else.
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 8.0;
                    self.big_button(ui);
                    let op = self.pattern_button(ui);
                    OVER_OPENER.store(op.hovered(), Ordering::Relaxed);
                });
            }
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
    thumb_h: f32,
    cap: f32,
    label_font: f32,
) -> bool {
    let mut sent = false;
    // Room on the left for a vertical label; the rail shifts right of it. Every
    // part that belongs to the rail - the value, both nudge caps and the handle -
    // is placed on the rail centre `cx`, never on the column centre: the label
    // column pushes the rail right, so anything centred on the column hangs off
    // the rail's left edge by half the label column (6.5 points, about 4 pixels
    // at the usual zoom - the handle used to, which is what made it look off).
    const LABEL_COL: f32 = 13.0;
    ui.vertical(|ui| {
        // The value sits above the rail, where it can be read without covering
        // anything, instead of inside the handle.
        let (val_rect, _) = ui.allocate_exact_size(egui::vec2(width, 17.0), Sense::hover());
        let vgalley = ui.painter().layout_no_wrap(
            format!("{:.0}", *value),
            FontId::proportional(12.5),
            label_c(),
        );
        ui.painter().galley(
            egui::pos2(val_rect.center().x + LABEL_COL * 0.5, val_rect.center().y) - vgalley.size() / 2.0,
            vgalley,
            label_c(),
        );

        let (rect, resp) = ui.allocate_exact_size(egui::vec2(width, height), Sense::click_and_drag());
        *active = resp.dragged() || resp.is_pointer_button_down_on();

        let rail_w = ((rect.width() - LABEL_COL) * 0.62).clamp(16.0, 26.0);
        // Two nudge caps: one step per press, for fine work at a distance.
        let CAP: f32 = cap;
        let inner_h = (rect.height() - 2.0 * (CAP + 3.0)).max(24.0);
        let cx = rect.center().x + LABEL_COL * 0.5;
        let rail = Rect::from_center_size(egui::pos2(cx, rect.center().y), egui::vec2(rail_w, inner_h));
        let up_rect = Rect::from_center_size(
            egui::pos2(cx, rect.top() + CAP * 0.5),
            egui::vec2(CAP, CAP),
        );
        let dn_rect = Rect::from_center_size(
            egui::pos2(cx, rect.bottom() - CAP * 0.5),
            egui::vec2(CAP, CAP),
        );
        let up_resp = ui.interact(up_rect, ui.id().with((label, "nudge_up")), Sense::click());
        let dn_resp = ui.interact(dn_rect, ui.id().with((label, "nudge_dn")), Sense::click());
        if up_resp.clicked() {
            *value = (*value + step).clamp(min, max);
            sent = true;
        }
        if dn_resp.clicked() {
            *value = (*value - step).clamp(min, max);
            sent = true;
        }
        for (r, up, hot_cap) in [(up_rect, true, up_resp.hovered()), (dn_rect, false, dn_resp.hovered())] {
            ui.painter().rect_filled(
                r,
                CornerRadius::same(6),
                if hot_cap { combo_hot() } else { combo_fill() },
            );
            let ink = Color32::from_rgb(226, 230, 244);
            let c = r.center();
            let a = 3.5;
            let pts = if up {
                vec![egui::pos2(c.x - a, c.y + a * 0.6), egui::pos2(c.x + a, c.y + a * 0.6), egui::pos2(c.x, c.y - a * 0.7)]
            } else {
                vec![egui::pos2(c.x - a, c.y - a * 0.6), egui::pos2(c.x + a, c.y - a * 0.6), egui::pos2(c.x, c.y + a * 0.7)]
            };
            ui.painter().add(egui::Shape::convex_polygon(pts, ink, egui::Stroke::NONE));
        }
        let lgalley = ui.painter().layout_no_wrap(
            label.to_owned(),
            FontId::proportional(label_font),
            label_c(),
        );
        let lpos = egui::pos2(
            rect.left() + LABEL_COL * 0.5 + 4.0,
            rect.center().y + lgalley.size().x * 0.5,
        );
        ui.painter().add(
            egui::epaint::TextShape::new(lpos, lgalley, label_c())
                .with_angle(-std::f32::consts::FRAC_PI_2),
        );
        let radius = CornerRadius::same((rail_w / 2.0).round().clamp(0.0, 255.0) as u8);

        // The thumb travels between the rail ends, so map the pointer over that
        // same range and the thumb always lands under it.
        let travel_top = rail.top() + thumb_h * 0.6;
        let travel_bot = rail.bottom() - thumb_h * 0.6;
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

        // The handle is exactly as wide as its rail: one continuous instrument
        // rather than a fat block on a thin stick. It rides the rail centre, so
        // it covers the rail and nothing else.
        let thumb = Rect::from_center_size(
            egui::pos2(cx, thumb_y),
            egui::vec2(rail_w, thumb_h * 2.1),
        );
        let hot = resp.hovered() || resp.dragged();
        painter.rect_filled(thumb, CornerRadius::same(11), if hot { thumb_hot_c() } else { thumb_c() });
        painter.rect_stroke(
            thumb,
            CornerRadius::same(11),
            egui::Stroke::new(1.0, Color32::from_rgba_unmultiplied(10, 12, 18, 90)),
            egui::StrokeKind::Inside,
        );
        // The handle is a grip, nothing more: the number lives above the rail.
        if hot {
            painter.line_segment(
                [egui::pos2(thumb.center().x - 5.0, thumb.center().y),
                 egui::pos2(thumb.center().x + 5.0, thumb.center().y)],
                egui::Stroke::new(1.5, Color32::from_rgba_unmultiplied(10, 12, 18, 90)),
            );
        }

        if hot {
            ui.ctx().set_cursor_icon(egui::CursorIcon::ResizeVertical);
        }
        if resp.drag_stopped() || (resp.clicked() && !resp.dragged()) {
            sent = true;
        }
    });
    sent
}

/// The firmware pattern name for an index, or a neutral placeholder when the
/// device reports an index the compiled firmware does not have.
fn pattern_name(index: i32) -> &'static str {
    if index >= 0 {
        crate::api::FALLBACK_PATTERNS
            .get(index as usize)
            .copied()
            .unwrap_or("Pattern")
    } else {
        "Pattern"
    }
}

/// A small text label on a dark translucent pill, matching the slider labels.
/// Returns the width it consumed, for callers that lay out around it.
fn label_pill(ui: &mut egui::Ui, text: &str, size: f32) -> f32 {
    let galley =
        ui.painter()
            .layout_no_wrap(text.to_owned(), FontId::proportional(size), label_c());
    let w = galley.size().x + 14.0;
    let (rect, _) = ui.allocate_exact_size(egui::vec2(w, 20.0), Sense::hover());
    let pill = Rect::from_center_size(rect.center(), galley.size() + egui::vec2(14.0, 6.0));
    ui.painter().rect_filled(
        pill,
        CornerRadius::same(7),
        Color32::from_rgba_unmultiplied(8, 10, 14, 150),
    );
    ui.painter()
        .galley(rect.center() - galley.size() / 2.0, galley, label_c());
    w
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
