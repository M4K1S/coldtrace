#ifndef INC_SD_H_
#define INC_SD_H_

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

#define SD_RESPONSE_TIMEOUT_TRIES 8U // NCR (delay between end of command to response) 1-8 bytes typically
#define SD_DATA_TIMEOUT_TRIES 500U // data token wait needs more retries than a command response

// CMD0 - GO_IDLE_STATE: reset the card into SPI mode
#define SD_CMD0      0
#define SD_CMD0_ARG  0x00000000U // no parameters needed for a reset
#define SD_CMD0_CRC  0x95U       // fixed CRC7 result for cmd=0, arg=0 (never varies)

// CMD8 - SEND_IF_COND: check voltage support, detect SD 2.0+
#define SD_CMD8      8
#define SD_CMD8_ARG  0x000001AAU // bits [11:8]=voltage range (0001=2.7-3.6V), bits [7:0]=arbitrary check pattern (0xAA) the card must echo back
#define SD_CMD8_CRC  0x87U       // fixed CRC7 result for cmd=8, arg=0x1AA (never varies)

// CMD55 - APP_CMD: flags the next command as application-specific
#define SD_CMD55 55

// CMD41 (sent as ACMD41): begin initialization, HCS (host capacity support) bit set for SDHC/SDXC support
#define SD_CMD41      41
#define SD_ACMD41_ARG 0x40000000U // bit 30 (HCS) = 1, tells the card the host supports high-capacity media

// CMD58 - READ_OCR: read capacity/voltage info, used for SDHC/SDXC detection
#define SD_CMD58 58

#define SD_DUMMY_CRC    0x01U // CRC unchecked after CMD0/CMD8, any byte works (stop bit 0)
#define SD_R1_IDLE_MASK 0x01U

// CMD17 - READ_SINGLE_BLOCK: read one 512-byte block at the given address
#define SD_CMD17 17
#define SD_DATA_START_TOKEN     0xFEU // card sends this before block data begins (read), or host sends it before sending data (write)
#define SD_BLOCK_SIZE           512U

// CMD24 - WRITE_BLOCK: write one 512-byte block at the given address
#define SD_CMD24 24
#define SD_DATA_ACCEPTED_MASK   0x1FU // lower 5 bits of the data response token
#define SD_DATA_ACCEPTED_VALUE  0x05U // 0bxxx00101 = write accepted

uint8_t sd_send_command(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t has_extra, uint8_t *extra_bytes);
uint8_t sd_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t *is_sdhc);
uint8_t sd_read_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, uint8_t *buf);
uint8_t sd_write_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, uint8_t *buf);

#endif /* INC_SD_H_ */
