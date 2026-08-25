#ifndef INC_DS3231_H_
#define INC_DS3231_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "i2c.h"
#include "tim.h"

#define DS3231_ADDR 0x68
#define DS3231_REG_STATUS 0x0F
#define DS3231_OSF_BIT (1U << 7) // Oscillator Stop Flag - set if power/battery was lost

typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;    // day of week, 1-7
    uint8_t date;   // day of month, 1-31
    uint8_t month;  // 1-12
    uint8_t year;   // 0-99
} RTC_Time;

uint8_t bcd_to_dec(uint8_t bcd);
uint8_t dec_to_bcd(uint8_t dec);

uint8_t rtc_set_time(I2C_TypeDef *port, TIM_TypeDef *tim_port, RTC_Time *time);
uint8_t rtc_get_time(I2C_TypeDef *port, TIM_TypeDef *tim_port, RTC_Time *time);
uint8_t rtc_time_is_valid(I2C_TypeDef *port, TIM_TypeDef *tim_port);

#endif
