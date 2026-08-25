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

I2C uses two shared, open-drain lines (SDA = data, SCL = clock) that any number of devices can sit on. No device can ever actively drive either line HIGH, only pull it LOW; an external pull-up resistor is what brings the line back HIGH whenever nothing is pulling it down. This is the same open-drain concept as GPIO's `OPEN_DRAIN` type, just required here because the bus is shared: if a device could drive HIGH, two devices disagreeing on the line's state would short-circuit each other. Open-drain means the worst case is just one device's LOW winning, never a conflict.

One consequence worth knowing up front, since it explains a register later on: pulling LOW is fast (a transistor actively switching on), but rising back to HIGH is slower (passive charging of the wire's capacitance through the pull-up resistor). SDA and SCL only get read as valid data while SCL is HIGH; a START condition is SDA going HIGH→LOW *while SCL is HIGH* (a deliberate break from the normal rule that SDA only changes while SCL is LOW, which is exactly why it's recognizable as a special signal and not a data bit), and STOP is the reverse (LOW→HIGH while SCL is HIGH).

### Setup (`I2C_Init`, I2C1 example, PB8=SCL/PB9=SDA)

1. Enable the I2C peripheral clock (`RCC_APB1ENR`) and GPIO clock
2. Configure SDA/SCL pins as alternate function, **AF4** (consistent across I2C1/2/3 on this chip)
3. Set `OTYPER` to open-drain and `PUPDR` to pull-up on both pins, for the shared-bus reasons above. `OTYPER` is 1 bit/pin (0=push-pull, 1=open-drain); `PUPDR` is 2 bits/pin like `MODER` (`01`=pull-up)
4. Set `CR2`'s `FREQ` bits (bits 5:0) to the peripheral's own input clock speed in MHz, this is just telling the hardware what clock it's running on, not the bus speed itself
5. Set `CCR`, the actual SCL bus-speed divider, plus the `F/S` bit (bit 15: 0=Standard mode/100kHz, 1=Fast mode/400kHz). See the CCR equation below for how this value is calculated
6. Set `TRISE`, the maximum time (in clock cycles) the peripheral will allow for that passive rise-to-HIGH to complete before it treats the line as having a fault. See the TRISE equation below
7. Set `CR1`'s `PE` bit (Peripheral Enable) **last**, the peripheral locks `CCR`/`TRISE` once enabled, so they must be configured first

**One-time timeout timer setup, added inside `I2C_Init`:** also calls `TIM_Init(tim_port, 16, 65535)` to configure a dedicated timer at a 1MHz tick rate, used by every transaction function below for timeouts (see next section). Same one-time-setup pattern as `ow_init()`.

### Transaction primitives (built on top of `I2C_Init`)

Every function below that waits on the bus or an external device shares an identical timeout structure: reset the timeout timer's `CNT` to 0, start it, poll a status flag while checking `CNT` against `I2C_TIMEOUT_US` (`#define`'d as 5000, i.e. 5ms, applied uniformly rather than tuned per-function), stop the timer, return 1 (success) or 0 (timeout). This is required because a bare `while(!flag);` hangs the whole program forever if a device never ACKs, wiring is bad, or the bus is stuck.

**`I2C_Start()`:** sets `CR1`'s `START` bit (bit 8) to generate a Start condition, polls `SR1`'s `SB` flag (bit 0) which the hardware sets once that condition has actually finished being generated on the bus. Hardware auto-clears `START` once done, no manual clear needed.

**`I2C_Stop()`:** sets `CR1`'s `STOP` bit (bit 9). No flag to wait on since nothing downstream depends on the Stop condition completing before the code moves on; hardware auto-clears the bit once the Stop condition completes.

**`I2C_SendAddress(addr, rw)`:** formats the address byte as `(addr << 1) | rw` (the 7-bit device address shifted up one, with the read/write bit in position 0; per I2C convention `rw=0` means write, `rw=1` means read), writes it to `DR`, then polls `SR1`'s `ADDR` flag (bit 1), which sets once the addressed device has acknowledged.

Clearing `ADDR` requires a specific two-register read sequence, not a direct bit clear: **read `SR1`, then read `SR2`**, discarding both values with `(void)`. This isn't optional or just informational, it's the literal hardware mechanism that clears the flag; skipping either read leaves `ADDR` stuck set and corrupts whatever operation comes next. `SR2` itself holds additional transaction-state bits (`MSL`, `BUSY`, `TRA`) that aren't used here but are part of why the two-step sequence exists, the address-match event bundles "address sent" (SR1) together with "here's the resulting mode" (SR2), and the hardware wants both looked at before it lets the flag clear.

**`I2C_WriteByte(byte)`:** writes to `DR`, polls `SR1`'s `TXE` flag (bit 7, "transmit register empty" = the peripheral has moved the last byte along and is ready for the next one to be loaded).

