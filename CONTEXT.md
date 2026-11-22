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
| Upstream patterns | `/root/ossm-refactor/LUST-motion` — `openlust/LUST-motion`, **public** (no token needed); its `lib/Pattern/` is the base of our pattern layer |
| Upstream patterns | `/root/ossm-refactor/OSSM-Lite` — `fray-d/OSSM-Lite` (renamed from `fray-d/OSSM-hardware`); its `Software/lib/StrokeEngine/src/pattern.h` is the source of the last six patterns |

## Machine facts — measured, not assumed

- **9.4248 mm** of carriage travel per motor revolution; **84.88 steps/mm**; usable travel 347 mm; keepout 15 mm.
- **The axis is vertical.** It holds its own carriage at **14.6 % torque ≈ 57 N**. Below about 15 % the carriage **drops** — this is how a stalled carriage and a falling one look identical to a step counter.
- **Speed**: the drive's encoder says the machine reaches what it is asked for — 300 mm/s at 1913 rpm, dead steady. The firmware's own step-timing estimate is **worthless for peak measurement**: in one run it reported 30, 74, 36, 3, −299, 180, −298, 47 and reversed sign mid-stroke.
- **Force**: `force_n = torque_feedback (0.1 %) × 3.8 N`. 100 % is 380 N at the rack.
  Same number as a weight: `force_kg = force_n / 9.80665`, so 1.0 % is 0.3875 kgf.
  At-rest reading is stable (mean −13.65 %, stdev 0.34 % over 12 s), so 1 kg along the
  axis (2.58 %) is measurable. A reading taken seconds after a homing run is not
  settled yet — one boot showed −5.3 % then −13.4 %.
- **A mass reads correctly to ~1 %, but ONLY away from an end stop** (measured
  2026-11-22 with a stated 3.093 kg on the carriage). Held at 60 mm, where the motor
  carries everything, the torque delta against the no-load baseline read **−3.131 kg,
  a 1.2 % error**. Held at position 0 the same mass read **2.33 kg**, and 2.0 kg after
  moving away and back — the end stop carries part of the weight, and how much depends
  on how the carriage arrived there. So a holding-torque reading at 0 is not a mass
  measurement. `force_kg` from the drive is the total (carriage + load + friction), not
  the added mass: it read −7.62 kg at rest with the 3.09 kg on. Readings:
  no load at 0 mm −13.65 %; load at 0 mm −19.65 %; **load at 60 mm −21.73 %**
  (stdev 0.71); load back at 0 mm −16.50 %.
- **Torque limit registers**: `0x01FE` = P05-10 forward, `0x01FF` = P05-11 reverse,
  scale 0.1 %. **The drive stores forward positive and reverse NEGATIVE** (vendor
  table: P05-11 range `-300.0-0`, default −200.0; the drive reads +2000 / −2000).
  A write with the wrong sign is echoed without complaint and does nothing defined.
  P05-03..P05-09 in that block are unimplemented and answer with their own address.
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
- **Torque limits**: `0x01FE` (P05-10, forward) and `0x01FF` (P05-11, reverse), 0.1 %.
  The reverse limit is a **negative** register value; the firmware now speaks
  magnitudes and applies the sign in `modbus_rtu::torqueLimitWireValue()`. The drive
  echoes a wrong-signed write without complaint, so always read back.
- Parameter addresses: **hex(group × 100 + index)**. Verified against a documented
  default. P03-15 (position-deviation alarm) lives at `0x013B`, default 2000.
  The PC tool's XML tables are extracted in `/root/.hermes/cache/scratch/ihsv/`.
- Writes: `POST /rest/modbusProbe {"param_write_reg":315,"param_write_value":20}` —
  always read back, never trust a write.

## API

- `POST /rest/signIn` (admin/admin) → `access_token`
- `/rest/strokeState` — run, depth, stroke, speed, sensation, **slow_start**,
  pattern, setup, position, usable, speed_measured, servo_state, homed,
  clipping, **uses_slow_start**
- **`slow_start`** (0..100, default 50) is where the slow part of a pattern
  starts, as a share of the active stroke. It is a second pattern parameter
  beside `sensation`, and only the patterns that answer `usesSlowStart()` read
  it: **Knot Short** and **Knot Deep**. `uses_slow_start` in the telemetry says
  whether the running pattern reads it, so a client can grey the slider out.
  Both knot patterns used to derive the slow point from `sensation`, so the
  Sensation slider and the pattern fought over one value; `sensation` no longer
  moves the slow point on either of them.
