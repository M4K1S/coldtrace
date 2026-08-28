# BareMetal STM32F446RE Drivers

Register-level (no HAL) drivers for the Nucleo-F446RE, built toward a flight controller.

## Board notes

- **Clock:** HSE via ST-LINK MCO must use `RCC_HSE_BYPASS`, not `RCC_HSE_ON` — there is no physical crystal on this board.
- **LD2 (onboard LED):** PA5, wired **active-low** (pin LOW = ON). Use `TIM_OCPOLARITY_LOW` if driving it via PWM.
- **PA1** doubles as **TIM2_CH2** — used for the PWM driver below.

## Driver status

### GPIO (`gpio.c/h`) — done
`GPIO_Init`, `GPIO_SetMode`, `GPIO_Set`, `GPIO_Toggle`, `GPIO_Read`. Input/Output/AltFunc/Analog modes, push-pull or open-drain output type.
- `GPIO_Init` applies `OTYPER` (push-pull/open-drain) for both `OUTPUT` and `ALT_FUNC` modes — it used to only apply it for `OUTPUT`, silently dropping open-drain config on any AF pin.
- **Limitation:** no `PUPDR` (pull-up/down) or `OSPEEDR` (speed) control — peripherals that need an internal pull-up (I2C) configure those registers by hand instead of going through `GPIO_Init`.

### UART (`uart.c/h`) — done
`UART_Init`, `UART_SendChar`, `UART_SendString`, `UART_ReceiveChar`, `UART_WaitForTC`, `UART_DataAvailable`.
Supports USART1/2/3, UART4/5, USART6. 8N1 only, no flow control.
- **Limitation:** `UART_ReceiveChar` is **blocking** (spins on RXNE). No non-blocking/interrupt/DMA receive yet — added a `UART_DataAvailable()` check in the app layer as a workaround.
- **Limitation:** no echo, no line-buffering — handled at the application layer intentionally (kept driver generic).

### TIM base (`tim.c/h`) — done
`TIM_Init` (explicit PSC/ARR), `TIM_Delay_us`, `TIM_Delay_ms`, `TIM_Start`, `TIM_Stop`, `TIM_Ready` (polls/clears UIF).
- **Limitation:** no interrupt-driven mode — `TIM_Ready()` requires polling.
- Every subsystem that needs microsecond timing (OneWire, I2C, SPI) gets its own dedicated `TIM_TypeDef*` passed in rather than sharing one timer — see `app.c`'s `TEMP_TIM`/`RTC_TIM`/`DELAY_TIM`.

### PWM (`pwm.c/h`) — done
`PWM_Init`, `PWM_Start`, `PWM_SetDuty`, `PWM_Stop`. PWM Mode 1, preload enabled (`OCxPE`), 1000-count period (ARR=999).
- **Limitation: only TIM2 Channel 2 (PA1) is implemented.** `channel` param exists but all internal logic is gated on `channel == 2` — passing 1/3/4 silently does nothing.
- **Limitation:** `dutyCycle` is 0-100, not range-checked — values >100 overflow past ARR and just pin the output at 100%.
- `fCK` now comes from the shared `FCK_HZ` define in `clock.h` (was duplicated as a local constant in every peripheral driver) — confirm it still matches the project's actual `SystemClock_Config()` before trusting frequency output.

### I2C (`i2c.c/h`) — done
`I2C_Init`, `I2C_Start`, `I2C_Stop`, `I2C_SendAddress`, `I2C_WriteByte`, `I2C_ReadByte`, `I2C_WriteReg`, `I2C_ReadReg`, `I2C_WriteBytes`, `I2C_ReadBytes`. I2C1 (PB8=SCL/PB9=SDA), Standard (100kHz) or Fast (400kHz) mode. Timeout-guarded (`I2C_TIMEOUT_US`) via a caller-supplied `tim_port` — see `notes.md` for the full register sequence.
- **Limitation:** I2C1 only wired up.

### OneWire (`onewire.c/h`) — done
`ow_init`, `ow_reset`, `ow_write_bit`, `ow_read_bit`, `ow_write_byte`, `ow_read_byte`. Bit-banged over GPIO, timed via a dedicated `TIM_Delay_us`.

### DS3231 RTC (`ds3231.c/h`) — done
`rtc_set_time`, `rtc_get_time`, `rtc_time_is_valid`, `bcd_to_dec`/`dec_to_bcd`. Built on I2C; `App_Init()` prompts for a time-set over UART only when `rtc_time_is_valid()` (OSF flag) says the clock lost power.

### DS18B20 temp sensor (`ds18b20.c/h`) — done
`ds18b20_start_conversion`, `ds18b20_read_temp`. Built on OneWire.
- `ds18b20_read_temp` now reads the full 9-byte scratchpad and checks it against the DS18B20's own Dallas/Maxim CRC8 (byte 8), rejecting the reading on a mismatch — it used to trust the 2 temperature bytes unconditionally with no way to detect a corrupted 1-Wire transfer.

### SPI (`spi.c/h`) — done
`SPI_Init`, `SPI_TransferByte`. SPI1 (PA5/6/7), Mode 0, 8-bit frames, selectable prescaler (÷2-÷256). Timeout-guarded (`SPI_TIMEOUT_US`, was referenced but never `#define`'d — fixed).

### SD card over SPI (`sd.c/h`) — done
`sd_send_command`, `sd_init`, `sd_read_block`, `sd_write_block`. SPI-mode SD, not the board's onboard SDIO slot (see `notes.md` for why SDIO was skipped for v1).
- Every `SPI_TransferByte` call is now checked for a timeout and aborts (deselecting CS) instead of silently continuing on a failed transfer — previously the return value was ignored everywhere in this file.
- `sd_init` now remembers the SDHC/SDSC (`is_sdhc`) flag it detects from the OCR and `sd_read_block`/`sd_write_block` convert `addr` to a byte offset for SDSC cards — previously `is_sdhc` was computed but never used, so an SDSC card would have been addressed incorrectly.
- **Not yet wired into `app.c`** — `sd_init`/`SPI_Init` aren't called from the application layer yet, so the datalogger below isn't reachable at runtime.
- **Limitation:** nothing enforces the SD spec's ≤400kHz SPI clock requirement during the CMD0/CMD8/ACMD41 identification phase — the `SPI_Init` prescaler is entirely the caller's responsibility.

### Datalogger (`log.c/h`) — done
`log_init`, `log_append_reading`, `log_save_position`, `log_flush`. Buffers 64 8-byte `LogRecord`s (timestamp, temp×100, door state) per 512-byte SD block, tracks the next-write block in a reserved metadata block. Built on `sd.c` — same "not yet wired into `app.c`" caveat as above.

## Not started (needed for flight controller)

- **Input capture** (TIM, opposite of PWM) — for reading RC receiver PWM/PPM.
- **ADC** — battery voltage sensing, analog RC input.
- **DMA** — needed once IMU reads move off polling; DAC/DMA experiment done separately, not yet reusable as a driver.
