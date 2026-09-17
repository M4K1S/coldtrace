# BareMetal — Cold-Storage Monitor

Application layer for an STM32F446RE (Nucleo-F446RE) temperature/RTC/SD datalogger. Register-level peripheral drivers (no HAL) live in a separate repo, pulled in here as a git submodule — see below.

## How it all works

**Hardware, and what's wired where:**

| Peripheral | Bus/pins | Purpose |
|---|---|---|
| DS18B20 temp sensor | 1-Wire, bit-banged on PC0, TIM2 for µs timing | The actual temperature reading |
| DS3231 RTC | I2C3, PA8=SCL/PC9=SDA, TIM3 for I2C timeouts | Wall-clock time, battery-backed across power loss |
| SSD1306 OLED | Own I2C1 bus, PB8=SCL/PB9=SDA, shares TIM3 for I2C timeouts | At-a-glance status display |
| SD card | SPI1, PA5=SCK/PA6=MISO/PA7=MOSI, PB12=CS, TIM5 for SPI timeouts | Persistent datalog storage |
| ESP8266 (ESP-01S) | UART4, PC10=TX/PC11=RX, 115200 8N1, shares TEMP_TIM (TIM2) for AT-command timeouts | Wifi uplink — posts each reading to the server over HTTP |
| Red LED | GPIO output, PC5 | Temp-over-threshold alarm (`temp_threshold_c`, default 28.0°C) — takes priority over the green LED |
| Green LED | GPIO output, PB1 | Door-closed indicator, off whenever the red alarm is active |
| Reed switch (door sensor) | GPIO input, PC13, needs an external pull-up (no internal PUPDR support) | LOW = magnet present = door closed |
| — | TIM4 | Not tied to any sensor — just paces the main loop |

**Reserved, not yet wired up in code:** PC2 for a buzzer — set aside so adding it later doesn't collide with anything already wired.

**There is no debug UART.** UART4/PC10/PC11 is dedicated entirely to the ESP8266's AT-command channel (moved off USART1/PA9-PA10 during bring-up) — anything else writing to that UART would corrupt AT commands or responses coming back from the module, so all the old debug prints were removed rather than sharing the line. The OLED's wifi icon is the only "is the ESP link up" indicator now (see below). Getting this link actually working took a long, genuinely instructive bring-up process — missing ground, a UART-peripheral/pin mismatch, and two capture-code bugs all masqueraded as ESP flakiness before being found; see `BareMetalDrivers/notes.md`'s "ESP8266 hardware bring-up" section for the full writeup.

The OLED previously shared the RTC's I2C bus, then moved to its own I2C3 bus (PA8=SCL/PC9=SDA) because I2C2's default SDA pin (`PB11`) isn't broken out on this Nucleo-64 board's headers — see `BareMetalDrivers/notes.md`'s I2C section for that story. It's since settled on its own I2C1 bus, with the RTC moved to I2C3, once the ESP8266/LED/reed-switch pin additions needed the room.

**Boot sequence (`App_Init`, runs once):**
1. Bring up the 1-Wire pin, I2C3 (RTC bus), the delay timer, the status LEDs, and the reed switch input.
2. Bring up I2C1 and initialize the SSD1306. If it doesn't ACK, `oled_ready` stays `0` and every later screen-draw call is just skipped — no OLED, no crash.
3. Check the DS3231's Oscillator Stop Flag — if the RTC's backup battery has ever been fully dead, it won't have a trustworthy time, so the time is silently defaulted to 12:00:00 instead of blocking boot on a UART prompt (there's no UART free for one anyway).
4. Bring up UART4 for the ESP8266 and attempt `AT+CWMODE=1` + `AT+CWJAP=...` (SSID/password/host live in `Core/Inc/secrets.h`, gitignored — not checked into the repo). Result feeds `wifi_connected`, which drives the OLED's wifi icon.
5. Bring up SPI1 slow (≤400kHz, required during card identification), run the SD card's identification sequence, then switch SPI to full speed and initialize the on-card log layout. If either step fails, `sd_logging_ready` stays `0` and logging is silently skipped later, same pattern as the OLED.

**Main loop (`App_Loop`, repeats roughly once a second):**
1. Read the current time from the DS3231.
2. Read the reed switch and update the status LEDs immediately (covers the case where the DS18B20 read below fails and skips its own LED-refreshing polling loop).
3. Trigger a DS18B20 conversion, and while waiting out its worst-case 750ms conversion time, re-poll the reed switch/LEDs every 25ms instead of one long blocking wait, so the door sensor stays responsive. Read the result back (CRC-checked — a bad 1-Wire transfer is detected and the reading is thrown away rather than logged as garbage).
4. If the SD card came up and a time reading succeeded: append a `(timestamp, temp, door_state)` record to the log, and remember this moment as the "last refresh" time shown on the OLED. If the ESP8266 reported a successful wifi join, also POST the same reading to the server over HTTP via `esp_send_reading`.
5. If the OLED came up: redraw the whole status screen (wifi/SD icons, last-refresh time, big temp reading) and push it to the panel.
6. Sleep out the rest of the ~1s loop period (polled the same 25ms-step way as step 3), repeat from 1.

