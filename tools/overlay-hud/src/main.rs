//! OSSM-Stroke overlay HUD.
//!
//! A small, frameless, transparent, always-on-top control panel for the
//! OSSM-Stroke machine. It polls the device REST API from a background thread
//! and mirrors the controls of `interface/src/routes/stroke/Stroke.svelte`:
//! Start/Stop, Speed/Depth/Stroke/Sensation sliders, a compact pattern selector
//! and the small depth-setup button. It does not stream the event WebSocket.
//!
//! Usage:
//!   overlay-hud                     run the overlay
//!   overlay-hud --toggle-click-through
//!                                   flip click-through in the config and exit
//!   overlay-hud --help

mod api;
mod app;
mod config;
mod worker;

use eframe::egui;

fn main() -> eframe::Result<()> {
    let args: Vec<String> = std::env::args().collect();

    if args.iter().any(|a| a == "--help" || a == "-h") {
        print_help();
        return Ok(());
    }
    if args.iter().any(|a| a == "--toggle-click-through") {
        match config::toggle_click_through() {
            Ok(value) => {
                println!("click_through = {value}");
                std::process::exit(0);
            }
            Err(e) => {
                eprintln!("overlay-hud: {e}");
                std::process::exit(2);
            }
        }
    }
    if args.iter().any(|a| a == "--probe") {
        // Headless smoke test of the REST client: sign in, read the state and
        // settings, and echo the state the firmware returns for a no-op command.
        // Proves the device link without a display.
        let (cfg, _) = match config::Config::load() {
            Ok(v) => v,
            Err(e) => {
                eprintln!("overlay-hud: config: {e}");
                std::process::exit(2);
            }
        };
        match probe(&cfg) {
            Ok(()) => std::process::exit(0),
            Err(e) => {
                eprintln!("overlay-hud: probe failed: {e}");
                std::process::exit(1);
            }
        }
    }

    let (cfg, config_path) = match config::Config::load() {
        Ok(v) => v,
        Err(e) => {
            eprintln!("overlay-hud: config: {e}");
            std::process::exit(2);
        }
    };
    eprintln!(
        "overlay-hud: device {} as {} (config {})",
        cfg.device,
        cfg.username,
        config_path.display()
    );

    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("OSSM Stroke HUD")
            .with_app_id("ossm-overlay-hud")
            .with_inner_size([cfg.window_width, cfg.window_height])
            .with_min_inner_size([320.0, 190.0])
            .with_decorations(false)
            .with_transparent(true)
            .with_resizable(true)
            .with_always_on_top()
            .with_taskbar(false)
            // Never take the foreground, not even at start-up: Windows brings the
            // taskbar forward the moment a window that is not the fullscreen one
            // is activated, and the overlay is meant to sit on top of a video
            // without the shell noticing it arrived. Shown with SW_SHOWNOACTIVATE.
            .with_active(false)
            .with_mouse_passthrough(cfg.click_through),
        ..Default::default()
    };

    let cfg_for_app = cfg.clone();
    hide_own_console();
    eframe::run_native(
        "OSSM Stroke HUD",
        native_options,
        Box::new(move |cc| {
            let hwnd = own_window(cc);
            remove_drop_shadow(hwnd);
            let _ = keep_unactivated(hwnd);
            Ok(Box::new(app::OverlayApp::new(cc, cfg_for_app, config_path, hwnd)) as Box<dyn eframe::App>)
        }),
    )
}

/// The Win32 handle of the window eframe just created, as a plain integer so the
/// type can be carried on platforms that have no such thing (`0` there).
///
/// `CreationContext` implements `raw_window_handle::HasWindowHandle`, so this
/// needs no winit or windows-sys dependency.
#[cfg(windows)]
fn own_window(cc: &eframe::CreationContext<'_>) -> isize {
    use core::ffi::c_void;
    use raw_window_handle::{HasWindowHandle, RawWindowHandle};

    match cc.window_handle().map(|h| h.as_raw()) {
        Ok(RawWindowHandle::Win32(handle)) => handle.hwnd.get() as *mut c_void as isize,
        _ => 0,
    }
}

#[cfg(not(windows))]
fn own_window(_cc: &eframe::CreationContext<'_>) -> isize {
    0
}

