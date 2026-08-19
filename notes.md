# Bare-Metal STM32F446 Peripheral Notes

Step-by-step register sequences for each peripheral, plus the math behind any timing calculations. Written as a personal reference for how/why each driver was built.

---

## GPIO

**Steps to configure a pin as output:**
1. Enable the GPIO port's clock in `RCC_AHB1ENR` (bit position = port index: A=0, B=1, C=2...)
2. Clear the pin's 2 bits in `MODER` (each pin gets 2 bits, at position `pin_number * 2`)
3. Set `MODER` bits to `01` for output mode (`00`=input, `01`=output, `10`=alternate function, `11`=analog)

**To toggle/set a pin:** write to `ODR` (Output Data Register), set the bit to drive HIGH, clear it to drive LOW. `GPIO_Toggle` XORs the bit instead of set/clear.

---

## UART

**Steps to initialize:**
1. Enable the USART/UART peripheral's clock (`RCC_APB1ENR` for USART2/3, UART4/5; `RCC_APB2ENR` for USART1/6, split because USART1/6 sit on the faster APB2 bus)
2. Enable the GPIO port clock for TX/RX pins
3. Clear then set `MODER` bits for TX/RX pins to alternate function (`10`)
4. Clear then set `AFR[]` to the correct AF number (AF7 for USART1/2/3, AF8 for UART4/5/USART6)
5. Calculate and write `BRR` (baud rate register), see equation below
6. Set `CR1`: enable `UE` (USART enable), `RE` (receiver enable), `TE` (transmitter enable)
7. Set `CR2` stop bits (00 = 1 stop bit)

**Baud rate (BRR) equation:**
```
USARTDIV = fCK / (16 × baudRate)
```
`USARTDIV` isn't a whole number in general. It has a fractional part, and `BRR` needs the whole part (mantissa) and fractional part (as a 4-bit value, 0-15) stored separately:
```
mantissa = floor(USARTDIV)
DIV_Fraction = round(16 × (USARTDIV - mantissa))
```
If `DIV_Fraction` rounds up to `16` (overflow), reset it to `0` and increment `mantissa` by 1 to carry over.
```
BRR = (mantissa << 4) | DIV_Fraction
```
Mantissa occupies bits 15:4, fraction occupies bits 3:0.

**Sending a char:** wait for `TXE` (bit 7 of `SR`) to be 1 (transmit buffer empty), then write the byte to `DR`.
**Receiving a char:** wait for `RXNE` (bit 5 of `SR`) to be 1 (data ready), then read `DR`.
**Checking data availability (non-blocking):** read `RXNE` once, don't wait, return true/false immediately.

---

## TIM (base timer, no PWM)

**Steps to initialize (explicit PSC/ARR):**
1. Enable the timer's clock (`RCC_APB1ENR` for TIM2-7, `RCC_APB2ENR` for TIM1/8-11)
2. Write `PSC` (prescaler - 1) and `ARR` (auto-reload - 1)
3. Reset `CNT` to 0
4. Set `EGR`'s `UG` bit to force an update event (loads PSC/ARR immediately rather than waiting for the next natural overflow)
5. Clear `SR`'s `UIF` bit (clean starting state)

**To start/stop:** set/clear `CR1`'s `CEN` bit (bit 0).
**To poll for a completed period:** check `SR`'s `UIF` bit; if set, clear it and report "ready."

**Timer frequency equation:**
```
Output frequency = Timer input clock / ((PSC + 1) × (ARR + 1))
```
Timer input clock isn't always the same as the APB bus clock, **if the APB prescaler isn't 1 (i.e. APB clock ≠ AHB clock), the timer clock is automatically doubled** by hardware. Example: APB1 = 45MHz (from a /4 prescaler off 180MHz AHB) → TIM2/4's actual input clock = 90MHz, not 45MHz.

---

## PWM

**Steps to initialize (TIM2 Channel 2 example):**
1. Enable timer clock + GPIO clock, same as base TIM
2. Configure the output pin as alternate function, correct AF number for the timer
3. Set `ARR` (period) and `PSC` (prescaler), same frequency equation as base TIM, since PWM runs on the same counter
4. Configure `CCMRx`'s `OCxM` bits to `110` (PWM Mode 1) and `OCxPE` bit to `1` (preload enabled, prevents mid-cycle glitches when duty cycle is updated while running)
5. Set `CCRx` to the initial duty cycle value

**To start:** set `CCER`'s `CCxE` bit (connects the channel's internal signal to the physical pin), then set `CR1`'s `CEN` bit (starts the counter).
**To stop:** clear `CCER`'s `CCxE` bit.
**To change duty cycle live:** just write a new value to `CCRx`, safe to do while running because `OCxPE` is enabled, so the change applies cleanly at the next period boundary instead of glitching mid-cycle.