**What the OLED actually shows, top to bottom:** a wifi icon top-left (now driven by `wifi_connected`, the ESP8266's real `AT+CWJAP` result), an SD-card icon next to it (only shown once the card + log layer both came up cleanly, driven by the same `sd_logging_ready` flag the logger itself uses), a refresh icon top-right with the `HH:MM` of the last successful SD write next to it, and a large centered temperature reading to 1 decimal place (e.g. `-12.5°`), built from real digit art cropped to its ink and scaled up 2x. See the "OLED status display" section under App layer below for the design choices behind that layout, and `BareMetalDrivers/notes.md`'s SSD1306 section for exactly how each element is drawn.

## Server side (`coldtrace-server`)

A minimal Flask app (separate repo, `coldtrace-server`) receives the POSTs `esp_send_reading` makes:
- `POST /api/reading` — expects JSON `{"temp": <float>, "door": 0|1, "timestamp": <uint32>}`, appends it (plus a server-side UTC receive time) to `readings.jsonl`.
- `GET /dashboard` — renders the last 50 readings as an HTML table.

Deployed on [Render](https://render.com)'s free tier: push the Flask repo (`app.py` + `requirements.txt` + a `Procfile` running `gunicorn app:app`) to GitHub, then create a Render Web Service pointed at it (build command `pip install -r requirements.txt`, start command `gunicorn app:app`). Render gives back a URL like `https://coldtrace-server.onrender.com` — that's the value in `app.c`'s `ESP_SERVER_HOST`. Free-tier services spin down after inactivity and take a few seconds to wake on the next request; fine for this project's occasional-reading traffic pattern, just not instant on a cold start.

## Board notes

- **Clock:** HSE via ST-LINK MCO must use `RCC_HSE_BYPASS`, not `RCC_HSE_ON` — there is no physical crystal on this board.
- **LD2 (onboard LED):** PA5, wired **active-low** (pin LOW = ON). Use `TIM_OCPOLARITY_LOW` if driving it via PWM.
- **PA1** doubles as **TIM2_CH2** — used by the PWM driver.

## Driver library (submodule)

The GPIO/UART/TIM/PWM/I2C/SPI/OneWire/DS18B20/DS3231/SD drivers used by this project live in [`BareMetalDrivers/`](BareMetalDrivers/), a submodule pointing at [stm32-baremetal-drivers](https://github.com/M4K1S/stm32-baremetal-drivers). See that repo's own README and `notes.md` for driver-level documentation (register sequences, timing math, datasheet values, and the bug writeups for things like the SPI `SSM`/`SSI` Mode Fault and SDHC/SDSC addressing).

**Cloning this repo:** the submodule isn't populated by a plain `git clone` — run:
```bash
git clone --recurse-submodules <this-repo-url>
```
or, if already cloned without that flag:
```bash
git submodule update --init
```

**Updating to a newer driver version:** this repo is pinned to one specific commit of the driver repo, on purpose — it does *not* auto-update when the driver repo changes. To pull in a newer driver version deliberately:
```bash
cd BareMetalDrivers
git pull origin main      # or check out whatever commit/tag you want
cd ..
git add BareMetalDrivers
git commit -m "Bump BareMetalDrivers to <reason>"
```
That commit is what actually "locks in" the update for this project — until you do that, this repo keeps building against whatever commit it's currently pinned to, even if the driver repo has moved on.

## App layer

### Datalogger (`log.c/h`) — done
`log_init`, `log_append_reading`, `log_save_position`, `log_flush`. Buffers 64 8-byte `LogRecord`s (timestamp, temp×100, door state) per 512-byte SD block, tracks the next-write block in a reserved metadata block. Built on `sd.c`.
- Wired into `app.c`: `App_Init()` brings up SPI1 + the card (`SD_SPI`/`SD_TIM`=TIM5/`SD_CS_PORT`+`SD_CS_PIN`=PA4), `App_Loop()` calls `log_append_reading()` right after the existing UART temp print, whenever both the card and the RTC read succeeded.
- **Limitation:** `timestamp` is seconds-since-midnight, not a real epoch (the RTC driver doesn't expose one) — fine for ordering readings within a day, wraps at midnight. `door_state` is hardcoded to `0`, no door sensor exists yet.

### OLED status display — done (v1)
Wired into `app.c`: `App_Init()` calls `ssd1306_init()` on `OLED_I2C`/`OLED_TIM` (`RTC_I2C`/`RTC_TIM` — the OLED shares the RTC's already-initialized I2C1 bus, no separate `I2C_Init()` call needed); `App_Loop()` calls `oled_draw_status_screen()` + `oled_update()` once per loop, right after the SD-log-write attempt. Uses the drawing primitives added to `ssd1306.c` — see `BareMetalDrivers/notes.md`'s SSD1306 section for how each one actually works; this section is about the choices made at the app-integration level.

**Design choices, and why:**
- **Wifi icon now reflects the real ESP8266 link state.** Started as a hardcoded `const uint8_t 0` placeholder (no wifi hardware existed yet at the time) specifically so `oled_draw_status_screen()` never needed to change later — once `esp_connect_wifi()`'s `AT+CWJAP` result was available, it just replaced the placeholder's value, same boolean input as before.
- **SD icon and "last refresh" time are both real status, not simulated.** The SD icon reuses `sd_logging_ready` — the exact same flag the datalogger itself checks before writing — so the icon can never claim the card is fine when logging actually isn't happening. The refresh timestamp only updates inside the `if (!log_append_reading(...))` `else` branch, i.e. only on a write that actually succeeded, not merely attempted; a fresh boot with zero successful writes correctly shows the refresh icon alone with no time next to it, rather than a misleading `00:00`.
- **Temperature shown to 1 decimal place, not rounded to a whole degree.** Went whole-degree, then 2 decimal places, then settled on 1 — each change came from actually looking at the display on hardware rather than guessing. 2 decimal places doesn't fit alongside a sign, big digits, and a degree mark on a 128px panel (see the big-digit note below), so 1dp is the balance between precision and everything actually fitting on screen. `oled_draw_status_screen()` takes the temperature pre-converted to tenths (`int32_t temp_deci`, e.g. `-125` → `-12.5`) rather than a `float`, matching the same fixed-point convention `UART_SendTemp()` already used for the exact same reason (no libc float-printf dependency).
- **Shares the RTC's I2C1 bus, not its own I2C3 bus.** Ran on a separate I2C3 bus (PA8=SCL/PC9=SDA) for a while, needed at the time because I2C2's usual SDA pin isn't broken out on this Nucleo board — see the "How it all works" section up top. Consolidated back onto the shared I2C1 bus as part of a full pin-assignment pass across the whole board (freeing up room for planned additions like the ESP8266/reed switch/buzzer elsewhere on the pinout). Electrically this was always fine either way — I2C is a shared bus by design — splitting it onto its own bus was purely about pin availability at the time, never a technical requirement.
- **Timeout timer shared with the RTC (`OLED_TIM` = `RTC_TIM`), rather than a fourth dedicated one.** This timer is only ever used as a disposable microsecond stopwatch during I2C timeout polling — never left running, never holds state between calls — and every I2C transaction in `app.c` is blocking and sequential, so there's no scenario where two timeout checks could race on the same counter. Saves a timer peripheral that would otherwise sit idle.

- **Big digits and the minus sign are real art, extracted from an existing pixel font — not the small font scaled up, and not a rasterized TrueType face.** Went through both of those first: scaling the small 8x11 font up was a stopgap; rendering fresh digit art from Arial Bold looked fine in preview but rasterized rough once actually on the panel (a font designed for hard 1-bit pixel grids holds up at small sizes in a way an antialiased one doesn't). See `BareMetalDrivers/notes.md` for the full back-and-forth. The width constraint that drove 1 decimal place over 2 applies regardless of digit source: a sign + 3 integer digits + point + 2 fraction digits + degree mark doesn't fit on a 128px panel at a legible size, but the DS18B20's actual range (-55°C to +125°C, negative readings never 3 digits) fits comfortably at 1 decimal place.
- **Digits are cropped to their real ink and scaled 2x, not drawn at the source font's full cell size.** First real-hardware photo of this screen showed two bugs: the degree mark floating well above the "23.3" it belonged to, and way more gap between digits than the code's `BIG_GAP` should produce. Both traced to the same cause — the source font's cells are padded with blank rows/columns the digits don't use (measured directly: ink only fills rows 20-35/cols 1-11 of an 18x36 cell). Cropping to that measured box before scaling fixes both, and doubles the digits in size as a side effect.
- **Degree mark is a small hand-drawn ring, not a square outline.** The original version was four straight edges forming a box — recognizable as "a small square," not as a degree sign. Replaced with an explicit pixel mask with the corners left blank, which actually reads as a ring at this size.

## Not started (needed for flight controller)

- **Input capture** (TIM, opposite of PWM) — for reading RC receiver PWM/PPM.
- **ADC** — battery voltage sensing, analog RC input.
- **DMA** — needed once IMU reads move off polling; DAC/DMA experiment done separately, not yet reusable as a driver.
