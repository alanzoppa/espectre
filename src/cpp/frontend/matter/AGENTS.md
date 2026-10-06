# Agent Rules — Matter Frontend Fork

This fork adds a NeoPixel (WS2812) extended color light endpoint to the
upstream ESPectre Matter occupancy-sensor firmware. The repository root
`AGENTS.md` governs everything not overridden here; this file adds the
rules specific to the light-endpoint work.

## Fork Mission

- Goal: one ESP32 (Seeed XIAO ESP32-S3) that is simultaneously a Wi-Fi CSI
  motion sensor (upstream behavior, unchanged) and a Matter-controlled
  NeoPixel light. Controllers see one node with two application endpoints:
  occupancy sensor + extended color light.
- Target hardware: XIAO ESP32-S3, WS2812 data line on GPIO 1 (D0/A0).
  Configure via `ESPECTRE_MATTER_LED_GPIO` (default 1) and
  `ESPECTRE_MATTER_LED_COUNT` (default 1), Kconfig menu "ESPectre Matter".
- The upstream `README.md`, `docs/`, and frontend guide own general
  repository behavior. Upstream's routing rules apply unchanged
  (surgical diffs, narrow reads, no unbounded dumps).
- Do not upstream this work. The GPL license makes the personal fork
  legal; there is no plan to contribute back.

## Fork-Specific Files

| File | Role |
|------|------|
| `src/cpp/frontend/matter/app/main/neopixel_light.{h,cpp}` | WS2812 driver (RMT backend), HSV/xy/CCT rendering, Identify animation. The only fully fork-owned code. |
| `src/cpp/frontend/matter/app/main/app_main.cpp` | Endpoint creation + attribute-callback wiring (fork sections marked with comments). |
| `src/cpp/frontend/matter/app/main/Kconfig.projbuild` | `ESPECTRE_MATTER_LED_GPIO` / `LED_COUNT` options. |
| `src/cpp/frontend/matter/app/main/idf_component.yml` | `espressif/led_strip ^2.5.5` dependency (public, so RMT driver headers propagate). |
| `src/cpp/frontend/matter/app/main/CMakeLists.txt` | Registers `neopixel_light.cpp`; adds `driver` to REQUIRES. |
| `src/cpp/frontend/matter/app/sdkconfig.defaults` | Endpoint/device-type budget raised 2→3 (root + occupancy + light). |

## Hard Constraints

- **Dependency direction** (upstream rule): the neopixel driver is
  frontend-local. `neopixel_light.cpp` includes only FreeRTOS,
  `led_strip*`, and `esp_*` headers plus its own header. If a change
  seems to need SDK sensing headers, stop — it belongs in `app_main.cpp`.
- **Endpoint budget**: `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=3`
  and `CONFIG_ESP_MATTER_MAX_DEVICE_TYPE_COUNT=3` in
  `sdkconfig.defaults`. Three = root node + occupancy + light. Do not
  lower; if a fourth endpoint is ever added, raise both together.
- **Matter version pin**: `espressif/esp_matter` pinned to `1.6.0`.
  esp-matter publishes releases only through the ESP Component Registry
  (the GitHub repo has no version tags; `gh api .../tags` is empty), so
  verify APIs by downloading the registry zip: query
  `https://components.espressif.com/api/components/espressif/esp_matter`,
  take the `url` from the 1.6.0 entry, and inspect
  `components/esp_matter/` inside the zip.
- **led_strip pin**: `^2.5.5`. The v2.x API is `led_model`,
  `led_pixel_format` (`LED_PIXEL_FORMAT_GRB`), and
  `led_strip_new_rmt_device`. led_strip 3.x renames these to
  `color_component_format` and `LED_STRIP_COLOR_COMPONENT_FORMAT_GRB` —
  do not mix APIs across majors. If the resolver picks 3.x, pin `2.5.5`
  exactly.
