# BareMetal STM32F446RE Drivers

Register-level (no HAL) drivers for the Nucleo-F446RE, built toward a flight controller.

## Board notes

- **Clock:** HSE via ST-LINK MCO must use `RCC_HSE_BYPASS`, not `RCC_HSE_ON` — there is no physical crystal on this board.
- **LD2 (onboard LED):** PA5, wired **active-low** (pin LOW = ON). Use `TIM_OCPOLARITY_LOW` if driving it via PWM.
- **PA1** doubles as **TIM2_CH2** — used for the PWM driver below.

## Driver status

### GPIO (`gpio.c/h`) — done
`GPIO_Init`, `GPIO_Set`, `GPIO_Toggle`. Basic push-pull output only.
- **Limitation:** no input mode, no pull-up/down, no open-drain support yet.

### UART (`uart.c/h`) — done
`UART_Init`, `UART_SendChar`, `UART_SendString`, `UART_ReceiveChar`, `UART_WaitForTC`.
Supports USART1/2/3, UART4/5, USART6. 8N1 only, no flow control.
- **Limitation:** `UART_ReceiveChar` is **blocking** (spins on RXNE). No non-blocking/interrupt/DMA receive yet — added a `UART_DataAvailable()` check in the app layer as a workaround.
- **Limitation:** no echo, no line-buffering — handled at the application layer intentionally (kept driver generic).

### TIM base (`tim.c/h`) — done
`TIM_Init` (explicit PSC/ARR), `TIM_Init_ms` (ms-based helper), `TIM_Start`, `TIM_Stop`, `TIM_Ready` (polls/clears UIF).
- **Limitation:** TIM2 only currently wired up for clock enable.
- **Limitation:** no interrupt-driven mode — `TIM_Ready()` requires polling.

### PWM (`pwm.c/h`) — done
`PWM_Init`, `PWM_Start`, `PWM_SetDuty`, `PWM_Stop`. PWM Mode 1, preload enabled (`OCxPE`), 1000-count period (ARR=999).
- **Limitation: only TIM2 Channel 2 (PA1) is implemented.** `channel` param exists but all internal logic is gated on `channel == 2` — passing 1/3/4 silently does nothing.
- **Limitation:** `dutyCycle` is 0-100, not range-checked — values >100 overflow past ARR and just pin the output at 100%.
- Confirm `fCK` in `pwm.c` matches the project's actual `SystemClock_Config()` before trusting frequency output — currently hardcoded, not calculated from RCC.

## Not started (needed for flight controller)

- **I2C** — next up, blocking on this for IMU (accel/gyro) reads.
- **Input capture** (TIM, opposite of PWM) — for reading RC receiver PWM/PPM.
- **ADC** — battery voltage sensing, analog RC input.
- **DMA** — needed once IMU reads move off polling; DAC/DMA experiment done separately, not yet reusable as a driver.
