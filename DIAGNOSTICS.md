# Serial Diagnostics

This build is configured for **maximum serial visibility** so any problem is
easy to diagnose. Open the serial monitor at **115200 baud** right after reset.

## What you now see

### 1. Verbose system logs (`platformio.ini`)
```
-DCORE_DEBUG_LEVEL=5
-DLOG_LOCAL_LEVEL=ESP_LOG_VERBOSE
```
This unlocks the internal Arduino/ESP-IDF logs that were previously silenced:
WiFi association steps, TCP, SD/SPI, mDNS, etc. These are usually where
"can't connect / can't reach the device" problems show up.

> To go quiet again for a production build, set both back to
> `CORE_DEBUG_LEVEL=0` and `LOG_LOCAL_LEVEL=ESP_LOG_ERROR`.

### 2. Per-step boot trace (`main.cpp`)
Every subsystem prints a line *before* it initialises:
```
[BOOT] >>> init LittleFS          heap=265000
[BOOT] >>> init SD card           heap=264100
...
```
If the device **boot-loops or freezes**, the **last `[BOOT]` line printed**
is the subsystem that crashed/hung — that is your culprit.

### 3. End-of-boot SELF-TEST report (`main.cpp`)
On every successful boot you get a one-glance health snapshot:
```
================ BOOT SELF-TEST ================
Last reset : power-on (normal)        <- PANIC / BROWNOUT / watchdog = problem
Heap       : 198000 free  (min-ever 171000)
-- Core ----------------------------------------
  [OK ] LittleFS      (960 KB total)  <- if "--", web GUI files weren't flashed
  [-- ] SD card       (absent)
       IR buttons=12  groups=3  schedules=1
-- IR ------------------------------------------
  RX = GPIO15    TX channels active = 2
-- Hardware modules (detected on bus?) ---------
  [OK ] NFC  PN532    enabled=yes      <- "--" = module not wired / wrong pins
  [-- ] RFID RC522    enabled=no
  ...
-- Network -------------------------------------
  AP   : http://192.168.4.1
  STA  : 192.168.1.42                  <- "(not connected)" = wrong WiFi creds
  mDNS : active  ->  http://ir-remote.local
-- Security ------------------------------------
  API auth : disabled (open)           <- if "ENABLED" and you lost the pass,
================================================     that's why every API 401s
```

## How to read it for "a feature doesn't work"
- **Whole UI blank / nothing loads** → check `LittleFS` line; if `--`, the
  `littlefs.bin` was not flashed (or flashed to the wrong offset `0x310000`).
- **A radio feature (NFC/RFID/NRF24/SubGHz) dead** → check its `[OK]/[--]`
  line; `--` means the chip wasn't detected on the bus (wiring / power / pins).
- **Can't reach device** → check `STA` / `mDNS` lines.
- **Every API returns 401** → `API auth : ENABLED`; log in or reset auth.
- **Random reboots** → `Last reset` shows PANIC / BROWNOUT (usually a weak
  USB cable/supply) / watchdog.

Toggle everything off with `#define DIAG_VERBOSE 0` at the top of `main.cpp`.

---

## 4. Per-feature request log  (`[REQ]` lines)  — which tab/button works
Controlled by `#define DIAG_HTTP_LOG 1` in `include/config.h`.

**Every** API call now prints one line — tap any button and watch serial:
```
[REQ] POST   /api/transmit                       -> 200 OK
[REQ] GET    /api/status                          -> 200 OK
[REQ] POST   /api/sd/format                       -> 401 AUTH (login required)
[REQ] GET    /api/v1/rules/triggers               -> 404 MISSING
[REQ] POST   /api/macro/run                        -> 400 FAIL
```

This works without touching the ~300 individual handlers because every
response goes through the central `sendJson()` / `sendJsonDoc()` helpers,
and unknown routes fall through `onNotFound()` (also a `sendJson` 404).

**How to read the result word:**
| Word      | Meaning                                                            |
|-----------|--------------------------------------------------------------------|
| `OK`      | Feature worked (HTTP 2xx).                                          |
| `MISSING` | 404 — that route is **not in the firmware** (e.g. the Rules tab).  |
| `AUTH`    | 401/403 — blocked because login is enabled and you're not logged in.|
| `FAIL`    | 4xx/5xx — handler ran but rejected the input or errored.           |

**Workflow to find a dead button:** open serial, tap the button once.
- No `[REQ]` line at all → the UI never sent the request (front-end/JS issue,
  or you're looking at a stale cached `index.html`).
- `MISSING` → backend route doesn't exist for that feature.
- `AUTH`   → log in first (or disable auth).
- `FAIL`   → the request reached the handler; the body/params were rejected.
- `OK` but nothing happens physically → firmware is fine; check wiring/hardware
  (cross-check the BOOT SELF-TEST `[--]` lines for that module).

Set `DIAG_HTTP_LOG 0` to silence for production.

---

## 5. Web GUI fix — why the UI features were dead (`data/index.html.gz`)

**Root cause:** `const SdExtraUI` was declared **twice** at global scope in the
page's JavaScript (an old copy in the main script block and a newer complete
copy in the "extra modules" block). In a real browser, a duplicate top-level
`const` throws `SyntaxError: Identifier 'SdExtraUI' has already been declared`,
which **aborts that entire `<script>` block**. That block contained:

- `WpenUI` (the whole WiFi-Pen tab) + ~30 `wpen*` button handlers
- `NfcSdUI`, `RfidExtraUI`, `SubGhzExtraUI`, `Nrf24ExtraUI`,
  `SystemExtraUI`, `AuditExtraUI`, `RulesExtraUI`

So every button in those tabs silently did nothing (handler was `undefined`).
A normal per-block syntax check misses this because each `<script>` is valid on
its own — the collision only happens when they share one global scope in the
browser.

**Fix:** removed the stale older `SdExtraUI` copy, keeping the newer complete
one. Verified headlessly (jsdom) that the page now loads with **zero** errors
and all the above modules become defined.

**Also fixed:** an NTP timezone-hint line called `.toFixed()` on a string
(operator-precedence bug), throwing whenever the Scheduler tab polled NTP.

