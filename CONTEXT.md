# OSSM project — start here

Two halves: the **firmware** (OSSM-Stroke refactored onto ESP32-SvelteKit, controlling a
rack-and-pinion machine) and a **Rust Windows overlay HUD** that lays controls over a
VR or large view. Written at the end of a long session so the next one starts informed
rather than re-deriving.

## Where things are

| What | Where |
| --- | --- |
| Repo | `/root/ossm-refactor/OSSM-SvelteKit`, branch `dev` |
| Remote | `ortlof/OSSM-Stroke-SvelteKit` (private) |
| Push | `/usr/local/bin/ossm-gh open <6-digit code>` then `ossm-gh push`, then `ossm-gh close` |
| Firmware build | `/root/.platformio/penv/bin/pio run -e esp32dev` |
| OTA | `/root/.pio-venv/bin/python scripts/ota-upload.py --device 192.168.2.115 build/release/*.bin` |
| Overlay | `tools/overlay-hud` — `cargo build --release [--target x86_64-pc-windows-gnu]` |
| Deliverables | `/root/ossm-refactor/dist/`, served on `:8099` by the `ossm-dist` systemd unit |
| Machine | `192.168.2.115` (LAN via Tailscale subnet route), login admin/admin |
| Windows test box | `10.99.10.51`, user `buildwin`, key `~/.ssh/id_ed25519_win` |

## Machine facts — measured, not assumed

- **9.4248 mm** of carriage travel per motor revolution; **84.88 steps/mm**; usable travel 347 mm; keepout 15 mm.
- **The axis is vertical.** It holds its own carriage at **14.6 % torque ≈ 57 N**. Below about 15 % the carriage **drops** — this is how a stalled carriage and a falling one look identical to a step counter.
- **Speed**: the drive's encoder says the machine reaches what it is asked for — 300 mm/s at 1913 rpm, dead steady. The firmware's own step-timing estimate is **worthless for peak measurement**: in one run it reported 30, 74, 36, 3, −299, 180, −298, 47 and reversed sign mid-stroke.
- **Force**: `force_n = torque_feedback (0.1 %) × 3.8 N`. 100 % is 380 N at the rack.
- **No lost steps**: 1 count = 0.011 mm across 260 mm of travel.
- Measured envelope of about 84.8 % flash: at a 60 mm stroke ~300 mm/s, at 300 mm ~430–500 mm/s.

## Modbus to the iHSV57 drive

- Link: `Serial2`, GPIO 16/17, **57600 8E1**, unit 1.
- **One read from `0x0840`, count 9** gives d06 input frequency, d07 speed command,
  **d08 motor speed (rpm)**, d09–d14 torque command / feedback / average / peak /
  overload / regen, all in 0.1 %.
- **Position**: `0x0834/35` command sum, `0x0836/37` feedback sum, `0x0838/39`
  deviation. **32-bit, high word first.**
- `0x0849/4A/4C` are d16/d17/d19 — **IO status bitfields, not alarm words.** Hours went
  into proving that. The alarm register is **still unidentified**; the reliable fault
  tells are the counters reading zero, or `drive_stalled`.
- Parameter addresses: **hex(group × 100 + index)**. Verified against a documented
  default. P03-15 (position-deviation alarm) lives at `0x013B`, default 2000.
  The PC tool's XML tables are extracted in `/root/.hermes/cache/scratch/ihsv/`.
- Writes: `POST /rest/modbusProbe {"param_write_reg":315,"param_write_value":20}` —
  always read back, never trust a write.

## API

- `POST /rest/signIn` (admin/admin) → `access_token`
- `/rest/strokeState` — run, depth, stroke, speed, sensation, pattern, setup, position,
  usable_travel, speed_measured, servo_state, homed
- `/rest/strokeSettings`, `/rest/systemStatus`
- `/rest/modbusProbe` — `torque_cmd/fb/avg/peak_pct`, `overload_pct`, `force_n`,
  `speed_actual_mm_s`, `speed_peak_mm_s`, `drive_stalled`, `mb_*`, `param_write_*`

## Gotchas that cost real time

- **OTA can return `http=507 Insufficient Storage`** when flash is full. The device then
  keeps running the old image and the script says "device did not come back" after 90 s.
  It is **not** a crash and **not** WiFi. Read the response code first.