**`I2C_ReadByte(ack, *byte_out)`:** polls `SR1`'s `RXNE` flag (bit 6, "receive register not empty" = a byte has fully arrived and is sitting in `DR`). The `ACK` bit (`CR1` bit 10) has to be set **before** the wait loop starts, not after, because the hardware auto-generates the ACK/NACK for a byte at the exact moment that byte finishes arriving, based on whatever `ACK` was set to at that instant. Setting it after `RXNE` fires is too late, that byte's ACK/NACK has already gone out. `ACK=1` tells the sender "keep going" (use for every byte except the last one in a multi-byte read); `ACK=0` tells it "stop" (use for the final byte).

**Output-parameter pattern (`I2C_ReadByte`, and `ds18b20_read_temp` updated to match):** the return value is dedicated purely to success/failure (1/0); the actual data comes back through a pointer parameter (`*byte_out`, `*temp_out`) that the function writes into via the caller's address (`&variable`, i.e. `*ptr = value` follows the pointer to write directly into the caller's memory). This avoids the sentinel-value problem, a `uint8_t` byte has no value that's safely "impossible" to use as an error marker (unlike, say, `-999°C` for a temperature, which is outside any real range), since any of the 256 possible byte values could be genuinely valid data.

### Higher-level (built on the primitives above)

Every function below bails out to `I2C_Stop()` + `return 0` the moment any primitive call fails, rather than pressing on regardless, so a mid-transaction failure never gets reported as a success and never leaves the bus in an unknown state.

**`I2C_WriteReg(dev_addr, reg_addr, value)`:** `Start → SendAddress(WRITE) → WriteByte(reg_addr) → WriteByte(value) → Stop`. The two data bytes aren't special to the I2C protocol itself, electrically they're just two ordinary bytes sent one after another; their meaning ("set register pointer" then "store this value there") comes entirely from how the receiving device's own internal logic is designed to interpret the byte immediately following its address, a convention most I2C peripheral chips follow, documented per-device in that device's own datasheet.