- **`pull_out`** — transient, like `home`. It stops the pattern, clears `run`,
  and retracts the carriage all the way to the home reference (`moveToMin()`,
  step 0, which is where the homing run parks it) at 40 mm/s. Clearing `run` is
  deliberate: leaving it set would let the next unrelated command start the
  sweep again from wherever it stopped. Read back as `pull_out`.
- `/rest/strokeSettings` — machine geometry, motor, **and the torque limits**:
  `torque_outward_pct`, `torque_inward_pct` (0..300), `torque_outward_forward`.
  A change applies within ~100 ms and is written through to the drive.
- `/rest/modbusProbe` — `torque_cmd/fb/avg/peak_pct`, `overload_pct`, `force_n`,
  `force_kg`, `speed_actual_mm_s`, `speed_peak_mm_s`, `drive_stalled`, `mb_*`,
  `param_write_*`, and the drive's own limit read-back `torque_limit_forward_pct` /
  `torque_limit_reverse_pct` / `torque_limit_ok` (polled every 5 s).
- **Web UI**: `/stroke` is the live control page; **`/setup` (menu "Machine Setup")
  is where the torque limits are set** — outward and inward, each with its ceiling in
  N and kgf, a swap switch, and the drive's read-back. Source
  `interface/src/routes/setup/`. The page never changes the limits mid-pattern.

## Web UI on a phone — the four fixes, and the preview loop

**`interface/src/app.css` carries a "Layout on a phone" block taken from the DTT
trainer's interface.** Both projects are ESP32-SvelteKit forks and their
`+layout.svelte` files are byte-identical, so the same four defects come with the
framework. Without the block, measured on the Stroke page at 390 px: daisyUI's
`.label` is `white-space: nowrap`, so the longest description was **1685 px wide
inside a 358 px box** and every helper line was clipped mid-word at the screen edge.
With the block the same label is 358 px and wraps to 90 px tall. The four rules:

1. `.label { white-space: normal }` — daisyUI nowraps every label; there are 90 of
   them across 12 pages and they are sentences, not form labels.
2. `.fieldset, .grid > *, .flex-col > * { min-width: 0 }` — flex/grid children
   default to `min-width: auto` and refuse to shrink below their content.
3. `@media (min-width: 640px) { .stats.stats-vertical { grid-template-columns:
   repeat(3, minmax(0, 1fr)) } }` — outranks the `sm:stats-horizontal` utility.
   **Three, not the DTT's four**: both OSSM cards hold 3 stats, so one rule is
   enough and `minmax(0, 1fr)` is what stops a stat pushing past the card edge.
   The DTT needed two rules because its card holds four.
4. `.toast { pointer-events: none } .toast > * { pointer-events: auto }` — the toast
   host is a fixed corner box and daisyUI lets it eat taps, a dead zone on a phone.

Verified over 390 / 700 / 1024 px on `/stroke` and `/setup`: `.label` computes
`normal`, zero labels overflow their box, the widest label equals its parent width
exactly, no page overflow, and the stats go 1 column (phone) to 3 (from 640 px) with
zero overflow.

**Fast UI loop, no firmware build: `scripts/ui-preview.py`** (copied from the DTT
tree). It serves `interface/build` locally and proxies `/rest` to the device with a
token it signs in for, and rewrites `security: false` into `/rest/features` so the
front end does not draw a login over a working session.

```sh
cd interface && npm run build
python3 scripts/ui-preview.py --device http://192.168.2.115 --user admin --password <pw> --port 8096
# then open http://127.0.0.1:8096/stroke with a phone viewport
```

**Use port 8096, not 8095**: the DTT tree keeps a `ui-preview.py` running on 8095 and
another on 8097, and hitting the wrong one serves the DTT front end (its title reads
"DTT Trainer") under a login form — which looks exactly like the OSSM UI failing to
authenticate. Check `ss -ltnp | grep 809` first. `/ws/events` is **not** proxied, so
the page shows "Connection to device lost" toasts; that is the preview, not the device.

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

