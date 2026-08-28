#include "app.h"
#include <stdint.h>
#include "uart.h"
#include "ds18b20.h"
#include "ds3231.h"
#include "spi.h"
#include "sd.h"
#include "log.h"

// DS18B20 data line + the timer dedicated to OneWire's microsecond delays
#define TEMP_PORT GPIOA
#define TEMP_PIN  10
#define TEMP_TIM  TIM2

// DS3231 is on I2C1 (PB8=SCL, PB9=SDA)
#define RTC_I2C   I2C1
#define RTC_TIM   TIM3

// Dedicated timer for app-level loop pacing
#define DELAY_TIM TIM4

// SD card over SPI1 (PA5=SCK, PA6=MISO, PA7=MOSI), CS is a plain GPIO
#define SD_SPI     SPI1
#define SD_TIM     TIM5
#define SD_CS_PORT GPIOA
#define SD_CS_PIN  4

// Set once in App_Init if the SD card + log layer both came up cleanly;
// App_Loop checks this before every log_append_reading() call
static uint8_t sd_logging_ready = 0;

// Prints a signed float with 2 decimal places, no libc float-printf dependency
static void UART_SendTemp(USART_TypeDef *port, float tempC) {
    int32_t centi = (int32_t)(tempC * 100.0f + (tempC >= 0.0f ? 0.5f : -0.5f));
    if (centi < 0) {
        UART_SendChar(port, '-');
        centi = -centi;
    }

    int32_t whole = centi / 100;
    int32_t frac = centi % 100;

    char digits[4];
    uint8_t n = 0;
    if (whole == 0) {
        digits[n++] = '0';
    } else {
        while (whole > 0) {
            digits[n++] = '0' + (whole % 10);
            whole /= 10;
        }
    }
    while (n > 0) {
        UART_SendChar(port, digits[--n]);
    }

    UART_SendChar(port, '.');
    UART_SendChar(port, '0' + (frac / 10));
    UART_SendChar(port, '0' + (frac % 10));
}

static void UART_SendTwoDigit(USART_TypeDef *port, uint8_t val) {
    UART_SendChar(port, '0' + (val / 10) % 10);
    UART_SendChar(port, '0' + (val % 10));
}

static uint8_t parse_two_digits(const char *s) {
    return (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
}

// Blocks on UART RX until the user types a valid 13-digit time string and
// presses Enter, then writes it to the DS3231. Format: YYMMDDHHMMSSD
// (D = day of week, 1-7). Reprompts on a bad-length line.
static void set_time_from_uart(void) {
    UART_SendString(USART2, "RTC time not set. Enter time as YYMMDDHHMMSSD (D=day 1-7), then Enter:\r\n");

    char buf[13];
    uint8_t idx = 0;

    while (1) {
        char c = UART_ReceiveChar(USART2); // blocks until a char arrives
        if (c == '\r' || c == '\n') {
            if (idx == 13) break;
            UART_SendString(USART2, "\r\nExpected 13 digits, try again:\r\n");
            idx = 0;
            continue;
        }
        if (c >= '0' && c <= '9' && idx < 13) {
            buf[idx++] = c;
            UART_SendChar(USART2, c); // echo
        }
    }
    UART_SendString(USART2, "\r\n");

    RTC_Time time;
    time.year    = parse_two_digits(&buf[0]);
    time.month   = parse_two_digits(&buf[2]);
    time.date    = parse_two_digits(&buf[4]);
    time.hours   = parse_two_digits(&buf[6]);
    time.minutes = parse_two_digits(&buf[8]);
    time.seconds = parse_two_digits(&buf[10]);
    time.day     = (uint8_t)(buf[12] - '0');

    if (rtc_set_time(RTC_I2C, RTC_TIM, &time)) {
        UART_SendString(USART2, "RTC time set.\r\n");
    } else {
        UART_SendString(USART2, "RTC set failed (I2C timeout) - check DS3231 wiring.\r\n");
    }
}

void App_Init(void)
{
    UART_Init(USART2, 115200);
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
    I2C_Init(RTC_I2C, SPEED_STANDARD, RTC_TIM);
    TIM_Init(DELAY_TIM, 16, 65535); // 1MHz tick for TIM_Delay_us/ms

    if (!rtc_time_is_valid(RTC_I2C, RTC_TIM)) {
        set_time_from_uart();
    }

    // CS starts deselected (HIGH) before anything touches the SD card
    GPIO_Init(SD_CS_PORT, SD_CS_PIN, OUTPUT, PUSH_PULL);
    GPIO_Set(SD_CS_PORT, SD_CS_PIN, 1);

    // SD spec requires <=400kHz during card identification - SPI_PRESCALER_64
    // at 16MHz gives 250kHz
    SPI_Init(SD_SPI, SPI_PRESCALER_64, SD_TIM);

    uint8_t is_sdhc;
    if (!sd_init(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, &is_sdhc)) {
        UART_SendString(USART2, "SD card not found - logging disabled.\r\n");
        return;
    }

    // Card is identified now, safe to run SPI at full speed (~8MHz) for block I/O
    SPI_Init(SD_SPI, SPI_PRESCALER_2, SD_TIM);

    if (!log_init(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN)) {
        UART_SendString(USART2, "SD card found but log_init failed - logging disabled.\r\n");
        return;
    }

    sd_logging_ready = 1;
    UART_SendString(USART2, "SD card ready, logging enabled. is_sdhc=");
    UART_SendChar(USART2, '0' + is_sdhc);
    UART_SendString(USART2, "\r\n");
}

void App_Loop(void)
{
    RTC_Time time;
    uint8_t have_time = rtc_get_time(RTC_I2C, RTC_TIM, &time);

    if (have_time) {
        UART_SendTwoDigit(USART2, time.hours);
        UART_SendChar(USART2, ':');
        UART_SendTwoDigit(USART2, time.minutes);
        UART_SendChar(USART2, ':');
        UART_SendTwoDigit(USART2, time.seconds);
        UART_SendString(USART2, "  ");
    } else {
        UART_SendString(USART2, "RTC read failed  ");
    }

    if (!ds18b20_start_conversion(TEMP_PORT, TEMP_PIN, TEMP_TIM)) {
        UART_SendString(USART2, "DS18B20 not found\r\n");
        TIM_Delay_ms(DELAY_TIM, 1000);
        return;
    }

    TIM_Delay_ms(DELAY_TIM, 750); // tCONV max for 12-bit resolution

    float tempC;
    if (!ds18b20_read_temp(TEMP_PORT, TEMP_PIN, TEMP_TIM, &tempC)) {
        UART_SendString(USART2, "DS18B20 read failed\r\n");
        TIM_Delay_ms(DELAY_TIM, 250);
        return;
    }

    UART_SendString(USART2, "Temp: ");
    UART_SendTemp(USART2, tempC);
    UART_SendString(USART2, " C\r\n");

    // Log this reading to the SD card alongside the UART printout, if the
    // card came up in App_Init() and we actually have a timestamp for it
    if (sd_logging_ready && have_time) {
        // Seconds since midnight - not a real epoch, just enough to order
        // readings within a day. Swap for a proper timestamp if the date
        // fields ever need to leave the RTC struct.
        uint32_t timestamp = (uint32_t)time.hours * 3600U + (uint32_t)time.minutes * 60U + time.seconds;
        uint8_t door_state = 0; // no door sensor wired up yet

        if (!log_append_reading(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, tempC, timestamp, door_state)) {
            UART_SendString(USART2, "SD log write failed\r\n");
        }
    }

    TIM_Delay_ms(DELAY_TIM, 250); // ~1s total loop period including the 750ms conversion wait
}
