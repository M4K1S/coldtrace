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

// DS18B20 data line and its microsecond timer
#define TEMP_PORT GPIOC
#define TEMP_PIN  0
#define TEMP_TIM  TIM2

// ESP8266 on UART4 (PC10 TX, PC11 RX); times out on TEMP_TIM
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

// Reed switch to GND; needs the external pull-up
#define REED_PORT GPIOC
#define REED_PIN  13

// Buzzer - through a transistor/1k resistor, not driven directly
#define BUZZER_PORT GPIOB
#define BUZZER_PIN  4
// Short off, mostly-on
#define BUZZER_ON_MS  400U
#define BUZZER_OFF_MS 200U
#define BUZZER_CYCLE_MS (BUZZER_ON_MS + BUZZER_OFF_MS)

// Door left open: gentler alarm
#define DOOR_ALARM_AFTER_MS   (60U * 1000U)
#define DOOR_BEEP_PERIOD_MS   5000U
#define DOOR_BEEP_MS          150U
#define DOOR_LED_SLOW_MS      1000U // green flash period while the door is open
#define DOOR_LED_FAST_MS      300U  // ...once it has been open too long
#define HEARTBEAT_PERIOD_MS   5000U // short green blip while the door is closed
#define HEARTBEAT_MS          75U
#define MUTED_ALARM_LED_MS    1000U // red flash period while the alarm is muted

// Mute button - one leg to GND, uses the internal pull-up
#define BUTTON_PORT GPIOC
#define BUTTON_PIN  3
#define BUTTON_LONG_PRESS_MS (3U * 1000U)
#define MUTE_SHORT_MS (15U * 60U * 1000U)
#define MUTE_LONG_MS  (12U * 60U * 60U * 1000U)
#define BOOT_MUTE_MS  (15U * 60U * 1000U)

// Minimum time between accepted button edges
#define BUTTON_DEBOUNCE_MS 40U

// Minutes the reading must stay over the threshold before alarming (set in /admin)
static uint32_t alarm_delay_min = 15;
#define ALARM_DELAY_MIN_LIMIT 240U

// Watchdog timeout: ~20 s nominal (13-38 s across LSI tolerance)
#define WATCHDOG_RELOAD 2500U

// Temperature above which the red alarm LED takes over (can be changed in /admin)
static float temp_threshold_c = -15.0f;

// Millisecond clock
static volatile uint32_t ms_ticks = 0;

uint32_t App_Millis(void) {
    return ms_ticks;
}

void App_TickISR(void) {
    ms_ticks++;
}

// Enable watchdog
static void watchdog_init(void) {
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP; // pause it at debugger breakpoints
    IWDG->KR = 0xCCCC;                              // start (also turns the LSI oscillator on)
    IWDG->KR = 0x5555;                              // unlock PR and RLR
    IWDG->PR = 6;                                   // LSI / 256
    IWDG->RLR = WATCHDOG_RELOAD;
    for (uint32_t i = 0; i < 100000U && IWDG->SR; i++) {}
    IWDG->KR = 0xAAAA;
}

static inline void watchdog_feed(void) {
    IWDG->KR = 0xAAAA;
}

// Set once the SD card and its hourly history are usable
static uint8_t sd_logging_ready = 0;
#define SD_RETRY_MS 30000U
static uint32_t last_sd_attempt_ms = 0;

// Set once in App_Init if the SSD1306 ACKed its init sequence
static uint8_t oled_ready = 0;

// Result of the last WiFi join attempt
static uint8_t wifi_connected = 0;

// Failed live posts in a row before the WiFi link is checked
#define WIFI_RECONNECT_AFTER_FAILURES 2

// Live readings go out this often
#define POST_INTERVAL_MS 5000U
static uint32_t last_post_ms = 0;
static uint8_t wifi_fail_count = 0;

// Retry the WiFi join this often while disconnected
#define WIFI_RETRY_MS 60000U
static uint32_t last_wifi_attempt_ms = 0;
static uint8_t tcp_only_failures = 0; // failure bursts while still joined to the AP

// Last good temperature, kept for the OLED when a read fails
static float last_tempC = 0.0f;
static uint8_t have_temp = 0;

// Clock shown next to the refresh icon
static RTC_Time last_refresh_time;
static uint8_t have_last_refresh = 0;

// RTC re-sync hour (at minute 1); also synced on boot
#define TIME_SYNC_HOUR 12
static uint8_t time_synced_at_hour = 255; // 255 = not synced yet

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