**Duty cycle scaling:** if `ARR = 999` (1000 counts) and duty is expressed as a 0-100 percentage:
```
CCRx = dutyCycle × 10
```
(50% → CCRx = 500 → half of 1000 counts → 50% of each period spent "on.")

**CCMRx register layout (why two registers):** `CCMR1` covers channels 1-2, `CCMR2` covers channels 3-4. Each channel gets one byte within its register: channel 1 = bits 0-7, channel 2 = bits 8-15 (of CCMR1); channel 3 = bits 0-7, channel 4 = bits 8-15 (of CCMR2). Within each byte: `OCxPE` is at relative bit 3, `OCxM` is at relative bits 6:4, so for channel 2 specifically, that's absolute bit 11 (OC2PE) and bits 14:12 (OC2M).

**PWM Mode 1 vs Mode 2:** Mode 1 = output HIGH while counter < CCR, LOW after. Mode 2 = inverted (LOW while counter < CCR, HIGH after). Achieves the same kind of inversion as flipping `OCPolarity`, only need one or the other, not both (they'd cancel out).

**Active-high vs active-low wiring (why polarity matters):**
- Active-high (`GPIO → LED → GND`): pin HIGH = current flows = LED on. Bigger duty cycle = brighter, intuitively.
- Active-low (`3.3V → LED → GPIO`): pin LOW = current flows = LED on (pin sinks current to ground). Bigger duty cycle = pin spends more time HIGH = LED off more, inverted from what you'd expect. Fix by using `OCPOLARITY_LOW` instead of `HIGH`, or use PWM Mode 2 instead of Mode 1 (equivalent effect, pick one).

---

## I2C

**Steps to initialize (I2C1, PB8=SCL/PB9=SDA example):**
1. Enable I2C peripheral clock (`RCC_APB1ENR`) and GPIO clock
2. Configure SDA/SCL pins as alternate function, **AF4** (consistent across I2C1/2/3 on this chip)
3. Set `OTYPER` to open-drain and `PUPDR` to pull-up on both pins (I2C requires this, no device can actively drive the bus HIGH, only pull it LOW; the pull-up passively returns it to HIGH). `OTYPER` is 1 bit/pin (0=push-pull, 1=open-drain); `PUPDR` is 2 bits/pin like `MODER` (`01`=pull-up)
4. Set `CR2`'s `FREQ` bits (bits 5:0), tells the peripheral its own input clock speed, in MHz
5. Set `CCR`, the actual bus-speed divider (see equation below), plus the `F/S` bit (bit 15: 0=Standard mode, 1=Fast mode)
6. Set `TRISE`, max allowed SCL rise time, converted to clock cycles (see equation below)
7. Set `CR1`'s `PE` bit **last**, the peripheral locks CCR/TRISE once enabled, so they must be configured first

**Why I2C needs open-drain + pull-ups:** SDA and SCL are shared, multi-device lines. If a device could actively drive HIGH, two devices disagreeing on the line's state would short-circuit each other. Open-drain means each device can only pull LOW (safe to share); pull-up resistors passively bring the line back to HIGH when nothing's pulling it down. This is also why pulling LOW is fast (active transistor) but rising to HIGH is slower (passive RC charging through the pull-up resistor), directly relevant to the CCR/TRISE timing math below.

**CCR (clock divider) equation, derivation:**

The manual defines, for Standard mode:
```
Thigh = CCR × TPCLK1
Tlow  = CCR × TPCLK1
```
where `TPCLK1` is the *period* of the peripheral clock (`TPCLK1 = 1 / fPCLK1`), not the frequency.

Total SCL period = Thigh + Tlow = `2 × CCR × TPCLK1`. SCL frequency is the reciprocal of that period:
```
fSCL = 1 / (2 × CCR × TPCLK1)
```
Solving for CCR, and substituting `TPCLK1 = 1/fPCLK1`:
```
CCR = fPCLK1 / (2 × fSCL)      [Standard mode]
```
Fast mode (DUTY=0) uses `Tlow = 2 × CCR × TPCLK1` instead of `1×`, making the total period `3 × CCR × TPCLK1`, giving:
```
CCR = fPCLK1 / (3 × fSCL)      [Fast mode, DUTY=0]
```

**Worked example (fPCLK1 = 16MHz, Standard mode, fSCL = 100kHz):**
```
CCR = 16,000,000 / (2 × 100,000) = 16,000,000 / 200,000 = 80
```

**TRISE equation:**

Derived from the I2C bus spec's maximum allowed SCL rise time: 1000ns for Standard mode, 300ns for Fast mode (Fast mode's shorter clock period leaves less timing margin, so the allowed rise time is tighter).
```
TRISE = (fPCLK1 × maxRiseTime) + 1
```
Standard mode (1000ns), simplifies to just FREQ in MHz + 1.
