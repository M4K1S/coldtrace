# Bare-Metal STM32F446 Peripheral Notes

Step-by-step register sequences for each peripheral, plus the math behind any timing calculations. Written as a personal reference for how/why each driver was built.

---

## GPIO

**`GPIO_Mode` enum values are deliberately chosen to match `MODER`'s actual bit encoding:**
```c
typedef enum { INPUT, OUTPUT, ALT_FUNC, ANALOG } GPIO_Mode;   // 00, 01, 10, 11
typedef enum { PUSH_PULL, OPEN_DRAIN } GPIO_OutputType;        // 0, 1
```
Because C enums auto-assign sequential values from 0, and those values happen to equal the register's real 2-bit (`MODER`) and 1-bit (`OTYPER`) codes, both registers can be set directly from the enum value with no if-chain:
```c
port->MODER |= (mode << (pin * 2));
port->OTYPER |= (type << pin);
```

**Steps to configure a pin (`GPIO_Init`, one-time setup):**
1. Enable the GPIO port's clock in `RCC_AHB1ENR` (bit position = port index: A=0, B=1, C=2...)
2. Clear the pin's 2 bits in `MODER` (each pin gets 2 bits, at position `pin_number * 2`), then set from the `GPIO_Mode` value
3. If `mode == OUTPUT`: clear then set the pin's 1 bit in `OTYPER` from the `GPIO_OutputType` value. `OTYPER` is irrelevant/ignored for Input, AF, or Analog modes, so it's only touched in the Output branch

**`GPIO_SetMode` (lightweight, for switching direction mid-transaction):** only touches `MODER`, does not re-enable clocks or touch `OTYPER`. Needed for protocols like 1-Wire that flip a single pin between Output and Input repeatedly within one transaction, `OTYPER`'s open-drain setting (configured once via `GPIO_Init`) persists correctly across these switches since `SetMode` never clears it.

**Push-pull vs open-drain, and why 1-Wire (and I2C) need open-drain:** push-pull can actively drive both HIGH and LOW. Open-drain can only actively pull LOW, releasing (floating) is the only way to go HIGH, relying on an external pull-up resistor to do it. Necessary any time multiple devices might drive the same line, so two devices disagreeing can never short-circuit each other, worst case is one device's LOW wins.

**To write a pin:** `ODR` (Output Data Register), set the bit to drive HIGH, clear it to drive LOW. `GPIO_Toggle` XORs the bit instead of set/clear. Only meaningful once a pin is in Output mode.

**To read a pin:** `IDR` (Input Data Register), shift the target bit down to position 0 and mask with `& 0b1U` to isolate it:
```c
uint8_t GPIO_Read(GPIO_TypeDef *port, uint8_t pin) {
    return (port->IDR >> pin) & 0b1U;
}
```
Needed any time an open-drain line is being driven by something else (another device pulling it low) and you need to sense that state, rather than just recalling what you last wrote to `ODR`.

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

---

## I2C transaction primitives (built on I2C_Init)

All timeout-capable functions below share the identical structure: reset the timeout timer's `CNT` to 0, start it, poll a status flag while checking `CNT` against `I2C_TIMEOUT_US` (5000, i.e. 5ms, applied uniformly rather than tuned per-function), stop the timer, return 1 (success) or 0 (timeout). Every I2C function that waits on an external device or the peripheral's internal state machine needs this, since a bare `while(!flag);` hangs forever if a device doesn't ACK.

**`I2C_Start()`:** sets `CR1`'s `START` bit (bit 8), polls `SR1`'s `SB` flag (bit 0). Hardware auto-clears `START` once the Start condition is generated, no manual clear needed.

**`I2C_Stop()`:** sets `CR1`'s `STOP` bit (bit 9). No flag to wait on, hardware auto-clears it once the Stop condition completes.

**`I2C_SendAddress(addr, rw)`:** formats the address byte as `(addr << 1) | rw` (7-bit address shifted up, R/W bit in position 0; per I2C convention `rw=0` is write, `rw=1` is read), writes it to `DR`, polls `SR1`'s `ADDR` flag (bit 1).