// Hours added to the server's UTC time; updated from its X-Tz-Offset header
static int8_t local_utc_offset_hours = -4;

static uint8_t days_in_month(uint8_t month, uint8_t year_2digit) {
    static const uint8_t days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2) {
        uint16_t full_year = 2000U + year_2digit;
        uint8_t leap = (full_year % 4 == 0) && (full_year % 100 != 0 || full_year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

// Shifts a UTC time by local_utc_offset_hours, rolling the date over
static void apply_utc_offset(RTC_Time *t) {
    int16_t hours = (int16_t)t->hours + local_utc_offset_hours;

    if (hours < 0) {
        hours += 24;
        if (t->date > 1) {
            t->date--;
        } else {
            if (t->month > 1) t->month--;
            else { t->month = 12; t->year--; }
            t->date = days_in_month(t->month, t->year);
        }
    } else if (hours >= 24) {
        hours -= 24;
        if (t->date < days_in_month(t->month, t->year)) {
            t->date++;
        } else {
            t->date = 1;
            if (t->month < 12) t->month++;
            else { t->month = 1; t->year++; }
        }
    }

    t->hours = (uint8_t)hours;
}

// Remaining mute time; touched by the main loop only
static uint32_t mute_remaining_ms = 0;

// Set when a press changes the mute, so the OLED redraws at once
static uint8_t mute_changed = 0;

// The ISR only records edges; debounce_button() acts once the line settles
static volatile uint32_t last_edge_ms = 0;
static volatile uint8_t last_edge_pressed = 0;
static volatile uint8_t edge_pending = 0;

static uint8_t debounced_pressed = 0;
static uint32_t held_start_ms = 0;
// Set once a hold has fired the long mute, so its release does nothing
static uint8_t long_press_fired = 0;

void EXTI3_IRQHandler(void) {
    if (!(EXTI->PR & (1U << BUTTON_PIN))) return;
    EXTI->PR |= (1U << BUTTON_PIN); // cleared by writing 1

    last_edge_pressed = (GPIO_Read(BUTTON_PORT, BUTTON_PIN) == 0);
    last_edge_ms = App_Millis();
    edge_pending = 1;
}

// Short press mutes for MUTE_SHORT_MS; a press while muted cancels it
static void debounce_button(void) {
    if (!edge_pending) return;
    if (App_Millis() - last_edge_ms < BUTTON_DEBOUNCE_MS) return; // still settling

    edge_pending = 0;
    uint8_t pressed = last_edge_pressed; // stable now - no new edge for a full window

    if (pressed == debounced_pressed) return; // bounce that already reverted - not a real change
    debounced_pressed = pressed;

    if (pressed) {
        held_start_ms = App_Millis();
        long_press_fired = 0;
        return;
    }

    if (long_press_fired) return; // check_long_press() already handled this one, while held

    if (mute_remaining_ms > 0) {
        mute_remaining_ms = 0;
    } else {
        mute_remaining_ms = MUTE_SHORT_MS;
    }
    mute_changed = 1;
}

// Fires the long mute mid-hold, unless already muted (that press cancels)
static void check_long_press(void) {
    if (!debounced_pressed || long_press_fired || mute_remaining_ms > 0) return;
    if (App_Millis() - held_start_ms < BUTTON_LONG_PRESS_MS) return;

    long_press_fired = 1;
    mute_remaining_ms = MUTE_LONG_MS;
    mute_changed = 1;
}

// Counts the mute down by real elapsed time
static void mute_tick(void) {
    static uint32_t last_ms = 0;
    static uint8_t counting = 0;

    if (mute_remaining_ms == 0) {
        counting = 0;
        return;
    }

    uint32_t now = App_Millis();
    if (counting) {
        uint32_t elapsed = now - last_ms;
        mute_remaining_ms -= elapsed < mute_remaining_ms ? elapsed : mute_remaining_ms;
    }
    counting = mute_remaining_ms > 0;
    last_ms = now;
}

// True once the reading has been over the threshold for alarm_delay_min
static uint8_t temp_alarm_active(void) {
    static uint8_t over = 0;
    static uint32_t over_since_ms = 0;

    if (!have_temp || last_tempC <= temp_threshold_c) {
        over = 0;
        return 0;
    }

    uint32_t now = App_Millis();
    if (!over) {
        over = 1;
        over_since_ms = now;
    }
    uint32_t minutes = alarm_delay_min < 1U ? 1U : (alarm_delay_min > ALARM_DELAY_MIN_LIMIT ? ALARM_DELAY_MIN_LIMIT : alarm_delay_min);
    return now - over_since_ms >= minutes * 60000U;
}

// Red alarm (slow when muted), else amber door-open flash and beep, else green heartbeat
static void update_status_leds(uint8_t door_closed) {
    static uint32_t cycle_start_ms = 0;
    static uint8_t was_alarming = 0;
    static uint32_t door_open_since_ms = 0;
    static uint8_t door_was_open = 0;

    uint32_t door_open_ms = 0;
    if (door_closed) {
        door_was_open = 0;
    } else {
        if (!door_was_open) {
            door_open_since_ms = App_Millis();
            door_was_open = 1;
        }
        door_open_ms = App_Millis() - door_open_since_ms;
    }

    if (temp_alarm_active()) {
        GPIO_Set(LED_GREEN_PORT, LED_GREEN_PIN, 0);

        if (!was_alarming) {
            cycle_start_ms = App_Millis();
            was_alarming = 1;
        }
        // time-based phase, so a late call can't freeze the pattern
        uint32_t elapsed = App_Millis() - cycle_start_ms;
        uint8_t on = (elapsed % BUZZER_CYCLE_MS) < BUZZER_ON_MS;
        uint8_t muted = mute_remaining_ms > 0;
        GPIO_Set(LED_RED_PORT, LED_RED_PIN, muted ? (elapsed % MUTED_ALARM_LED_MS) < MUTED_ALARM_LED_MS / 2 : on);
        GPIO_Set(BUZZER_PORT, BUZZER_PIN, muted ? 0 : on);
    } else {
        was_alarming = 0;
        uint8_t red_on = 0;
        uint8_t green_on = 0;
        if (door_closed) {
            green_on = (App_Millis() % HEARTBEAT_PERIOD_MS) < HEARTBEAT_MS;
        } else {
            uint32_t led_period = door_open_ms >= DOOR_ALARM_AFTER_MS ? DOOR_LED_FAST_MS : DOOR_LED_SLOW_MS;
            red_on = green_on = (door_open_ms % led_period) < led_period / 2; // red + green = amber
        }
        GPIO_Set(LED_RED_PORT, LED_RED_PIN, red_on);
        GPIO_Set(LED_GREEN_PORT, LED_GREEN_PIN, green_on);

        // door open too long: short repeating beep, silenced by mute
        uint8_t door_beep = 0;
        if (door_open_ms >= DOOR_ALARM_AFTER_MS) {
            door_beep = ((door_open_ms - DOOR_ALARM_AFTER_MS) % DOOR_BEEP_PERIOD_MS) < DOOR_BEEP_MS;
        }
        GPIO_Set(BUZZER_PORT, BUZZER_PIN, (door_beep && mute_remaining_ms == 0) ? 1 : 0);
    }
}

// Redraws the OLED from current state
static void refresh_oled(void) {
    if (!oled_ready) return;

    int32_t temp_deci = (int32_t)(last_tempC * 10.0f + (last_tempC >= 0.0f ? 0.5f : -0.5f));
    uint8_t muted = mute_remaining_ms > 0;
    // round up so a fresh 15 min mute shows 15, not 14
    uint32_t mute_min_total = (mute_remaining_ms + 59999U) / 60000U;
    uint8_t mute_hh = (uint8_t)(mute_min_total / 60U);
    uint8_t mute_mm = (uint8_t)(mute_min_total % 60U);
    oled_draw_status_screen(wifi_connected, sd_logging_ready,
                             have_last_refresh, last_refresh_time.hours, last_refresh_time.minutes,
                             have_temp, temp_deci, muted, mute_hh, mute_mm);
    oled_update(OLED_I2C, OLED_TIM);
}

// Called from esp.c's blocking waits so the button, LEDs and OLED stay live
static void esp_idle_tick(void) {
    watchdog_feed();
    debounce_button();
    check_long_press();
    mute_tick();
    update_status_leds(read_door_closed());
    if (mute_changed) {
        mute_changed = 0;
        refresh_oled();
    }
}

// Delays in 25 ms steps, servicing the button, LEDs and door each step
static uint8_t delay_polling_door(uint32_t total_ms) {
    uint8_t door_closed = read_door_closed();
    for (uint32_t waited = 0; waited < total_ms; waited += 25) {
        watchdog_feed();
        debounce_button();
        check_long_press();
        mute_tick();
        update_status_leds(door_closed);
        if (mute_changed) {
            mute_changed = 0;
            refresh_oled();
        }
        TIM_Delay_ms(DELAY_TIM, 25);
        door_closed = read_door_closed();
    }
    update_status_leds(door_closed);
    return door_closed;
}

// Hourly history: finished hours go to the SD, then to the server
#define HOUR_SAVE_QUEUE         4      // finished hours waiting for a working SD
#define HOURLY_UPLOADS_PER_TICK 3      // per live post, so catch-up never stalls the loop
#define BACKLOG_SYNC_RETRY_MS   30000U
#define NO_KEY                  0xFFFFFFFFUL

static HourAcc hour_acc;
static HourRecord save_queue[HOUR_SAVE_QUEUE];
static uint8_t save_head = 0, save_count = 0;

static uint32_t server_last_key = 0;   // newest hour the server has (valid if server_last_known)
static uint8_t server_last_known = 0;
static uint32_t next_upload_idx = 0;   // next SD record to send
static uint8_t upload_ready = 0;       // next_upload_idx is trustworthy
static uint8_t need_backlog_sync = 1;  // (re)ask the server what it has
static uint32_t last_backlog_try_ms = 0;

static void hour_queue_push(const HourRecord *r) {
    if (save_count == HOUR_SAVE_QUEUE) { // SD has been failing for hours - drop the oldest
        save_head = (save_head + 1) % HOUR_SAVE_QUEUE;
        save_count--;
    }
    save_queue[(save_head + save_count) % HOUR_SAVE_QUEUE] = *r;
    save_count++;
}

static void hour_flush_queue(void) {
    while (save_count > 0) {
        if (!hourly_append(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, &save_queue[save_head])) return; // retried next loop
        save_head = (save_head + 1) % HOUR_SAVE_QUEUE;
        save_count--;
    }
}

// Points next_upload_idx at the first SD hour newer than the server's X-Last-Hour
static void compute_upload_index(void) {
    upload_ready = 0;
    if (!sd_logging_ready || !server_last_known) return;

    uint32_t idx = hourly_count();
    while (idx > 0) {
        HourRecord r;
        if (!hourly_read(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, idx - 1, &r)) return; // retry on the next sync
        if (r.key <= server_last_key) break;
        idx--;
    }
    next_upload_idx = idx;
    upload_ready = 1;
}

// Time sync plus the server's newest hour; sets where uploads start
static void try_backlog_sync(void) {
    RTC_Time synced_time;
    uint32_t last = NO_KEY;

    TIM_Init(ESP_TIM, 16, 0xFFFFFFFF);
    uint8_t ok = esp_sync_time(ESP_UART, ESP_TIM, ESP_SERVER_HOST, &synced_time, &local_utc_offset_hours, &temp_threshold_c, &alarm_delay_min, &last);
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);

    if (!ok) return;
    apply_utc_offset(&synced_time);
    rtc_set_time(RTC_I2C, RTC_TIM, &synced_time);

    if (last != NO_KEY) {
        server_last_key = last;
        server_last_known = 1;
        compute_upload_index();
        need_backlog_sync = !upload_ready;
    }
}

// Sends a few saved hours the server is missing, oldest first.
static void upload_hours(void) {
    if (!sd_logging_ready || !upload_ready) return;

    for (uint8_t i = 0; i < HOURLY_UPLOADS_PER_TICK && next_upload_idx < hourly_count(); i++) {
        HourRecord r;
        if (hourly_read(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, next_upload_idx, &r)) {
            TIM_Init(ESP_TIM, 16, 0xFFFFFFFF);
            uint8_t ok = esp_send_hourly(ESP_UART, ESP_TIM, ESP_SERVER_HOST, r.key, r.min_x100, r.max_x100, r.avg_x100, r.door_samples, r.samples);
            ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
            if (!ok) return; // try again next tick
        }
        next_upload_idx++; // skips unreadable slots
    }
}

static void wifi_reconnect(void) {
    TIM_Init(ESP_TIM, 16, 0xFFFFFFFF);
    esp_reset(ESP_UART, ESP_TIM);
    wifi_connected = esp_connect_wifi(ESP_UART, ESP_TIM, ESP_WIFI_SSID, ESP_WIFI_PASS);
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
    wifi_fail_count = 0;
    last_wifi_attempt_ms = App_Millis();
    if (wifi_connected) {
        need_backlog_sync = 1; // an outage may have left hours unsent
        last_backlog_try_ms = 0;
    }
}

// Brings the SD up (5 tries) and sets where uploads start; retried from App_Loop
static void sd_bring_up(void) {
    last_sd_attempt_ms = App_Millis();

    uint8_t is_sdhc = 0;
    uint8_t ok = 0;
    for (uint8_t attempt = 0; attempt < 5 && !ok; attempt++) {
        watchdog_feed();
        if (attempt > 0) TIM_Delay_ms(DELAY_TIM, 200);
        // identification needs <=400 kHz: 250 kHz at /64
        SPI_Init(SD_SPI, SPI_PRESCALER_64, SD_TIM);
        ok = sd_init(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN, &is_sdhc);
    }
    if (!ok) {
        return;
    }

    // full speed once identified
    SPI_Init(SD_SPI, SPI_PRESCALER_2, SD_TIM);

    if (!hourly_init(SD_SPI, SD_TIM, SD_CS_PORT, SD_CS_PIN)) {
        return;
    }

    sd_logging_ready = 1;
    compute_upload_index();
    need_backlog_sync = !upload_ready;
}

void App_Init(void)
{
    // 1 ms tick for App_Millis()
    SysTick_Config(SystemCoreClock / 1000U);

    watchdog_init();

    // keep the UI alive during blocking ESP waits
    esp_set_idle_tick(esp_idle_tick);

    // DELAY_TIM is initialized first
    TIM_Init(DELAY_TIM, 16, 65535); // 1MHz tick for TIM_Delay_us/ms

    // let the ESP's boot chatter finish
    TIM_Delay_ms(DELAY_TIM, 2000);

    // Init ow, I2C, and LED pins
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
    I2C_Init(RTC_I2C, SPEED_STANDARD, RTC_TIM);

    GPIO_Init(LED_RED_PORT, LED_RED_PIN, OUTPUT, PUSH_PULL);
    GPIO_Init(LED_GREEN_PORT, LED_GREEN_PIN, OUTPUT, PUSH_PULL);
    GPIO_Init(BUZZER_PORT, BUZZER_PIN, OUTPUT, PUSH_PULL);
    GPIO_Init(REED_PORT, REED_PIN, INPUT, PUSH_PULL); // OTYPER param unused for INPUT mode
    GPIO_Init(BUTTON_PORT, BUTTON_PIN, INPUT, PUSH_PULL);
    GPIO_SetPull(BUTTON_PORT, BUTTON_PIN, PULL_UP);

    // mute button on EXTI3, both edges
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->EXTICR[0] &= ~(0xFU << 12);           // EXTI3 field, in EXTICR[0] bits [15:12]
    SYSCFG->EXTICR[0] |= (0x2U << 12);            // 0x2 = GPIOC
    EXTI->IMR  |= (1U << BUTTON_PIN);             // unmask line 3
    EXTI->FTSR |= (1U << BUTTON_PIN);             // falling edge (press)
    EXTI->RTSR |= (1U << BUTTON_PIN);             // rising edge (release)
    NVIC_EnableIRQ(EXTI3_IRQn);

    // OLED is on its own I2C1 bus
    I2C_Init(OLED_I2C, SPEED_STANDARD, OLED_TIM);
    oled_ready = ssd1306_init(OLED_I2C, OLED_TIM);

    // TIM2 is widened to 32-bit for ESP's timeout response
    UART_Init(ESP_UART, ESP_BAUD);
    TIM_Init(ESP_TIM, 16, 4294967295);

    wifi_connected = esp_connect_wifi(ESP_UART, ESP_TIM, ESP_WIFI_SSID, ESP_WIFI_PASS);
    last_wifi_attempt_ms = App_Millis();

    // boot-time RTC sync; DEFAULT_TIME only if that fails and the RTC is invalid
    uint8_t time_synced = 0;
    if (wifi_connected) {
        RTC_Time synced_time;
        uint32_t last = NO_KEY;
        if (esp_sync_time(ESP_UART, ESP_TIM, ESP_SERVER_HOST, &synced_time, &local_utc_offset_hours, &temp_threshold_c, &alarm_delay_min, &last)) {
            apply_utc_offset(&synced_time);
            time_synced = rtc_set_time(RTC_I2C, RTC_TIM, &synced_time);
            if (last != NO_KEY) {
                server_last_key = last;
                server_last_known = 1;
            }
        }
    }

    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);

    // boot with the alarm muted while things settle
    mute_remaining_ms = BOOT_MUTE_MS;

    if (!time_synced && !rtc_time_is_valid(RTC_I2C, RTC_TIM)) {
        RTC_Time default_time = DEFAULT_TIME;
        rtc_set_time(RTC_I2C, RTC_TIM, &default_time);
    }

    // CS starts deselected (HIGH) before anything touches the SD card
    GPIO_Init(SD_CS_PORT, SD_CS_PIN, OUTPUT, PUSH_PULL);
    GPIO_Set(SD_CS_PORT, SD_CS_PIN, 1);

    sd_bring_up();
}