## Where the flash goes, and what is worth reclaiming

Measured from the linker map (`firmware.map`, the block after "Linker script and
memory map" — everything earlier is discarded sections and double-counts) and from
the symbols in the ELF. The slot is 1,966,080 bytes on the plain ESP32
(`min_spiffs.csv`); the S3 uses the 8 MB layout and is not tight.

| Component | Bytes | Note |
| --- | --- | --- |
| Embedded web UI | 247,897 | `ESP_SVELTEKIT_DATA_0..8`, the gzipped SvelteKit bundle. **14.8 % of the image and the biggest single removable item.** `ESP_SVELTEKIT_DATA_5` alone is 194,498. |
| newlib `libc.a` | 206,000 | `_svfprintf_r` 11 210, `_vfprintf_r` 11 429, the rest locale/stdio |
| WiFi stack | ~485,000 | net80211 163 k, lwIP 138 k, wpa_supplicant 64 k, pp 56 k, phy 32 k, mDNS 32 k |
| ESP32-SvelteKit framework | ~124,000 | 372 k in total, less the 248 k UI it carries |
| FastAccelStepper | 130,000 | the step engine — needed |
| `libstdc++` | 118,000 | |
| mbedTLS | 83,000 + | only if anything still uses TLS |
| Our app + StrokeEngine + 15 patterns | 72,000 | |
| PsychicHttp | 49,000 | |
| `libFrameworkArduino.a` | 44,907 | the Arduino core — the whole of what "leave Arduino" would remove |
| FastLED | 0 | was 52 k, removed — see below |

### Leaving PlatformIO saves nothing; leaving Arduino saves almost nothing

**PlatformIO is a build system and a package manager. It puts 0 bytes in the image.**
It compiles the same sources with the same toolchain; ESP-IDF's own build would
produce the same binary. Any claim of a flash saving there is wrong.

**Arduino is ESP-IDF plus a layer, not an alternative to it.** The heavy parts —
the 485 kB WiFi stack, 206 kB newlib, 118 kB libstdc++, FreeRTOS, mbedTLS — are
ESP-IDF and stay under either toolchain. Measured, the Arduino-only part is
`libFrameworkArduino.a` at **44,907 bytes**, plus ~139 kB of Arduino-flavoured
libraries (HTTPClient, WiFi, Network, FS, SD, Update, OTA, mDNS, LittleFS, Hash,
Ethernet) that are *functionality*, not overhead — moving to plain IDF means
re-implementing them. So the honest ceiling for leaving Arduino is about
**45–60 kB, in exchange for rewriting the HTTP server, OTA, filesystem, WiFi and
the whole framework integration.** Not worth it. **Decision: stay on Arduino.**

### What was actually reclaimed

- **FastLED: 52 kB, removed.** It was declared as a dependency and pulled 81 kB into
  the link, but nothing referenced it — the only occurrence of the name outside
  `.pio` was the dependency line itself, and its only contribution to the image was
  the global `CFastLED` object and its constructor, kept alive by its own static
  initialiser. Removing the dependency took Flash from 85.0 % to 82.4 % and left
  zero FastLED symbols in the image. 289 kB free became **339 kB free**.

### Deliberately not done

- **Moving the web UI off the device would free 248 kB**, but the machine would stop
  serving its own UI — the LAN dist server (already on :8099) or another host would
  have to serve it. That is a product decision, not a technical one, and with 339 kB
  free there is no pressure to make it.
- **The TLS stack (mbedTLS 83 kB + `NetworkClientSecure` 14 kB + cert bundle 16 kB,
  roughly 120–150 kB)** is only reclaimable if nothing uses TLS. `FT_MQTT` and
  `FT_NTP` are off, so it may already be dead weight — but that needs an audit of
  what still opens a secure connection (OTA, the cert store) before touching it.

## Pattern library — 15 patterns from two upstreams

`lib/StrokeEngine/src/` holds `Pattern.h` (base), **one file per pattern**, and
`PatternLib.h` as the table. `PatternMath.h` is shared. The old single-file
`pattern.h` is gone.

Two sources, both ported onto the same contract:

| Source | What it gave |
| --- | --- |
| `openlust/LUST-motion` (`lib/Pattern/`) | the layout, the mm/relative contract, `SimpleStroke` and `Knot` (ours, carried over), `RandomDepth` |
| `fray-d/OSSM-hardware` → **renamed `fray-d/OSSM-Lite`** | `HalfnOut`, `ProgressiveStroke`, `KnotStroke`, `SlapStroke`, `RandomStroke`, `PointStroke` |

**The fray-d URL in the notes is stale twice over: the repo was renamed to
`OSSM-Lite`, and there is no `expirimental` branch.** The patterns are on `main`
and `New-Protocols`; the other five branches (`OG`, `Enhanced-Homing`,
`Fix/Streaming-logic`, `Fix/Not-Homed-Behavior`, `Feature/UserConfigService`)
carry only the classic eight. Its base class is the *old* contract — `_speed` in
steps/s, absolute step positions — so each pattern was translated;
`Pattern::peakSpeedMmPerSecond()` is the value its `_speed` field held.

The table (indices are stable; the last six were appended):

| # | Name | Note |
| --- | --- | --- |
| 0–6 | Simple Stroke, Teasing or Pounding, Robo Stroke, Half'n'Half, Deeper, Stop'n'Go, Insist | as before |
| 7 | **Knot Short** | ours: creeps the last stretch and stops **short** of full depth |
| 8 | **Random Depth** | LUST: random insertion depth, full return every stroke |
| 9 | Half'n'Out | every second cycle pulls only half way out; the insertion is always full |
| 10 | Progressive | the sweep walks in over a cycle in sensation-sized segments |
| 11 | **Knot Deep** | fray-d: out, in to a sensation point, then a slow push **all the way in** |
| 12 | Slap | out at half speed, pause, then 90 % and 100 % hard |
| 13 | **Random Target** | fray-d: jumps to a random point, at least a sensation-set distance apart |
| 14 | Point | park at one point inside the sweep |

Two patterns came out of each of two upstreams doing similar-sounding things, so
the names say which is which. Class names and indices are unchanged, so no stored
state and no client breaks.

**The two knot patterns take their slow point from `slow_start`, not from
`sensation`.** Both used to map `sensation` (−100..100) onto the slow point, so
the Sensation slider and the pattern fought over one value: moving the slow point
moved the sensation with it. `slow_start` is a share of the active stroke
(0..100 %, default 50) and owns it alone. Verified on the host harness: Knot
Short's creep start moves 6 → 30 → 52.2 mm as the slider goes 10 → 50 → 90 %,
Knot Deep's middle target 6 → 30 → 54 mm, and `sensation` no longer moves either.
The other 13 patterns are unchanged.

**Nothing upstream describes the six fray-d patterns.** Its `Pattern.mdx` documents
only indices 0–6 and even says Jack Hammer and Stroke Nibbler are "available in the
library but not currently implemented in the firmware". The six exist only in
`pattern.h` on `main`/`New-Protocols`, reachable through the `StrokePatterns` enum
and `Pattern::Create()`. So the behaviour noted above for those six is read from
their code — it is not an upstream statement of intent, and it has not been
confirmed against whoever wrote them.

**The contract, and it is the point:** a pattern is handed the stroke LENGTH in mm
and the full-stroke TIME in seconds, and answers with a position in **0..stroke, in
millimetres**, relative to the near end of the sweep, plus mm/s and mm/s².
`StrokeEngine::_toMotionParameter()` is the single bridge — it adds
`(_depth - _stroke)`, converts to steps and clamps — and `_applyMotionProfile()`
keeps the final word on the machine envelope. Before the port every pattern carried
`_depth` and handed back an absolute step position, so each one re-derived the
machine envelope and each one could get it wrong alone.

`setSpeedLimit()` and `setDepth()` are gone from the pattern API. `getNumberOfPattern()`
moved out of the header, so the pattern table exists in one translation unit
instead of one per includer. `SimpleStroke` and `Knot` were **not** upstream; both
are ported so nothing is lost, and Knot's absolute targets are unchanged
(240 / 270 / 295.2 mm). `RandomDepth` is the new pattern from upstream.
`JackHammer` and `StrokeNibbler` were dropped: not in the table, and both divide by
the full-stroke time as if it were the half-stroke time, so every speed they
commanded was half of what was intended.

Five bugs were found by reviewing the patterns on the host and then verifying on
the machine, and **all five are still present in LUST-motion `main` and on every
branch** — do not assume a fix is coming from there:

| Pattern | Error |
| --- | --- |
| Insist | the stroke fraction `(100 − abs(s))/100` is exactly 0 at the ±100 the engine clamps to, and `_acceleration` divides by it. Measured **−25,300,232 mm/s² and zero travel**, so the engine clipped the acceleration to 1 step/s² and the machine simply stopped. Floored at 0.05 |
| Stop'n'Go | `_isStillDelayed()` compared against `_startDelayMillis + delay`, i.e. against **absolute uptime** before the first `_startDelay()`, so the pattern sat out its first pause after every boot — up to 10 s at sensation +100. `_startDelayMillis` was an `int` holding a `millis()` value |
| Stop'n'Go | `_startDelay()` ran when the out move was **issued**, and the engine only asks for the next target once the driver stops, so the machine moved through most of the delay. Measured at 20 mm/s / 40 mm stroke / sensation 0: a nominal 5050 ms pause lasted **1.90–1.96 s** (5.05 s minus the ~3.1 s out move). At a slow enough speed or a long enough stroke the pause vanishes. Now armed on the move and started on the next call — same run then gave **5.01 / 4.80 / 4.88 s** |
| Deeper | printed on **every stroke** through `DEBUG_PATTERN`, which was the last debug switch still on. `String` building means malloc and float formatting on the motion task, 6 kB stack |
| TeasingPounding | one `ESP_LOGV` per stroke on the motion path, removed with the rest of the debug IO |

Teasing/Pounding, Half'n'Half and Half'n'Out at sensation +100 still ask above the
machine's 660 mm/s and 10000 mm/s²; the engine clips, so such a stroke takes longer
than the commanded time. By design, not a fault. Two more of the same kind, found
by the harness and deliberately left as upstream wrote them: `RandomDepth` derives
its acceleration from the full-stroke speed, so a short random insertion asks up to
26 000 mm/s² (the fray-d `RandomStroke` computes it from the actual travel and does
not), and `Progressive` at sensation −100 has a segment count of 2, where its cycle
arithmetic degenerates into a jumpy sequence. It is sensible from about 5 segments
up.

**Knot was re-derived, not just ported.** It used fixed *speed* factors, so the
creep ran at 2 % of the commanded speed (about 40 s for a 40 mm stroke at 20 mm/s)
and the cycle time was unbounded. Its phases are now defined by *time shares* —
0.75 + 0.75 + 2.5 half-strokes, i.e. a cycle is 2 commanded stroke times — and each
phase derives its speed and acceleration from the standard 1/3-1/3-1/3 relations.
The creep still dominates the cycle because it covers a short distance over the
longest share. Measured after the change: the full cycle completes in ~12 s at
20 mm/s and the creep runs at ~3.3 mm/s.

Review and regression harness: `scripts/pattern-review/` in the `ossm-stroke-firmware`
skill. It compiles the real pattern headers against Arduino stubs, mirrors
`_toMotionParameter()`, and prints every pattern's move for a range of indices and
sensations. Reasoning about the code found far less than running it did.

**Motion has not been re-verified on the machine since the port.** The build, the
flash and the pattern table are verified; the carriage has not moved.

## Overlay HUD, current state

- **Two shapes**: shut `300 × 62` (power button + three-line opener), open **`300 × 800`**.
  Opens when the opener is hovered; shuts 400 ms after the pointer leaves, unless a
  slider is being dragged. The width grew with the fifth column; the panel is drawn at
  `DESIGN_W` 380 points and scaled into the window, so the faders came out the same size.
- **The panel unfolds away from the nearer screen edge**, so it can be docked at the
  foot of a screen: a strip in the lower half of the screen opens upward (bottom edge
  fixed, top edge moves up), one in the top half opens downward (top edge fixed), which
  is the behaviour it always had. The direction is latched when the panel opens, so
  dragging the open panel across the middle does not flip it under the hand, and the
  strip returns to where it was when the panel shuts. Re-measured at 300 points wide:
  strip (300,620)-(487,659) opens to (300,160)-(487,659) with the bottom edge held; a
  strip at the screen foot, (300,730)-(487,769), opens to (300,270)-(487,769), entirely
  on screen; one in the upper half, (300,150)-(487,189), opens to (300,150)-(487,649)
  with the top edge held.
- **Five columns own their buttons**: column 1 carries power (top) and the pattern menu
  (bottom); column 2 carries **pull out** (top); column 3 carries T / keep-start (top)
  and the crosshair / depth-setup (bottom). Columns 4 and 5 stay clear so the last two
  faders line up with the rest.
- **Five faders**: Speed, Depth, Stroke, Sensation, **Slow start**. The slow-start label
  reads `SLOW START` while a pattern that reads the parameter runs and `SLOW (KNOT)`
  otherwise, driven by `uses_slow_start` in the telemetry. It stays live either way,
  because the value is remembered and applies as soon as a knot pattern runs.
- **The pull-out button** is amber, not the Start green or the Stop red, so it can never
  be read as Start/Stop. Its glyph is a bar with an arrow running up to it (the carriage
  back at the end stop) and its tooltip says plainly that it moves the machine. It is
  disabled until the machine is homed.
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
4. `controls_row` in `app.rs` is still dead code from the layout change; delete it. It is
   a second pattern selector and it is not drawn, so it is only a trap for the next
   reader.
5. The overlay's open/shut hover timing (400 ms) has never been judged by a human.
6. Nudge caps are 15 points and vertical labels 10 points — both are candidates for
   enlargement if they are hard to hit or read at headset distance.
7. **Which register is "outward" is not yet proven.** `torque_outward_forward`
   defaults to true; confirm it with a slow jog at the machine and flip the switch in
   Machine Setup if the carriage pushes when it should pull.
8. **The mass check is done and it works** (see the force bullet above): 3.093 kg read as
   −3.131 kg, 1.2 % error, measured at 60 mm away from the end stop. At position 0 it
   reads short, because the stop carries part of the weight. Use it as a load monitor
   rather than a scale, and take the reading mid-travel, never parked.
9. **The pattern port is motion-verified (192.168.2.115, supervised, 20 mm/s,
   depth 60 / stroke 40 except where noted).** Simple Stroke sweeps 20.1↔59.4 mm at
   20.3 mm/s with reversals at both ends, full stroke ≈ 6 s as the engine derives.
   Random Depth inserts to a different depth every stroke and always returns the full
   stroke. Insist at sensation +100 sits at 58–60 mm vibrating over a ~2 mm amplitude
   and never freezes. Stop'n'Go pauses 5.01/4.80/4.88 s for a nominal 5050 ms.
   Not yet done on hardware: the Stop'n'Go **boot** case, which needs a run within
   ~10 s of power-up because that is the window the old absolute-uptime gate closed.
   The host harness proves that one case at every uptime.
10. **The slow-start slider and the pull-out button are motion-verified** (2026-11-22,
   supervised, 3.093 kg on the carriage, 20 mm/s, depth 60 / stroke 40). Knot Short's
   fast phase ended at **28.1 mm** with `slow_start` 20 % and at **52.0 mm** with 80 % —
   the two values a share of the 40 mm stroke puts them at — with the creep filling the
   rest and both stopping at the 56.8 mm stop-short target. The **pull-out fired
   mid-pattern**: the carriage went 51.8 → 0.0 mm in about 1.6 s (≈37 mm/s against the
   40 mm/s command), the pattern stopped, and `run` went false. On its own it took
   59.8 → 0.0 mm in ~1.6 s. The machine moved the 3.09 kg load without complaint.
11. **The six fray-d patterns and the re-derived Knot are motion-verified** (same
   supervised setup, 20 mm/s, depth 60 / stroke 40). Knot Short: the full cycle now
   completes in ~12 s, twice the commanded stroke time, and reaches the stop-short
   target 56.8 mm; the creep runs at ~3.3 mm/s, up from 0.4. Knot Deep: 20 → 40 (the
   sensation point) → 60 in one cycle. Half'n'Out: in to 60 every time, out to 40
   then 20 on alternate cycles. Slap: out to 20, a ~5.6 s pause, then 90 % and
   100 %. Random Target: targets vary every stroke. Point: parks at exactly 40.0 mm
   (the middle) and holds. Progressive walks the sweep in segments. Nothing hit the
   position limit and no fault was raised.
