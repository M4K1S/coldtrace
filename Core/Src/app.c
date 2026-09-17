#include "app.h"
#include <stdint.h>
#include "uart.h"
#include "ds18b20.h"
#include "ds3231.h"
#include "spi.h"
#include "sd.h"
#include "log.h"
#include "ssd1306.h"
#include "esp.h"
#include "secrets.h"

// DS18B20 data line + the timer dedicated to OneWire's microsecond delays.
#define TEMP_PORT GPIOC
#define TEMP_PIN  0
#define TEMP_TIM  TIM2

// ESP8266 on UART4 (PC10=TX, PC11=RX), borrows TEMP_TIM (32-bit) for its timeout
#define ESP_UART UART4
#define ESP_TIM  TEMP_TIM
#define ESP_BAUD 115200

// DS3231 is on I2C3 (PA8=SCL, PC9=SDA)
#define RTC_I2C   I2C3
#define RTC_TIM   TIM3

// Dedicated timer for app-level loop pacing
#define DELAY_TIM TIM4

// SD card over SPI1 (PA5=SCK, PA6=MISO, PA7=MOSI), CS is a plain GPIO
#define SD_SPI     SPI1
#define SD_TIM     TIM5
#define SD_CS_PORT GPIOB
#define SD_CS_PIN  12

// SSD1306 on its own I2C1 bus (PB8=SCL, PB9=SDA)
#define OLED_I2C I2C1
#define OLED_TIM RTC_TIM

// Status LEDs - plain GPIO outputs, active-high
#define LED_RED_PORT   GPIOC
#define LED_RED_PIN    5
#define LED_GREEN_PORT GPIOB
#define LED_GREEN_PIN  1

// Reed switch (door sensor) - normally-open, one leg to GND, needs an
// external pull-up to 3.3V on this pin
#define REED_PORT GPIOC
#define REED_PIN  13

// Temperature above which the red alarm LED takes over
static float temp_threshold_c = 28.0f;

// Set once in App_Init if the SD card + log layer both came up cleanly;
// App_Loop checks this before every log_append_reading() call
static uint8_t sd_logging_ready = 0;

// Set once in App_Init if the SSD1306 ACKed its init sequence
static uint8_t oled_ready = 0;

// Set once in App_Init from the ESP8266's AT+CWJAP result
static uint8_t wifi_connected = 0;

// Last successful DS18B20 reading, so the OLED keeps showing a value across
// loop iterations where the sensor doesn't respond
static float last_tempC = 0.0f;
static uint8_t have_temp = 0;

// Timestamp of the last successful SD log write, shown next to the refresh icon
static RTC_Time last_refresh_time;
static uint8_t have_last_refresh = 0;

// Written to the DS3231 whenever it reports no valid time
static const RTC_Time DEFAULT_TIME = {
    .seconds = 0,
    .minutes = 0,
    .hours   = 12,
    .day     = 1,
    .date    = 1,
    .month   = 1,
    .year    = 25
};

static uint8_t read_door_closed(void) {
    return (GPIO_Read(REED_PORT, REED_PIN) == 0);
}

// Red alarm LED takes priority over the green door-status LED
static void update_status_leds(uint8_t door_closed) {
    if (have_temp && last_tempC > temp_threshold_c) {
        GPIO_Set(LED_RED_PORT, LED_RED_PIN, 1);
        GPIO_Set(LED_GREEN_PORT, LED_GREEN_PIN, 0);
    } else {
        GPIO_Set(LED_RED_PORT, LED_RED_PIN, 0);
        GPIO_Set(LED_GREEN_PORT, LED_GREEN_PIN, door_closed ? 1 : 0);
    }
}

// Delays in 25ms steps instead of one long blocking wait, re-sampling the
// reed switch (and refreshing the LEDs) every step.
static uint8_t delay_polling_door(uint32_t total_ms) {
    uint8_t door_closed = read_door_closed();
    for (uint32_t waited = 0; waited < total_ms; waited += 25) {
        update_status_leds(door_closed);
        TIM_Delay_ms(DELAY_TIM, 25);
        door_closed = read_door_closed();
    }
    update_status_leds(door_closed);
    return door_closed;
}

