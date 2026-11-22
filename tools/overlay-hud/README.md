# overlay-hud — OSSM-Stroke desktop overlay

A small, frameless, transparent, always-on-top control panel for the
OSSM-Stroke machine, written in Rust. It is meant to sit over a video or a VR
window while the machine runs, and it exposes the same controls as the web UI at
`interface/src/routes/stroke/Stroke.svelte`.

Built with [`eframe`/`egui`](https://github.com/emilk/egui) 0.37 on the `glow`
(OpenGL) renderer. There is **no C dependency** anywhere (no TLS crate, no
OpenSSL): the device is spoken to over plain HTTP by a small hand-rolled client,
and the Windows specifics go through the pure-Rust bindings `egui` already pulls
in. That keeps the dependency tree small and — as it turns out — lets the GNU
cross build link with no mingw toolchain at all.

## What it shows

* **Four vertical sliders** — Speed (capped at the slider limit from
  `/rest/strokeSettings`), Depth, Stroke and Sensation (`-100 .. 100`, step 5) —
  and **one Start/Stop button**. The sliders and the button are the whole control
  surface, and they are deliberately large: the overlay is aimed at a pointer that
  is far from the screen (a VR controller, a laser pointer, a hand on a touch
  panel), so every target is big and nothing else competes for attention.
* **A compact pattern selector.** A small combo box in a slim row across the
  bottom picks the stroke pattern (`{"pattern": N}`). It shows the device's
  `pattern_name` and is small on purpose — it is a selector, not a feature.
* **A small Set-Depth button.** A square button with a hand-drawn crosshair
  glyph sits next to the pattern selector, well clear of Start/Stop, and toggles
  the firmware's interactive depth-setup mode (`{"setup": 1}` / `{"setup": 0}`).
  It lights up amber while `state.setup != 0`. Its tooltip says plainly that it
  **moves the machine**: the machine creeps to the depth position at 10 mm/s and
  holds there so the operator can look at it.
* **The value sits above the rail.** The number is printed above each rail, where it
  can be read without covering anything, so the handle stays a plain grip.
* **One status dot** inside the button: red = offline, yellow = connected but not
  homed, green = ready or running. No status text.
* **Vertical on purpose.** A vertical slider can be made tall and fat without
  widening the window, and a hand that is not resting on anything moves up and
  down more predictably than sideways.

Everything from the earlier design that is not needed at arm's length —
keep-stroke-start, the tuning section, the readout row — still lives only in the
browser page, which is the better place for it.

## The two shapes

Shut, the window is a 250 × 62 strip: the Start/Stop button and a three-line
opener, nothing else. Hovering the opener opens the full 250 × 800 panel; the
panel shuts again 400 ms after the pointer leaves it, unless a slider is being
dragged. Click-through (`Ctrl+Shift+O`, or `overlay-hud --toggle-click-through`
from another process) lets the pointer pass through the panel entirely.

The panel unfolds away from the nearer screen edge, so the overlay can be docked
at the foot of a screen: a strip in the lower half of the screen opens upward
(its bottom edge stays where it is, the top edge moves up), and one in the top
half opens downward (its top edge stays where it is). The direction is decided at
the moment the panel opens, so dragging the open panel across the middle of the
screen does not flip it under your hand, and the strip comes back to exactly where
it was when the panel shuts.

Every command is a `POST /rest/strokeState` with a subset body. The firmware
clamps what it receives (speed to the slider limit, depth to usable travel minus
`far_margin`, stroke to depth minus `home_margin`) and returns the state **after**
clamping; the overlay always displays those returned values, never the requested
ones.

## How it talks to the device

* **REST polling, not the WebSocket.** This project has a load-bearing lesson: do
  **not** stream `ws://…/ws/events` and make blocking HTTP calls from the same
  thread. The device's sends back up, the HTTP request stalls for its whole
  timeout, and it looks exactly like a firmware hang. So the overlay polls REST
  from one dedicated thread and never opens the event socket. The useful poll
  rate for the readout is 5–10 Hz (`poll_hz`, default 8).
* The thread signs in once with `POST /rest/signIn` and sends
  `Authorization: Bearer <jwt>` on every other call. A `401` clears the token and
  the thread signs in again (until then it retries at 2 Hz so a missing machine
  does not flood the network).
* The UI thread never blocks: it pushes commands through a channel and drains
  telemetry snapshots. While a slider is being dragged, or for 0.6 s after a
  command is sent, the widget keeps its own value instead of snapping back to the
  device echo.
* The pattern list is the firmware's fixed list
  (`lib/StrokeEngine/src/pattern.h`, the `patternTable` array), in firmware index
  order: Simple Stroke, Teasing or Pounding, Robo Stroke, Half'n'Half, Deeper,
  Stop'n'Go, Insist, Knot. The pattern selector's combo is built from it, and the
  index it POSTs is the firmware's own index.

## Configuration

Everything runs from a TOML file (`config.toml`, shipped with the defaults),
never from hard-coded values. The credentials are the firmware's factory defaults
and a known security issue — change them on the device and here.

```toml
device = "http://192.168.2.115"   # base URL, plain HTTP, no trailing slash
username = "admin"
password = "admin"
poll_hz = 8.0
settings_poll_secs = 2.0
click_through = false
click_through_shortcut = "Ctrl+Shift+O"
window_width = 430.0
window_height = 330.0

# How opaque the panel is: 0 is invisible, 255 is solid.
# Lower it for VR, raise it if the text fights a bright video behind it.
panel_alpha = 96
```

**Transparency.** `panel_alpha` is the knob. The panel is drawn translucent over a
fully transparent window, so whatever is behind it — a video, a VR mirror, the
desktop — stays visible. The slider labels sit on small dark pills and the slider
tracks are dark with a bright fill, so the controls stay readable whatever is
behind them.

**Setting the device address.** Edit `device` in `config.toml`. The file is
looked up in this order:

1. `$OSSM_HUD_CONFIG` if set,
2. `./config.toml` in the current directory,
3. `config.toml` next to the executable.

If none exist, the default config is written next to the executable, so a
double-clicked binary finds its own file even though its working directory is
somewhere else. On Windows, after the first run, edit `config.toml` in the same
folder as `overlay-hud.exe`. A config file may list only the keys you want to
change; any missing key falls back to its default.

## Click-through

Click-through makes the pointer pass through the panel to whatever is underneath.
Toggle it back **off** one of two ways:

* **Keyboard shortcut** (`click_through_shortcut`, default **`Ctrl+Shift+O`**) —
  works while the overlay has keyboard focus.
* **A second process flipping the config file** — this is the reliable path when
  the overlay cannot take focus (you just clicked the video window). The overlay
  re-reads `config.toml` about once a second and applies `click_through` from it,
  so any of these work:

  ```sh
  overlay-hud --toggle-click-through        # flips the flag and exits
  ```

  ```powershell
  # or write the file yourself from a Stream Deck / AutoHotkey script:
  (Get-Content config.toml) -replace '^click_through = .*', 'click_through = false' | Set-Content config.toml
  ```

In click-through mode the title strip shows `click-through · Ctrl+Shift+O` so you
can still see the escape hatch while the mouse is inert over the panel.

## Build on Windows (MSVC toolchain)

Prerequisite: the **MSVC** build tools (Visual Studio Build Tools with the
"Desktop development with C++" workload) and the Rust MSVC toolchain.

```powershell
rustup default stable-x86_64-pc-windows-msvc
cargo build --release
# -> target\release\overlay-hud.exe
```

`eframe`/`winit`/`glutin` link against the system `opengl32`/`gdi32` through the
`windows-sys` bindings; nothing beyond the MSVC toolchain is needed. Run
`overlay-hud.exe` next to its `config.toml`.

The binary is a console-subsystem executable, so `--probe`, `--help` and
`--toggle-click-through` still print when it is run from a shell. A black console
window must not sit behind an always-on-top panel, so the GUI start-up hides its
own console (`GetConsoleWindow` + `ShowWindow(SW_HIDE)`) whenever it runs without
a command-line flag. Double-click the exe and only the overlay appears; there is
nothing to configure and no shortcut trick to remember.

The Windows drop shadow is switched off in the same start-up code. Windows draws
that shadow around any window that still carries a frame style - and winit's
frameless windows do, because winit paints over the non-client area instead of
removing `WS_CAPTION`. The shadow therefore lands on the game, video or VR view
behind the panel as a soft grey halo. `with_has_shadow(false)` has **no effect on
Windows** (winit implements it for macOS only); `DwmSetWindowAttribute(hwnd,
DWMWA_NCRENDERING_POLICY, DWMNCRP_DISABLED)` removes it for good, and it survives
the window style writes that a click-through toggle makes.

The overlay also never takes the foreground, on purpose. Windows answers a
foreground change by bringing the taskbar forward, straight through a fullscreen
video, so the window carries `WS_EX_NOACTIVATE`: a click delivers the mouse
messages to the controls without activating the window, and it is shown with
`SW_SHOWNOACTIVATE` so starting it over a running video does not disturb the view
either. Because the operating system's move loop refuses to run for such a window,
the thin grip at the top drags the window itself (the mouse is captured and the
window follows the pointer); X11 and Wayland still use the window system's own
drag.

One trade-off comes with that: a window which never activates never receives
keyboard focus, so `Ctrl+Shift+O` cannot fire while the overlay sits over another
application. Use `overlay-hud --toggle-click-through` from a second process (a
Stream Deck, AutoHotkey, a shell one-liner) or edit `click_through` in
`config.toml`; the running overlay picks the file up within a second.

## Cross-compile from Linux (GNU target)

```sh
rustup target add x86_64-pc-windows-gnu
cargo build --release --target x86_64-pc-windows-gnu
# -> target/x86_64-pc-windows-gnu/release/overlay-hud.exe
```

### Measured outcome (this machine)

Tools: `cargo`/`rustc` 1.100.0, targets `x86_64-unknown-linux-gnu` and
`x86_64-pc-windows-gnu` installed. No `x86_64-w64-mingw32-gcc` is present on this
machine.

| Command | Result |
| --- | --- |
| `cargo check` | **passes**, no warnings |
| `cargo build --release` | **passes** → `target/release/overlay-hud` (~8.8 MB, ELF x86-64) |
| `cargo build --release --target x86_64-pc-windows-gnu` | **passes** → `target/x86_64-pc-windows-gnu/release/overlay-hud.exe` (~7.0 MB) |

```
$ file target/x86_64-pc-windows-gnu/release/overlay-hud.exe
target/x86_64-pc-windows-gnu/release/overlay-hud.exe: PE32+ executable for MS Windows 5.02 (console), x86-64 (stripped to external PDB), 9 sections
```

**The GNU cross build does not need mingw here.** Because the crate has no C
dependency, the GNU target links with `rust-lld` (shipped with rustc) instead of
`x86_64-w64-mingw32-gcc`. The common failure — missing mingw/OpenGL C headers —
does not arise for this dependency set. If you later add a crate with C code
(e.g. a TLS stack such as `ring`/`openssl-sys`), the GNU link will start asking
for a `x86_64-w64-mingw32-*` toolchain and you will need `mingw-w64` installed;
this crate avoids that on purpose.

## Verifying without a display

`overlay-hud --probe` signs in, prints the `/rest/strokeState` and
`/rest/strokeSettings` payloads and a no-op command echo, then exits — a headless
smoke test of the whole REST client (sign-in, Bearer auth, JSON, chunked and
content-length responses, POST body). It was run against a local mock of the
device and against the real machine on the network.

## Layout

| File | Content |
| --- | --- |
| `src/main.rs` | entry point, window options, `--probe` / `--toggle-click-through` |
| `src/config.rs` | config load/save, path resolution, click-through flip |
| `src/api.rs` | hand-rolled HTTP/1.1 client + `StrokeState` / `StrokeSettings` |
| `src/worker.rs` | the REST polling thread |
| `src/app.rs` | the egui overlay window |
| `config.toml` | default config (device address, credentials, shortcut) |

## Licence

MIT OR Apache-2.0.