**Clearing `ADDR` requires a specific two-register read sequence, not a direct bit clear:** read `SR1` then read `SR2`, discarding both values with `(void)`. This isn't optional or just informational, it's the literal hardware mechanism that clears the flag, skipping either read leaves `ADDR` stuck set and corrupts the next operation. `SR2` itself holds additional transaction-state bits (`MSL`, `BUSY`, `TRA`) that aren't used here but are part of why the two-step sequence exists.

**`I2C_WriteByte(byte)`:** writes to `DR`, polls `SR1`'s `TXE` flag (bit 7, "transmit register empty" = ready for the next byte to send).

**`I2C_ReadByte(ack, *byte_out)`:** polls `SR1`'s `RXNE` flag (bit 6, "receive register not empty" = a byte has arrived). The `ACK` bit (`CR1` bit 10) must be set **before** the wait loop, not after, because the hardware auto-generates the ACK/NACK for a byte at the moment that byte finishes arriving, based on whatever `ACK` was set to at that moment. Setting it after `RXNE` fires is too late, that byte's ACK/NACK has already been sent. `ACK=1` tells the sender "keep going" (use for all but the last byte of a multi-byte read); `ACK=0` tells it "stop" (use for the final byte).

**Output-parameter pattern for functions returning data (`I2C_ReadByte`, and `ds18b20_read_temp` updated to match):** return value is dedicated purely to success/failure (1/0), actual data comes back through a pointer parameter (`*byte_out`, `*temp_out`) that the function writes into via the caller's address (`&variable`). This avoids the sentinel-value problem, a `uint8_t` byte has no value that's safely "impossible" to use as an error marker (unlike, say, `-999°C` for a temperature), since any of the 256 possible byte values could be genuinely valid data.

---

## TIM microsecond delay (`TIM_Delay_us`)

1-Wire's timing (tens to hundreds of microseconds) is too fast and too precise for a software counting loop, needs a hardware timer with a known, exact tick rate.

**Setup:** call `TIM_Init(port, prescaler, autoReload)` once with a prescaler chosen so the tick rate is exactly 1MHz (1 tick = 1µs), so the delay function needs no extra math at the call site:
```
PSC + 1 = timer_input_clock / 1,000,000
```
At 16MHz input clock: `PSC = 16,000,000/1,000,000 - 1 = 15`, so call `TIM_Init(port, 16, 65535)` (large `autoReload` just gives headroom, actual delay length is controlled per-call, not by ARR).

**The delay function itself:** reset `CNT` to 0, start the counter, busy-wait until `CNT` reaches the requested microsecond count, stop the counter:
```c
void TIM_Delay_us(TIM_TypeDef *port, uint32_t micros){
    port->CNT = 0;
    port->CR1 |= (0b1U);
    while(port->CNT < micros) {}
    port->CR1 &= ~(0b1U);
}
```
Resetting `CNT` every call sidesteps any overflow/wraparound concerns entirely, since no 1-Wire delay used here comes anywhere close to the counter's max value (65535 for a 16-bit timer).

---

## OneWire (1-Wire protocol, DS18B20)

No dedicated peripheral, bit-banged over GPIO + `TIM_Delay_us`. Every timing number below comes from the DS18B20 datasheet's AC Electrical Characteristics table and Figure 15/16 timing diagrams.

**One-time setup (`ow_init`, called once at startup, not per-transaction):**
- `GPIO_Init(gpio_port, pin, OUTPUT, OPEN_DRAIN)`, the DQ pin must be open-drain since the DS18B20 can only pull the line LOW, never drive it HIGH, an external ~4.7kΩ pull-up resistor between DQ and VCC does the rest
- `TIM_Init(tim_port, 16, 65535)`, configures the 1MHz tick rate `TIM_Delay_us` needs