void App_Init(void)
{
    // DELAY_TIM is initialized first
    TIM_Init(DELAY_TIM, 16, 65535); // 1MHz tick for TIM_Delay_us/ms

    // Delay 2 seconds for ESP8266's ROM bootloader diagnostic bytes
    TIM_Delay_ms(DELAY_TIM, 2000);

    // Init ow, I2C, and LED pins
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
    I2C_Init(RTC_I2C, SPEED_STANDARD, RTC_TIM);

    GPIO_Init(LED_RED_PORT, LED_RED_PIN, OUTPUT, PUSH_PULL);
    GPIO_Init(LED_GREEN_PORT, LED_GREEN_PIN, OUTPUT, PUSH_PULL);
    GPIO_Init(REED_PORT, REED_PIN, INPUT, PUSH_PULL); // OTYPER param unused for INPUT mode

    // OLED is on its own I2C1 bus - needs its own I2C_Init call.
    I2C_Init(OLED_I2C, SPEED_STANDARD, OLED_TIM);
    oled_ready = ssd1306_init(OLED_I2C, OLED_TIM);

    if (!rtc_time_is_valid(RTC_I2C, RTC_TIM)) {
        RTC_Time default_time = DEFAULT_TIME;
        rtc_set_time(RTC_I2C, RTC_TIM, &default_time);
    }

    // TIM2 is widened to 32-bit for ESP's timeout response
    UART_Init(ESP_UART, ESP_BAUD);
    TIM_Init(ESP_TIM, 16, 4294967295);

    wifi_connected = esp_connect_wifi(ESP_UART, ESP_TIM, ESP_WIFI_SSID, ESP_WIFI_PASS);
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);

    // CS starts deselected (HIGH) before anything touches the SD card
    GPIO_Init(SD_CS_PORT, SD_CS_PIN, OUTPUT, PUSH_PULL);
    GPIO_Set(SD_CS_PORT, SD_CS_PIN, 1);

    // SD spec requires <=400kHz during card identification - SPI_PRESCALER_64
    // at 16MHz gives 250kHz
    SPI_Init(SD_SPI, SPI_PRESCALER_64, SD_TIM);

    uint8_t is_sdhc;
    if (!sd_init(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, &is_sdhc)) {
        return;
    }

    // Card is identified now, safe to run SPI at full speed
    SPI_Init(SD_SPI, SPI_PRESCALER_2, SD_TIM);

    if (!log_init(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN)) {
        return;
    }

    sd_logging_ready = 1;
}

void App_Loop(void)
{
    RTC_Time time;
    uint8_t have_time = rtc_get_time(RTC_I2C, RTC_TIM, &time);

    // Reed switch reads LOW when the magnet is present
    uint8_t door_closed = read_door_closed();
    update_status_leds(door_closed);

    // Sensor read failures no longer bail out of the whole loop early - the
    // OLED keeps running off the last known reading rather than going stale
    if (ds18b20_start_conversion(TEMP_PORT, TEMP_PIN, TEMP_TIM)) {
        // tCONV max for 12-bit resolution - polled in small steps so the reed switch/LEDs stay responsive
        door_closed = delay_polling_door(750);

        float tempC;
        if (ds18b20_read_temp(TEMP_PORT, TEMP_PIN, TEMP_TIM, &tempC)) {
            last_tempC = tempC;
            have_temp = 1;

            if (have_time) {
                // Seconds since midnight
                uint32_t seconds_since_midnight = (uint32_t)time.hours * 3600U + (uint32_t)time.minutes * 60U + time.seconds;
                // Save as full timestamp
                uint32_t timestamp = ((uint32_t)time.year << 26) | ((uint32_t)time.month << 22) | ((uint32_t)time.date << 17) | seconds_since_midnight;
                uint8_t door_state = door_closed ? 0 : 1; // reed switch: 0 = closed, 1 = open

                if (sd_logging_ready) {
                    if (log_append_reading(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, tempC, timestamp, door_state)) {
                        last_refresh_time = time;
                        have_last_refresh = 1;
                    }
                }

                // Push the same reading to the server over the ESP8266
                if (wifi_connected) {
                    TIM_Init(ESP_TIM, 16, 0xFFFFFFFF);
                    esp_send_reading(ESP_UART, ESP_TIM, ESP_SERVER_HOST, tempC, door_state, timestamp);
                    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
                }
            }
        }
    }

    // Refresh once more with this iteration's final have_temp/last_tempC
    update_status_leds(door_closed);

    if (oled_ready) {
        int32_t temp_deci = (int32_t)(last_tempC * 10.0f + (last_tempC >= 0.0f ? 0.5f : -0.5f));
        oled_draw_status_screen(wifi_connected, sd_logging_ready,
                                 have_last_refresh, last_refresh_time.hours, last_refresh_time.minutes,
                                 have_temp, temp_deci);
        oled_update(OLED_I2C, OLED_TIM);
    }

    // ~1s total loop period including the 750ms conversion wait
    delay_polling_door(250);
}
