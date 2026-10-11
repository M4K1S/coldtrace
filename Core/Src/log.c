#include "log.h"
#include <string.h>

static uint32_t hour_count = 0;

static int16_t to_x100(float v) {
    return (int16_t)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

uint32_t hour_key(uint8_t yy, uint8_t mm, uint8_t dd, uint8_t hh) {
    return (uint32_t)yy * 1000000UL + (uint32_t)mm * 10000UL + (uint32_t)dd * 100UL + hh;
}

// Adds a reading; returns 1 when the previous hour ended
uint8_t hour_acc_add(HourAcc *acc, uint32_t key, float temp, uint8_t door_open, HourRecord *finished) {
    uint8_t produced = 0;

    if (acc->n > 0 && key != acc->key) {
        if (key > acc->key) {
            finished->key = acc->key;
            finished->min_x100 = to_x100(acc->min);
            finished->max_x100 = to_x100(acc->max);
            finished->avg_x100 = to_x100(acc->sum / (float)acc->n);
            finished->door_samples = acc->door_n > 0xFFFFU ? 0xFFFFU : (uint16_t)acc->door_n;
            finished->samples = acc->n > 0xFFFFU ? 0xFFFFU : (uint16_t)acc->n;
            finished->marker = HOUR_MARKER;
            produced = 1;
        }
        acc->n = 0;
    }

    if (acc->n == 0) {
        acc->key = key;
        acc->min = temp;
        acc->max = temp;
        acc->sum = 0.0f;
        acc->door_n = 0;
    }

    if (temp < acc->min) acc->min = temp;
    if (temp > acc->max) acc->max = temp;
    acc->sum += temp;
    acc->n++;
    if (door_open) acc->door_n++;

    return produced;
}

static uint8_t hour_save_meta(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t count) {
    uint8_t block[SD_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    block[0] = (HOUR_META_MAGIC >> 24) & 0xFF;
    block[1] = (HOUR_META_MAGIC >> 16) & 0xFF;
    block[2] = (HOUR_META_MAGIC >> 8)  & 0xFF;
    block[3] = (HOUR_META_MAGIC >> 0)  & 0xFF;
    block[4] = (count >> 24) & 0xFF;
    block[5] = (count >> 16) & 0xFF;
    block[6] = (count >> 8)  & 0xFF;
    block[7] = (count >> 0)  & 0xFF;
    return sd_write_block(spi_port, tim_port, cs_port, cs_pin, HOUR_META_BLOCK, block);
}

uint8_t hourly_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin) {
    uint8_t block[SD_BLOCK_SIZE];

    if (!sd_read_block(spi_port, tim_port, cs_port, cs_pin, HOUR_META_BLOCK, block)) {
        return 0;
    }

    uint32_t magic = ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) | ((uint32_t)block[2] << 8) | block[3];
    if (magic != HOUR_META_MAGIC) {
        hour_count = 0; // never used for hourly history on this card
        return 1;
    }

    hour_count = ((uint32_t)block[4] << 24) | ((uint32_t)block[5] << 16) | ((uint32_t)block[6] << 8) | block[7];
    return 1;
}

uint32_t hourly_count(void) {
    return hour_count;
}

uint8_t hourly_append(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, const HourRecord *rec) {
    uint8_t block[SD_BLOCK_SIZE];
    uint32_t block_addr = HOUR_FIRST_DATA_BLOCK + hour_count / HOUR_RECORDS_PER_BLOCK;
    uint32_t slot = hour_count % HOUR_RECORDS_PER_BLOCK;

    if (slot == 0) {
        memset(block, 0, sizeof(block)); // first record of a fresh block
    } else if (!sd_read_block(spi_port, tim_port, cs_port, cs_pin, block_addr, block)) {
        return 0;
    }

    HourRecord r = *rec;
    r.marker = HOUR_MARKER;
    memcpy(block + slot * sizeof(HourRecord), &r, sizeof(HourRecord));

    // data first, then the count, so a power cut just rewrites the slot
    if (!sd_write_block(spi_port, tim_port, cs_port, cs_pin, block_addr, block)) {
        return 0;
    }
    if (!hour_save_meta(spi_port, tim_port, cs_port, cs_pin, hour_count + 1)) {
        return 0;
    }

    hour_count++;
    return 1;
}

uint8_t hourly_read(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t index, HourRecord *out) {
    uint8_t block[SD_BLOCK_SIZE];
    if (index >= hour_count) return 0;

    uint32_t block_addr = HOUR_FIRST_DATA_BLOCK + index / HOUR_RECORDS_PER_BLOCK;
    if (!sd_read_block(spi_port, tim_port, cs_port, cs_pin, block_addr, block)) {
        return 0;
    }

    memcpy(out, block + (index % HOUR_RECORDS_PER_BLOCK) * sizeof(HourRecord), sizeof(HourRecord));
    return out->marker == HOUR_MARKER;
}
