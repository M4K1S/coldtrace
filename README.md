# BareMetal — Cold-Storage Monitor

Application layer for an STM32F446RE (Nucleo-F446RE) temperature/RTC/SD datalogger. Register-level peripheral drivers (no HAL) live in a separate repo, pulled in here as a git submodule — see below.

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

## Not started (needed for flight controller)

- **Input capture** (TIM, opposite of PWM) — for reading RC receiver PWM/PPM.
- **ADC** — battery voltage sensing, analog RC input.
- **DMA** — needed once IMU reads move off polling; DAC/DMA experiment done separately, not yet reusable as a driver.