/// Windows draws a system drop shadow around every window that still carries a
/// frame style, and it draws that shadow *outside* the window rectangle. On a
/// transparent overlay the shadow lands on the video or VR view behind the panel
/// as a grey halo: measured against a bright backdrop, up to 64 of 255 grey
/// levels darker, 47 pixels to each side and 78 pixels below the window.
///
/// `with_has_shadow(false)` cannot switch it off. In winit that option is
/// macOS-only, and winit fakes a frameless window by painting over the
/// non-client area in `WM_NCCALCSIZE` instead of removing `WS_CAPTION` - so DWM
/// sees a normal framed window and keeps drawing the shadow. Turning DWM's
/// non-client rendering off is the documented way to stop it. The setting
/// outlives the window style writes winit makes later, which is what matters
/// here: a click-through toggle rewrites the styles, and the shadow stayed gone
/// across one.
///
/// No dependency is added for this; `dwmapi` is linked on every Windows target.
#[cfg(windows)]
fn remove_drop_shadow(hwnd: isize) {
    use core::ffi::c_void;

    extern "system" {
        fn DwmSetWindowAttribute(
            hwnd: *mut c_void,
            attribute: u32,
            value: *const c_void,
            size: u32,
        ) -> i32;
    }
    const DWMWA_NCRENDERING_POLICY: u32 = 2;
    const DWMNCRP_DISABLED: i32 = 1;

    if hwnd == 0 {
        return;
    }
    // Safety: `hwnd` is the window this process created and still owns; the
    // attribute value points at a local i32 that outlives the call.
    let hr = unsafe {
        DwmSetWindowAttribute(
            hwnd as *mut c_void,
            DWMWA_NCRENDERING_POLICY,
            &DWMNCRP_DISABLED as *const i32 as *const c_void,
            core::mem::size_of::<i32>() as u32,
        )
    };
    if hr != 0 {
        eprintln!("overlay-hud: could not disable the window shadow (hr 0x{:08X})", hr);
    }
}

#[cfg(not(windows))]
fn remove_drop_shadow(_hwnd: isize) {}