**`ow_reset()`, the handshake every single transaction begins with:**
1. Output mode, pull DQ low, hold **480µs minimum** (`tRSTL`)
2. Switch to Input mode, releasing the line (pull-up brings it back HIGH)
3. Wait ~70µs then sample DQ, this falls inside the DS18B20's response window: it waits 15-60µs (`tPDHIGH`) after sensing the release, then pulls the line low itself for 60-240µs (`tPDLOW`) as its presence pulse. Reading LOW here means a device responded
4. Wait out the remainder of **480µs total since release** (`tRSTH`, confirmed from datasheet Figure 15 that this window already contains the presence pulse, it isn't additional time on top)
5. Return whether presence was detected

**`ow_write_bit(bit)`:** every slot ≥60µs (`tSLOT`), plus ≥1µs recovery (`tREC`) after.
- Write 1: pull low ~5µs (must release within `tLOW1` max of 15µs), then release for the remainder of the slot
- Write 0: pull low and hold for the entire slot (`tLOW0`, 60-120µs)

**`ow_read_bit()`:** the DS18B20's response is only valid for 15µs total, measured from when the master first pulls low (`tRDV`), so the initial low-pulse and the release-to-sample gap both need to be as short as possible.
1. Pull low ~1-2µs to initiate the slot
2. Switch to Input mode (release)
3. Wait a few µs, then sample, total elapsed since step 1 must stay under 15µs
4. HIGH sampled = DS18B20 sent a `1` (it just left the line alone). LOW sampled = DS18B20 sent a `0` (it's actively pulling low itself during this slot)
5. Pad out the remainder of the ≥60µs slot before returning

Single sample is sufficient, unlike UART's oversampling: UART is asynchronous (transmitter/receiver clocks are independent, so the receiver has to guess where a bit boundary falls and average out uncertainty). A 1-Wire read slot is master-initiated, the master already knows exactly when the window started because it triggered it, no clock-drift uncertainty to compensate for.

**`ow_write_byte(byte)` / `ow_read_byte()`:** loop 8 times over the bit functions above. All 1-Wire data is transmitted **LSB first** (opposite of how the byte reads left-to-right in binary), so bit 0 goes first, bit 7 last.

Extracting bit `i` from a byte to send:
```c
uint8_t bit = (byte >> i) & 0b1U;   // shift bit i down to position 0, mask off the rest
```
Assembling a byte from bits read back, in the same LSB-first order:
```c
byte |= bit << i;   // shift the just-read bit up to position i, OR it in without disturbing bits already placed
```

---

## DS18B20 (device-specific layer, built on OneWire)

**Command bytes** (from the datasheet's ROM Command and Function Command tables):
```c
#define DS18B20_SKIP_ROM        0xCC // address whatever's on the bus, no 64-bit ROM code needed (single sensor)
#define DS18B20_CONVERT_T       0x44 // start a temperature conversion
#define DS18B20_READ_SCRATCHPAD 0xBE // read back the 9-byte scratchpad (temp data + config + CRC)
```

**Every DS18B20 operation starts with the same sequence** (reset, check presence, Skip ROM), factored into a shared `static` helper so it isn't duplicated across every function:
```c
static uint8_t ds18b20_begin(...) {
    if (!ow_reset(...)) return 0;
    ow_write_byte(..., DS18B20_SKIP_ROM);
    return 1;
}
```
`static` here means private to this file, not declared in `ds18b20.h`, since nothing outside this file should call it directly.

**`ds18b20_start_conversion()`:** `ds18b20_begin()`, then `ow_write_byte(DS18B20_CONVERT_T)`. Doesn't wait for or read the result, just triggers it, the sensor needs up to 750ms (`tCONV` at default 12-bit resolution) afterward before a read will return valid data.

**`ds18b20_read_temp()`:** `ds18b20_begin()`, then `ow_write_byte(DS18B20_READ_SCRATCHPAD)`, then `ow_read_byte()` twice for the temperature register's LS byte (scratchpad Byte 0) and MS byte (Byte 1).

**Temperature register format:** 16-bit two's complement, LS byte holds bits 2³ down to 2⁻⁴ (4 whole-number bits, 4 fractional bits), MS byte holds 5 sign bits plus bits 2⁶-2⁴. Since the smallest bit is 2⁻⁴ = 1/16, the whole 16-bit value scaled by 1/16 gives the temperature directly:
```c
int16_t temp_byte = (ms_byte << 8) | ls_byte;   // int16_t, not uint16_t, so negative values decode correctly via two's complement
float temp = ((float)temp_byte) / 16.0f;
```
Verified against the datasheet's own worked examples (e.g. `0191h` = 401 decimal, `401/16 = 25.0625°C`, matching the table exactly; `FFF8h` as signed int16 = -8, `-8/16 = -0.5°C`, also matching).