**`I2C_ReadReg(dev_addr, reg_addr, *value_out)`:** needs a **repeated Start**, not a straight sequence, because reading requires two distinct phases: first tell the device which register you want (write phase: `Start → SendAddress(WRITE) → WriteByte(reg_addr)`), then actually receive it (read phase: `Start` again (repeated, no `Stop` in between) `→ SendAddress(READ) → ReadByte(NACK)`, NACK since it's the only/last byte). The repeated Start (rather than a full Stop + fresh Start) matters because a full Stop releases the bus, letting another device potentially interleave a transaction in the gap, a repeated Start keeps the whole write-then-read exchange atomic and uninterrupted. Electrically, a (repeated) Start is also the only defined mechanism for signaling "direction may now switch", there's no other way to tell a device "stop listening, start talking" without it, since something has to hand control of SDA from the master (driving it during write) to the device (driving it during read).

**`I2C_WriteBytes(dev_addr, reg_addr, *data, len)`:** same shape as `WriteReg` but loops `WriteByte(data[i])` for `len` bytes after the register address, no repeated Start needed since the whole operation stays in write mode throughout.

**`I2C_ReadBytes(dev_addr, reg_addr, *data, len)`:** same repeated-start shape as `ReadReg`, but loops `len` times, ACKing every byte except the last (which gets NACK'd). Needed over calling `ReadReg` `len` times in a loop for two reasons: efficiency (one `Start`/`Stop` cycle instead of `len` of them), and atomicity, for something like the DS3231's 7 time registers, `len` separate reads risk a value changing (e.g. seconds rolling over) between calls, giving a self-inconsistent time snapshot; one held-open burst read guarantees all 7 bytes reflect the same instant.

**Caller must guarantee `data` points to a buffer of at least `len` bytes.** `uint8_t *data` carries no size information at runtime, C's type system can't and won't check this, a pointer to a single byte and a pointer to the first element of a 100-byte array look identical to the compiler. Passing a `len` larger than the actual buffer writes past its end (undefined behavior, a real and classic class of C bug), it's a contract enforced only by the caller matching the buffer's real allocated size to what's passed as `len`.

---

## DS3231 (RTC, device-specific layer built on I2C)

Address `0x68`, confirmed directly from the datasheet ("the 7-bit DS3231 address, which is 1101000").

**Why register auto-increment isn't an I2C feature, it's chip-specific:** I2C itself has no concept of "registers", it just moves raw bytes. The DS3231's own internal logic is what interprets the first byte after its address as "set my internal register pointer here", and increments that pointer itself after every subsequent byte transferred (confirmed in the datasheet: "This sets the register pointer... The register pointer increments after each data byte is transferred"). This is a convention this specific chip was designed to follow, not something the I2C protocol or your STM32 peripheral guarantees, different I2C devices can and do behave differently, always check the specific device's datasheet.

**Time/date registers (`0x00`-`0x06`) are stored in BCD**, not plain binary, confirmed directly in the datasheet. BCD packs each individual decimal digit into its own 4-bit nibble (upper nibble = tens digit, lower nibble = ones digit) rather than converting the whole number into one combined binary value, e.g. decimal 47 as BCD is `0100 0111` (`0x47`), completely different bits from plain-binary 47 (`00101111`). Historically chosen because it let old RTC/calculator hardware drive 7-segment displays directly from the raw register bits with zero binary-to-decimal conversion circuitry needed, one nibble maps straight to one digit. Largely legacy baggage on a modern microcontroller-based project like this one, doesn't buy anything a plain binary register wouldn't, it's just what this chip's convention already is.

**`bcd_to_dec(bcd)` / `dec_to_bcd(dec)`:** the translation layer between "how the chip stores a value" (BCD) and "how the rest of the code wants to use it" (plain decimal), same shift-and-mask technique used throughout this whole project:
```c
uint8_t tens = bcd >> 4;        // upper nibble
uint8_t ones = bcd & 0b1111U;   // lower nibble, masked
uint8_t dec = tens * 10 + ones;
```
and the reverse:
```c
uint8_t tens = dec / 10;   // integer division
uint8_t ones = dec % 10;   // remainder
uint8_t bcd = (tens << 4) | ones;
```

**Hours register (`0x02`) is the one exception to pure BCD**, per the datasheet's register map: bit 7 is unused (always 0), bit 6 is a 12/24-hour mode select, and bit 5 means different things depending on that mode (AM/PM in 12-hour mode, or the "20-hour" flag in 24-hour mode). This project always uses 24-hour mode, so bit 5 is genuinely part of the encoded hour value (needed to represent hours 20-23, since the BCD tens digit alone can't exceed what 2 bits allow) and must be kept, only bits 6 and 7 get masked off before running the byte through `bcd_to_dec()`:
```c
time->hours = bcd_to_dec(buffer[2] & 0b00111111U);
```
No equivalent masking is needed on the write side (`dec_to_bcd()` on any hour 0-23 never sets bit 6 or 7 in the first place, so 24-hour mode is preserved automatically).

**`rtc_get_time(*time)`:** one `I2C_ReadBytes()` burst call starting at register `0x00` for 7 bytes (relying on the chip's auto-increment to walk seconds through year in one atomic transaction), then each byte through `bcd_to_dec()` (masked for hours) into the matching `RTC_Time` struct field via the pointer (`time->field = ...`, arrow operator since `time` is a pointer to the struct, not the struct itself).

**`rtc_set_time(*time)`:** the mirror operation, each struct field through `dec_to_bcd()` into a local `buffer[7]`, then one `I2C_WriteBytes()` burst call to push all 7 at once. Per the datasheet, writing all 7 registers within the same burst (rather than 7 separate writes) matters because "the countdown chain is reset whenever the seconds register is written... the remaining time and date registers must be written within 1 second" to avoid rollover inconsistencies, a single held-open transaction guarantees this automatically.

**Why a burst read/write instead of 7 separate `ReadReg`/`WriteReg` calls:** efficiency (one Start/Stop cycle instead of seven), and atomicity, separate reads risk a value changing (e.g. seconds rolling over) between calls, giving a self-inconsistent time snapshot. The datasheet confirms the chip is explicitly designed around this: reads are synchronized to a secondary buffer on every I2C START specifically so a multi-byte burst read is guaranteed self-consistent even if the internal clock ticks over mid-read.

### Equations

**CCR (clock divider), derivation:**

The manual defines, for Standard mode: `Thigh = CCR × TPCLK1`, `Tlow = CCR × TPCLK1`, where `TPCLK1` is the *period* of the peripheral clock (`TPCLK1 = 1 / fPCLK1`), not the frequency. Total SCL period = Thigh + Tlow = `2 × CCR × TPCLK1`, and SCL frequency is the reciprocal of that period: `fSCL = 1 / (2 × CCR × TPCLK1)`. Solving for CCR and substituting `TPCLK1 = 1/fPCLK1`:
```
CCR = fPCLK1 / (2 × fSCL)      [Standard mode]
```
Fast mode (DUTY=0) uses `Tlow = 2 × CCR × TPCLK1` instead of `1×`, making the total period `3 × CCR × TPCLK1`:
```
CCR = fPCLK1 / (3 × fSCL)      [Fast mode, DUTY=0]
```
Worked example (fPCLK1 = 16MHz, Standard mode, fSCL = 100kHz): `CCR = 16,000,000 / (2 × 100,000) = 80`.

**TRISE:**

Derived from the I2C bus spec's maximum allowed SCL rise time: 1000ns for Standard mode, 300ns for Fast mode (Fast mode's shorter clock period leaves less timing margin, so the allowed rise time is tighter):
```
TRISE = (fPCLK1 × maxRiseTime) + 1
```
Standard mode (1000ns) simplifies to just FREQ in MHz + 1.

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