- **Identify callback**: endpoint-scoped. The Identify animation runs
  only for the light endpoint (`endpoint_id == g_light_endpoint_id`),
  never root or occupancy.
- **Thread safety**: `neopixel::set_*()` calls arrive on the Matter chip
  thread; the driver serializes on an internal mutex, and the identify
  task must take the same mutex before touching the strip.
- **License**: GPLv3-only. No dependencies incompatible with GPLv3;
  fork files carry upstream SPDX headers.

## esp-matter 1.6.0 API Facts (verified against the registry package)

Use these; do not re-derive from memory or from other esp-matter
versions online.

- `extended_color_light::config_t` (in
  `data_model/legacy/esp_matter_endpoint_impl.h`) inherits
  `dimmable_light::config_t` and adds `color_control`,
  `color_control_color_temperature`, `color_control_xy`,
  `color_control_remaining_time`.
- The base `cluster::color_control::config_t` has `color_mode`, `options`,
  `number_of_primaries`, `enhanced_color_mode`, `color_capabilities`, and
  `primary_1..6_x/y/intensity` — **no** `current_hue` or
  `current_saturation`. Hue/sat are feature-only attributes living in
  `cluster::color_control::feature::hue_saturation::config_t`.
- `extended_color_light::add()` wires only the color_temperature and xy
  features. Hue/sat requires an explicit
  `cluster::color_control::feature::hue_saturation::add(cluster, &config)`
  after endpoint creation; the add call also sets the `ColorCapabilities`
  bits automatically.
- `cluster::get(endpoint, ColorControl::Id)` fetches the color control
  cluster from an endpoint (`data_model/esp_matter_data_model.h`).
- `dimmable_light::config_t` has `level_control` (base config:
  nullable-u8 `current_level`, `min_level`, `max_level`, `options`,
  `on_level`) and `level_control_lighting` (feature config:
  `remaining_time`, `start_up_current_level` — `start_up_current_level`
  lives here, not in the base config).
- Attribute value union `esp_matter_attr_val_t`: `.val.b` (bool),
  `.val.u8`, `.val.u16`, `.val.a.b`/`.val.a.s` for strings.
- Identify callback enum (`esp_matter_identify.h`): `identification::START`
  / `STOP` / `EFFECT`, signature
  `(type, endpoint_id, effect_id, effect_variant, priv_data)`.
- Root `CONFIG_DEVICE_TYPE=0x0107` (occupancy) stays untouched; the light
  endpoint's device type comes from the `extended_color_light` factory
  (`ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID`).

## Validation Gates

- **Build (first milestone)**: `./espectre matter build --chip s3` must
  succeed. This build has never completed; compile errors are likely and
  expected — the code was written against the verified 1.6.0 package
  source but never compiled. Fix errors against the API facts above and
  the unpacked registry zip, not against guesses.
- **Boot-log check** after flashing: occupancy endpoint then light
  endpoint created, and `ESPectre Matter firmware started on endpoint %u`
  still prints the **motion** endpoint id (upstream behavior, preserved).
- **Flash and smoke test**: `./espectre matter flash --chip s3 --port
  /dev/cu.usbmodemXXXX`, commission over BLE into Apple Home or Home
  Assistant. Expect: pairing works, occupancy reports motion, and the
  light endpoint exposes OnOff / LevelControl / ColorControl.
- **Full suite**: `pytest -q` at repo root must not regress (upstream
  tests only; this fork adds no Python tests).

## Definition of Done

- `./espectre matter build --chip s3` succeeds with no new warnings from
  fork code (upstream warnings out of scope).
- Device boots, pairs via BLE, and shows both endpoints: occupancy +
  extended color light.
- On/off, brightness, hue/sat color, and CCT white from the controller
  drive the strip correctly; Identify blinks it during commissioning.
- `pytest -q` green at repo root.
- Committed to `main` on this fork with a Conventional Commit message.