#include "log.h"

static uint32_t next_write_block = LOG_FIRST_DATA_BLOCK;
static LogRecord record_buffer[LOG_RECORDS_PER_BLOCK];
static uint8_t buffer_index = 0;

uint8_t log_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin) {
    uint8_t block[SD_BLOCK_SIZE];
    uint32_t saved_block;

    if (!sd_read_block(spi_port, tim_port, cs_port, cs_pin, LOG_METADATA_BLOCK, block)) {
        return 0; // couldn't read the metadata block
    }

    // Store first 4 bytes into next_block
    saved_block = ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) | ((uint32_t)block[2] << 8) | ((uint32_t)block[3] << 0);

    if (saved_block == LOG_INVALID_MARKER) {
        saved_block = LOG_FIRST_DATA_BLOCK; // fresh/blank card
    }

    next_write_block = saved_block;

    return 1;
}

uint8_t log_append_reading(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, float temp, uint32_t timestamp, uint8_t door_state) {
    // If a previous flush failed and left the buffer full, retry it now before accepting a new reading
    if (buffer_index >= LOG_RECORDS_PER_BLOCK) {
        if (!sd_write_block(spi_port, tim_port, cs_port, cs_pin, next_write_block, (uint8_t *)record_buffer)) {
            return 0; // still failing, don't accept new data, buffer stays full and safe
        }
        next_write_block++;
        buffer_index = 0;
        if (!log_save_position(spi_port, tim_port, cs_port, cs_pin, next_write_block)) {
            return 0;
        }
    }

    // Pack the inputs into a record
    LogRecord record;
    record.timestamp = timestamp;
    record.temp_x100 = (int16_t)(temp * 100.0f + (temp >= 0.0f ? 0.5f : -0.5f));
    record.door_state = door_state;
    record.reserved = 0;

    record_buffer[buffer_index] = record;
    buffer_index++;

    // If the buffer just became full, flush it immediately
    if (buffer_index >= LOG_RECORDS_PER_BLOCK) {
        if (!sd_write_block(spi_port, tim_port, cs_port, cs_pin, next_write_block, (uint8_t *)record_buffer)) {
            return 0; // this reading is safely stored in RAM at index 63, will retry flush next call
        }
        next_write_block++;
        buffer_index = 0;
        if (!log_save_position(spi_port, tim_port, cs_port, cs_pin, next_write_block)) {
            return 0;
        }
    }

    return 1;
}

uint8_t log_save_position(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t next_block) {
    // Build a 512 byte buffer with the first 4 bytes holding next_block
    uint8_t block[SD_BLOCK_SIZE];
    block[0] = (next_block >> 24) & 0xFF;
    block[1] = (next_block >> 16) & 0xFF;
    block[2] = (next_block >> 8)  & 0xFF;
    block[3] = (next_block >> 0)  & 0xFF;

    return sd_write_block(spi_port, tim_port, cs_port, cs_pin, LOG_METADATA_BLOCK, block);
}

uint8_t log_flush(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin) {
    if (buffer_index == 0) {
        return 1; // nothing buffered, nothing to do
    }

    // Write the current (possibly partial) buffer as a safety snapshot
    return sd_write_block(spi_port, tim_port, cs_port, cs_pin, next_write_block, (uint8_t *)record_buffer);
}
