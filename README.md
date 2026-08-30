# BareMetal — Cold-Storage Monitor

Application layer for an STM32F446RE (Nucleo-F446RE) temperature/RTC/SD datalogger. Register-level peripheral drivers (no HAL) live in a separate repo, pulled in here as a git submodule — see below.

## How it all works

**Hardware, and what's wired where:**

| Peripheral | Bus/pins | Purpose |
|---|---|---|
| DS18B20 temp sensor | 1-Wire, bit-banged on PA10, TIM2 for µs timing | The actual temperature reading |
| DS3231 RTC | I2C1, PB8=SCL/PB9=SDA, TIM3 for I2C timeouts | Wall-clock time, battery-backed across power loss |
| SD card | SPI1, PA5=SCK/PA6=MISO/PA7=MOSI, PA4=CS, TIM5 for SPI timeouts | Persistent datalog storage |
| SSD1306 OLED | I2C3, PA8=SCL/PC9=SDA, shares TIM3 (RTC's timer) for I2C timeouts | At-a-glance status display |
| USART2 | 115200 8N1 | Debug/status text, and the one-time "type in the current time" prompt |
| — | TIM4 | Not tied to any sensor — just paces the main loop |

I2C3 (not I2C2) was used for the OLED specifically because I2C2's default SDA pin (`PB11`) isn't broken out on this Nucleo-64 board's headers at all — confirmed against the board's own pinout diagram, not a wiring mistake. See `BareMetalDrivers/notes.md`'s I2C section for the full story.

**Boot sequence (`App_Init`, runs once):**
1. Bring up UART, the 1-Wire pin, and I2C1 (RTC bus).
2. Check the DS3231's Oscillator Stop Flag — if the RTC's backup battery has ever been fully dead, it won't have a trustworthy time, so this blocks on a UART prompt asking for one (`YYMMDDHHMMSSD`) before continuing.
3. Bring up I2C3 and initialize the SSD1306. If it doesn't ACK, `oled_ready` stays `0` and every later screen-draw call is just skipped — no OLED, no crash.
4. Bring up SPI1 slow (≤400kHz, required during card identification), run the SD card's identification sequence, then switch SPI to full speed and initialize the on-card log layout. If either step fails, `sd_logging_ready` stays `0` and logging is silently skipped later, same pattern as the OLED.

**Main loop (`App_Loop`, repeats roughly once a second):**
1. Read the current time from the DS3231, print it over UART.
2. Trigger a DS18B20 conversion, wait out its worst-case 750ms conversion time, read the result back (CRC-checked — a bad 1-Wire transfer is detected and the reading is thrown away rather than logged as garbage).
3. Print the temperature over UART.
4. If the SD card came up and a time reading succeeded: append a `(timestamp, temp, door_state)` record to the log, and remember this moment as the "last refresh" time shown on the OLED.
5. If the OLED came up: redraw the whole status screen (wifi/SD icons, last-refresh time, big temp reading) and push it to the panel.
6. Sleep out the rest of the ~1s loop period, repeat from 1.

**What the OLED actually shows, top to bottom:** a wifi icon top-left (currently always hidden — no wifi hardware exists yet, this is a stub for later), an SD-card icon next to it (only shown once the card + log layer both came up cleanly, driven by the same `sd_logging_ready` flag the logger itself uses), a refresh icon top-right with the `HH:MM` of the last successful SD write next to it, and a large centered temperature reading to 2 decimal places (e.g. `-12.34°`). See the "OLED status display" section under App layer below for the design choices behind that layout, and `BareMetalDrivers/notes.md`'s SSD1306 section for exactly how each element is drawn.

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
Wired into `app.c`: `App_Init()` brings up I2C3 (`OLED_I2C`/`OLED_TIM`=I2C3/RTC_TIM) and calls `ssd1306_init()`; `App_Loop()` calls `oled_draw_status_screen()` + `oled_update()` once per loop, right after the SD-log-write attempt. Uses the drawing primitives added to `ssd1306.c` — see `BareMetalDrivers/notes.md`'s SSD1306 section for how each one actually works; this section is about the choices made at the app-integration level.

**Design choices, and why:**
- **Wifi icon always hidden (`wifi_connected` hardcoded `0`).** There's no wifi hardware or driver on this project yet — rather than fake a "connected" state or leave a half-wired stub that could get mistaken for something real, the flag is a plain `const uint8_t 0` with a comment explaining it's a placeholder. Flip it to a real signal once wifi exists; no other code needs to change, `oled_draw_status_screen()` already treats it as just another boolean input.
- **SD icon and "last refresh" time are both real status, not simulated.** The SD icon reuses `sd_logging_ready` — the exact same flag the datalogger itself checks before writing — so the icon can never claim the card is fine when logging actually isn't happening. The refresh timestamp only updates inside the `if (!log_append_reading(...))` `else` branch, i.e. only on a write that actually succeeded, not merely attempted; a fresh boot with zero successful writes correctly shows the refresh icon alone with no time next to it, rather than a misleading `00:00`.
- **Temperature shown to 2 decimal places, not rounded to a whole degree.** Originally rounded (simpler, and the "big" font was assumed to only need whole numbers), changed after actually looking at it on hardware — 2dp is what you'd want for something billed as a cold-storage *monitor*, where the difference between borderline-fine and borderline-failing can be under a degree. `oled_draw_status_screen()` takes the temperature pre-converted to hundredths (`int32_t temp_centi`, e.g. `-1234` → `-12.34`) rather than a `float`, matching the same fixed-point convention `UART_SendTemp()` already used for the exact same reason (no libc float-printf dependency).
- **I2C3, not a shared I2C1 bus with the RTC.** Electrically either would work (I2C is a shared bus by design), but I2C2's usual SDA pin isn't broken out on this Nucleo board at all — see the "How it all works" section up top and `BareMetalDrivers/notes.md` for the full story of how that was discovered and why I2C3 was the fallback.
- **Timeout timer shared with the RTC (`OLED_TIM` = `RTC_TIM`), rather than a fourth dedicated one.** This timer is only ever used as a disposable microsecond stopwatch during I2C timeout polling — never left running, never holds state between calls — and every I2C transaction in `app.c` is blocking and sequential, so there's no scenario where two timeout checks could race on the same counter. Saves a timer peripheral that would otherwise sit idle.

**Limitation:** the "big" enlarged digits are a software-scaled version of the small 8x11 font (see `notes.md`), not real hand-drawn large glyphs — the `OLED_BigDigits` bitmap set exists in `oled_fonts.c` but was never finished. Good enough for a legible v1 reading; worth revisiting if a more polished look is wanted later.

## Not started (needed for flight controller)

- **Input capture** (TIM, opposite of PWM) — for reading RC receiver PWM/PPM.
- **ADC** — battery voltage sensing, analog RC input.
- **DMA** — needed once IMU reads move off polling; DAC/DMA experiment done separately, not yet reusable as a driver.