/// Windows: make sure this window can never be activated.
///
/// Clicking an overlay must not pull the foreground away from the fullscreen
/// video or game underneath, because the shell answers a foreground change by
/// bringing the taskbar forward - straight through the picture. `WS_EX_NOACTIVATE`
/// keeps the window from being activated by a click while still delivering the
/// mouse messages to it, so the sliders and buttons keep working.
///
/// egui's `with_active(false)` only covers the first show; the style is set here
/// and re-applied whenever winit rewrites the window styles (a click-through
/// toggle does that, and winit builds the style set from its own flags, so the
/// bit is lost each time). The return value says whether it had to restore it.
///
/// The cost, and it is real: a window with this style never takes keyboard focus,
/// so the click-through shortcut cannot fire while the overlay is over another
/// application. Use `overlay-hud --toggle-click-through` from a second process
/// (Stream Deck, AutoHotkey) or edit `config.toml`; the overlay picks the file up
/// within a second.
#[cfg(windows)]
fn keep_unactivated(hwnd: isize) -> bool {
    use core::ffi::c_void;

    extern "system" {
        fn GetWindowLongW(hwnd: *mut c_void, index: i32) -> i32;
        fn SetWindowLongW(hwnd: *mut c_void, index: i32, value: i32) -> i32;
    }
    const GWL_EXSTYLE: i32 = -20;
    const WS_EX_NOACTIVATE: i32 = 0x0800_0000;

    if hwnd == 0 {
        return false;
    }
    let window = hwnd as *mut c_void;
    // Safety: `window` is the window this process created and still owns.
    unsafe {
        let style = GetWindowLongW(window, GWL_EXSTYLE);
        if style & WS_EX_NOACTIVATE != 0 {
            return false;
        }
        SetWindowLongW(window, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
    }
    true
}

#[cfg(not(windows))]
fn keep_unactivated(_hwnd: isize) -> bool {
    false
}

#[cfg(windows)]
#[repr(C)]
struct Point {
    x: i32,
    y: i32,
}

#[cfg(windows)]
#[repr(C)]
struct WinRect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

/// Windows: start moving the window under the pointer.
///
/// This is the overlay's own drag, and it exists because the operating system's
/// move loop does not run for a window with `WS_EX_NOACTIVATE`: winit's
/// `StartDrag` posts `WM_NCLBUTTONDOWN` with `HTCAPTION`, and the window simply
/// stays where it is. Measured on the build box: with the no-activate style the
/// grip drag delivered to the app (44 drag frames) and moved the window 0 pixels;
/// the same drag on the build without the style moved it 36x24, exactly the
/// pointer's own travel.
///
/// The mouse is captured, and the offset from the pointer to the window's corner
/// is kept, so the window follows the pointer at the same grip. Returns that
/// offset; `None` means the caller should fall back to the window system.
#[cfg(windows)]
fn begin_grip_drag(hwnd: isize) -> Option<(i32, i32)> {
    use core::ffi::c_void;

    extern "system" {
        fn GetCursorPos(point: *mut Point) -> i32;
        fn GetWindowRect(window: *mut c_void, rect: *mut WinRect) -> i32;
        fn SetCapture(window: *mut c_void) -> *mut c_void;
    }

    if hwnd == 0 {
        return None;
    }
    let window = hwnd as *mut c_void;
    let mut cursor = Point { x: 0, y: 0 };
    let mut rect = WinRect { left: 0, top: 0, right: 0, bottom: 0 };
    // Safety: `window` is the window this process created and still owns; both
    // out-parameters are valid for the duration of the call.
    unsafe {
        if GetCursorPos(&mut cursor) == 0 || GetWindowRect(window, &mut rect) == 0 {
            return None;
        }
        SetCapture(window);
    }
    Some((rect.left - cursor.x, rect.top - cursor.y))
}

/// Windows: one step of the drag started by [`begin_grip_drag`]. The pointer has
/// moved since the last frame; put the window back under the same grip.
#[cfg(windows)]
fn follow_grip_drag(hwnd: isize, grab: (i32, i32)) {
    use core::ffi::c_void;

    extern "system" {
        fn GetCursorPos(point: *mut Point) -> i32;
        fn SetWindowPos(
            window: *mut c_void,
            after: *mut c_void,
            x: i32,
            y: i32,
            cx: i32,
            cy: i32,
            flags: u32,
        ) -> i32;
    }
    // HWND_TOPMOST, and never take the foreground while moving.
    const HWND_TOPMOST: isize = -1;
    const SWP_NOSIZE: u32 = 0x0001;
    const SWP_NOACTIVATE: u32 = 0x0010;

    if hwnd == 0 {
        return;
    }
    let window = hwnd as *mut c_void;
    let mut cursor = Point { x: 0, y: 0 };
    // Safety: as above; `GetCursorPos` only writes to the local struct.
    unsafe {
        if GetCursorPos(&mut cursor) == 0 {
            return;
        }
        SetWindowPos(
            window,
            HWND_TOPMOST as *mut c_void,
            cursor.x + grab.0,
            cursor.y + grab.1,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE,
        );
    }
}

/// Windows: the drag is over; hand the mouse back.
#[cfg(windows)]
fn end_grip_drag(_hwnd: isize) {
    extern "system" {
        fn ReleaseCapture() -> i32;
    }

    // Safety: releasing a capture this process took, if it took one at all.
    unsafe {
        ReleaseCapture();
    }
}

#[cfg(not(windows))]
fn begin_grip_drag(_hwnd: isize) -> Option<(i32, i32)> {
    None
}

#[cfg(not(windows))]
fn follow_grip_drag(_hwnd: isize, _grab: (i32, i32)) {}

#[cfg(not(windows))]
fn end_grip_drag(_hwnd: isize) {}

/// On Windows the binary stays a console application so that `--probe`,
/// `--help` and `--toggle-click-through` keep their output when run from a
/// shell. That leaves a black console window sitting behind the overlay, which
/// is the one thing an always-on-top panel over a video window must not have.
/// Hiding it at start-up keeps both: a usable command line and a clean overlay.
///
/// No dependency is added for this; the two functions come from kernel32 and
/// user32, which both Windows targets link anyway.
#[cfg(windows)]
fn hide_own_console() {
    use core::ffi::c_void;

    extern "system" {
        fn GetConsoleWindow() -> *mut c_void;
        fn ShowWindow(hwnd: *mut c_void, cmd: i32) -> i32;
    }
    const SW_HIDE: i32 = 0;

    // Safety: both calls take the handle this process already owns, and a null
    // handle (no console attached, for instance when launched from Explorer) is
    // checked before use.
    unsafe {
        let hwnd = GetConsoleWindow();
        if !hwnd.is_null() {
            ShowWindow(hwnd, SW_HIDE);
        }
    }
}

#[cfg(not(windows))]
fn hide_own_console() {}

fn probe(cfg: &config::Config) -> Result<(), String> {
    let client = api::DeviceClient::new(&cfg.device, &cfg.username, &cfg.password)?;
    let token = client.sign_in()?;
    println!("signed in to {} (token {} chars)", cfg.device, token.len());

    let state = client.get_state(&token).map_err(|e| e.to_string())?;
    println!("GET /rest/strokeState ->");
    println!("{state:#?}");

    let settings = client.get_settings(&token).map_err(|e| e.to_string())?;
    println!("GET /rest/strokeSettings ->");
    println!("{settings:#?}");

    // A no-op write, to prove the POST path and show the clamped echo.
    let mut body = serde_json::Map::new();
    body.insert("sensation".to_string(), serde_json::Value::from(state.sensation));
    let patch = serde_json::Value::Object(body);
    let echoed = client.post_state(&token, &patch).map_err(|e| e.to_string())?;
    println!(
        "POST /rest/strokeState {patch} -> run={} speed={} depth={} stroke={} servo_state={} clipping={}",
        echoed.run, echoed.speed, echoed.depth, echoed.stroke, echoed.servo_state, echoed.clipping
    );
    Ok(())
}

fn print_help() {
    println!(
        "OSSM Stroke overlay HUD

USAGE:
    overlay-hud [OPTIONS]

OPTIONS:
    --toggle-click-through   Flip the click-through flag in the config file and
                             exit. Run this from a second process (Stream Deck,
                             AutoHotkey, a shell one-liner) to leave
                             click-through mode while the overlay cannot receive
                             the mouse.
    --probe                  Sign in, print the stroke state and settings and a
                             no-op command echo, then exit (headless smoke test).
    -h, --help               Print this help.

The device address, credentials and the click-through keyboard shortcut are read
from config.toml next to the executable (or ./config.toml, or $OSSM_HUD_CONFIG)."
    );
}
