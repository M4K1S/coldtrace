#ifndef INC_LOG_H_
#define INC_LOG_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "sd.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

typedef struct {
    uint32_t timestamp;  // 4 BYTES
    int16_t  temp_x100;  // 2 BYTES - temperature * 100, stored as a signed int (avoids storing a raw float)
    uint8_t  door_state; // 1 BYTE
    uint8_t  reserved;   // 1 BYTE  - padding, keeps the struct a clean 8 bytes
} LogRecord;

#define LOG_METADATA_BLOCK    0U    // reserved block, stores the "next write block" pointer
#define LOG_FIRST_DATA_BLOCK  1U    // actual log records start here
#define LOG_RECORDS_PER_BLOCK (SD_BLOCK_SIZE / sizeof(LogRecord)) // 512/8 = 64
#define LOG_INVALID_MARKER    0xFFFFFFFFU // a blank/erased SD card reads as all 1s

uint8_t log_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin);
uint8_t log_append_reading(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, float temp, uint32_t timestamp, uint8_t door_state);
uint8_t log_save_position(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t next_block);
uint8_t log_flush(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin);

#endif /* INC_LOG_H_ */

