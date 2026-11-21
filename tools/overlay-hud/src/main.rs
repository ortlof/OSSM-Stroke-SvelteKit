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
            // No OS drop shadow: a faint overlay must not cast one.
            .with_has_shadow(false)
            .with_decorations(false)
            .with_transparent(true)
            .with_resizable(true)
            .with_always_on_top()
            .with_taskbar(false)
            .with_mouse_passthrough(cfg.click_through),
        ..Default::default()
    };

    let cfg_for_app = cfg.clone();
    hide_own_console();
    eframe::run_native(
        "OSSM Stroke HUD",
        native_options,
        Box::new(move |cc| {
            Ok(Box::new(app::OverlayApp::new(cc, cfg_for_app, config_path)) as Box<dyn eframe::App>)
        }),
    )
}

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
