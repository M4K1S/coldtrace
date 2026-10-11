#ifndef INC_LOG_H_
#define INC_LOG_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "sd.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

// One 16-byte record per hour; SD block 0 holds the count, records start at block 1
typedef struct {
    uint32_t key;          // yymmddhh, e.g. 26100914 = 2026-10-09 14:00 local
    int16_t  min_x100;     // temperatures x100
    int16_t  max_x100;
    int16_t  avg_x100;
    uint16_t door_samples; // readings with the door open
    uint16_t samples;      // readings in the hour
    uint16_t marker;       // HOUR_MARKER: slot is written
} HourRecord;              // 16 bytes, no padding

#define HOUR_META_BLOCK        0U
#define HOUR_FIRST_DATA_BLOCK  1U
#define HOUR_RECORDS_PER_BLOCK (SD_BLOCK_SIZE / sizeof(HourRecord)) // 32
#define HOUR_META_MAGIC        0x48524C59U // "HRLY"
#define HOUR_MARKER            0xA55AU

// Running stats for the hour in progress
typedef struct {
    uint32_t key;
    float    min, max, sum;
    uint32_t n, door_n;
} HourAcc;

uint32_t hour_key(uint8_t yy, uint8_t mm, uint8_t dd, uint8_t hh);
uint8_t  hour_acc_add(HourAcc *acc, uint32_t key, float temp, uint8_t door_open, HourRecord *finished);

uint8_t  hourly_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin);
uint8_t  hourly_append(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, const HourRecord *rec);
uint32_t hourly_count(void);
uint8_t  hourly_read(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t index, HourRecord *out);

#endif /* INC_LOG_H_ */