- Flash was trimmed from 97.7 % to 84.8 % by dropping `FT_MQTT` and `FT_NTP` in
  `features.ini`. Keep headroom — 45 kB caused the whole 507 confusion.
- Every Modbus reply buffer must suit the **largest** read; use
  `MODBUS_RTU_READ_REPLY_MAX` (**a macro — it cannot be namespace-qualified**).
- **Flush the UART receive buffer before every request**, or a read silently returns the
  previous reply. A stale step count looks perfectly plausible.
- **Judge transparency with pixel statistics, never a vision model.** It called a
  99 %-transparent panel "opaque" twice, and once invented a slider-position bug by
  assuming 0–100 scales.
- `pkill -x`, never `pkill -f` — the `-f` form matches the shell running it and kills
  the tool call.
- In Rust, uppercase locals are **const patterns**: `let GAP = x;` fails with
  "refutable pattern in local binding". Bind lowercase.

## Overlay HUD, current state

- **Two shapes**: shut `250 × 62` (power button + three-line opener), open **`250 × 800`**.
  Opens when the opener is hovered; shuts 400 ms after the pointer leaves, unless a
  slider is being dragged.
- **The panel unfolds away from the nearer screen edge**, so it can be docked at the
  foot of a screen: a strip in the lower half of the screen opens upward (bottom edge
  fixed, top edge moves up), one in the top half opens downward (top edge fixed), which
  is the behaviour it always had. The direction is latched when the panel opens, so
  dragging the open panel across the middle does not flip it under the hand, and the
  strip returns to where it was when the panel shuts. Measured: strip
  (300,620)-(447,656) opens to (300,186)-(447,656); a strip at the screen foot,
  (300,730)-(447,766), opens to (300,296)-(447,766), entirely on screen.
- **Four columns own their buttons**: column 1 carries power (top) and the pattern menu
  (bottom); column 3 carries T / keep-start and the crosshair / depth-setup. Columns 2
  and 4 stay clear so the faders line up.
- Values above each rail, labels vertical down the left, up/down **nudge caps** (one step
  per press) at the rail ends, block handles.
- `panel_alpha = 3` (≈99 % transparent), text very dark grey, **Start/stop deliberately
  more saturated** so the one control that moves the machine stays findable.
- **No window shadow on Windows**: `remove_drop_shadow()` in `main.rs` turns DWM's
  non-client rendering off. winit's `has_shadow` is macOS-only and the window
  keeps `WS_CAPTION`, so DWM drew a halo over the view behind the panel — 47 px
  at the sides, 78 px below it, measured with a hide/show pixel diff. The
  setting survives a click-through toggle, which rewrites the window styles.
- **It never takes the foreground**, and that is what keeps the taskbar out of a
  fullscreen video: `WS_EX_NOACTIVATE` (re-applied every frame, because winit
  rebuilds the style set) plus `with_active(false)`. A click that activates the
  overlay is what makes the shell bring the taskbar forward; measured, the
  foreground stays put with the style and jumps to the overlay without it. The
  cost: no keyboard focus, so the click-through shortcut cannot fire — use
  `overlay-hud --toggle-click-through` or edit `config.toml`.
- **A no-activate window cannot use the OS move loop.** winit's `StartDrag` moved
  it 0 px while a build without the style moved it 36x24 on the same drag, so the
  grip drags the window itself (`SetCapture` + `SetWindowPos`).
- `config.toml` drives window size plus a `[layout]` section (slider width/height, thumb,
  button, cap size, gap, label font) and is **re-read live** while the HUD runs.
- Proven on Windows 11: runs, connects, live values, translucency confirmed by
  differential pixel capture. Proven screenshots in `dist/`.

## Open items

1. **The alarm register is unidentified.** Capturing d16/d17 across a known trip is the
   honest way; the counter-zero test already covers the safety need.
2. **The HUD does not show `drive_stalled` or the torque/speed values yet.** Turn the
   status dot amber on stall, and show the force in newtons.
3. `user_speed_limit` 500 / `max_speed` 600 sit above what has been proven. The measured
   envelope is ~300 mm/s at a 60 mm stroke.
4. `controls_row` in `app.rs` is dead code from the layout change; delete it.
5. The overlay's open/shut hover timing (400 ms) has never been judged by a human.
6. Nudge caps are 15 points and vertical labels 10 points — both are candidates for
   enlargement if they are hard to hit or read at headset distance.