void App_Loop(void)
{
    if (!wifi_connected && App_Millis() - last_wifi_attempt_ms >= WIFI_RETRY_MS) {
        wifi_reconnect();
    }

    if (!sd_logging_ready && App_Millis() - last_sd_attempt_ms >= SD_RETRY_MS) {
        sd_bring_up();
    }

    if (wifi_connected && need_backlog_sync && App_Millis() - last_backlog_try_ms >= BACKLOG_SYNC_RETRY_MS) {
        last_backlog_try_ms = App_Millis();
        try_backlog_sync();
    }

    RTC_Time time;
    uint8_t have_time = rtc_get_time(RTC_I2C, RTC_TIM, &time);
    if (have_time) {
        last_refresh_time = time;
        have_last_refresh = 1;
    }

    if (have_time && wifi_connected) {
        // once a day, at 12:01
        if (time.hours == TIME_SYNC_HOUR && time.minutes == 1 && time_synced_at_hour != TIME_SYNC_HOUR) {
            try_backlog_sync();
            time_synced_at_hour = TIME_SYNC_HOUR;
        } else if (!(time.hours == TIME_SYNC_HOUR && time.minutes == 1)) {
            time_synced_at_hour = 255;
        }
    }

    // Reed switch reads LOW when the magnet is present
    uint8_t door_closed = read_door_closed();
    update_status_leds(door_closed);
    if (mute_changed) {
        mute_changed = 0;
        refresh_oled();
    }

    // a failed read keeps the last value on screen
    if (ds18b20_start_conversion(TEMP_PORT, TEMP_PIN, TEMP_TIM)) {
        // 12-bit conversion time, polled in steps
        door_closed = delay_polling_door(750);

        float tempC;
        if (ds18b20_read_temp(TEMP_PORT, TEMP_PIN, TEMP_TIM, &tempC)) {
            last_tempC = tempC;
            have_temp = 1;

            // show the reading before the slow SD/network work
            refresh_oled();

            if (have_time) {
                // Seconds since midnight
                uint32_t seconds_since_midnight = (uint32_t)time.hours * 3600U + (uint32_t)time.minutes * 60U + time.seconds;
                // Save as full timestamp
                uint32_t timestamp = ((uint32_t)time.year << 26) | ((uint32_t)time.month << 22) | ((uint32_t)time.date << 17) | seconds_since_midnight;
                uint8_t door_state = door_closed ? 0 : 1; // reed switch: 0 = closed, 1 = open

                // fold into the current hour; a finished hour is queued, then saved
                HourRecord finished;
                if (hour_acc_add(&hour_acc, hour_key(time.year, time.month, time.date, time.hours), tempC, door_state, &finished)) {
                    hour_queue_push(&finished);
                }
                if (sd_logging_ready) {
                    hour_flush_queue();
                }

                if (wifi_connected && App_Millis() - last_post_ms >= POST_INTERVAL_MS) {
                    last_post_ms = App_Millis();
                    TIM_Init(ESP_TIM, 16, 0xFFFFFFFF);
                    uint8_t posted = esp_send_reading(ESP_UART, ESP_TIM, ESP_SERVER_HOST, tempC, door_state, timestamp);
                    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);

                    if (posted) {
                        wifi_fail_count = 0;
                        upload_hours(); // catch up saved hours the server lacks
                    } else if (++wifi_fail_count >= WIFI_RECONNECT_AFTER_FAILURES) {
                        // posts failing: check the link before resetting anything
                        wifi_fail_count = 0;
                        TIM_Init(ESP_TIM, 16, 0xFFFFFFFF);
                        uint8_t associated = esp_wifi_associated(ESP_UART, ESP_TIM);
                        ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);

                        if (!associated || ++tcp_only_failures >= 3) {
                            tcp_only_failures = 0;
                            wifi_reconnect(); // dropped, or ESP wedged
                        }
                    }
                }
            }
        }
    }

    update_status_leds(door_closed);

    mute_changed = 0;
    refresh_oled();

    // ~1s total loop period including the 750ms conversion wait
    delay_polling_door(250);
}
