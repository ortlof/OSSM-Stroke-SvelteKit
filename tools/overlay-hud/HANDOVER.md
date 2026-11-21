# Overlay HUD — handover

Everything the next session needs. Written at the end of a long session that built the
firmware side; the Rust client is complete and proven, and has one obvious next job.

## Where it is

- Crate: `tools/overlay-hud/` in this repo (branch `dev`)
- Files: `src/main.rs` (window setup, console hide), `src/app.rs` (all drawing),
  `src/api.rs` (hand-rolled HTTP, no client dependency), `src/config.rs`, `src/worker.rs`
- Config: `config.toml` beside the exe, else `./config.toml`, else `$OSSM_HUD_CONFIG`

## Build

```sh
cd tools/overlay-hud && . "$HOME/.cargo/env"
cargo build --release                                   # Linux
cargo build --release --target x86_64-pc-windows-gnu    # Windows PE32+, no mingw needed
```

Deliverables land in `../../dist/`: `overlay-hud.exe`, `overlay-hud-windows.zip`.

## What it is

Frameless, transparent, always-on-top HUD for laying controls over another window (VR
or a big display). Four vertical sliders — SPEED, DEPTH, STROKE, SENSATION — with the
value drawn inside the handle, a slim pattern selector, a crosshair button for the
depth-setup mode, and START/STOP with a status dot. `panel_alpha` (default 96) sets
translucency. `Ctrl+Shift+O` toggles click-through. The Windows console is hidden with
`GetConsoleWindow` + `ShowWindow` so `--probe` still prints.

## Rules learned the hard way

- **Poll REST, never stream the WebSocket.** Streaming blocked the event loop and made
  the app look like it had killed the device.
- **Judge transparency against a bright backdrop.** Against a dark window an opaque
  panel and a translucent one look identical.
- **Windows without a GPU needs Mesa**: `opengl32.dll` + `libgallium_wgl.dll` beside the
  exe and `GALLIUM_DRIVER=llvmpipe`, or it dies with `egui_glow requires opengl 2.0+`.
- **Starting a GUI over SSH lands in session 0** and draws nothing. Use a scheduled
  task with `/RL HIGHEST`.
- `unzip` is not installed on the dev box; use python `zipfile`.
- **`with_has_shadow(false)` does nothing on Windows.** winit fakes a frameless
  window by painting over the non-client area, so the window keeps `WS_CAPTION`
  and DWM draws its drop shadow *outside* the window rect, over whatever is
  behind the panel. `remove_drop_shadow()` in `main.rs` turns DWM non-client
  rendering off and the halo is gone, including after a click-through toggle
  (which rewrites the window styles). Measure it with a hide/show pixel diff:
  the halo is up to 64/255 grey and 47 px at the sides, 78 px below.

## Proven

Built for Linux and Windows; run on a Windows 11 VM connected to the real machine with
live values; translucent over a bright gradient; no console window; four sliders with
the pattern selector and crosshair present and readable. Screenshots:
`dist/win-4col.png`, `dist/design-v3-sensation-pattern.png`.

## The obvious next job

The firmware now publishes values the HUD does not show, and one of them is a safety
signal:

- `drive_stalled` (bool) — the drive is not taking pulses. Verified working.
- `torque_fb_pct`, `torque_cmd_pct` — drive torque in percent. A healthy vertical axis
  holds its carriage at about **14.6 %**; below that it drops.
- `drive_alarm_word` — raw register, deliberately not interpreted.

Suggested: turn the status dot amber when `drive_stalled` is true, and put the torque
figure where it can be read at a glance. Do not present the dot as healthy merely
because REST answered — `servo_state == "READY"` is what the firmware *believes*, and it
believed it while the drive was switched off.

## Not verified on Windows yet

Click-through, always-on-top behaviour over a full-screen application, and the
crosshair button's real-world usability in a headset.
