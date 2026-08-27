#include "sd.h"

uint8_t sd_send_command(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t has_extra, uint8_t *extra_bytes) {
    uint8_t dummy;
    uint8_t response = 0xFF; // Will hold card's R1 response - no real response can ever equal this because bit 7 must be 0

    GPIO_Set(cs_port, cs_pin, 0); // select the card

    // Send the 6-byte command frame
    SPI_TransferByte(spi_port, tim_port, 0x40 | cmd, &dummy);          // byte 0: start/transmission bits (01) + cmd number
    SPI_TransferByte(spi_port, tim_port, (arg >> 24) & 0xFF, &dummy);  // 32-bit argument, MSB first
    SPI_TransferByte(spi_port, tim_port, (arg >> 16) & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (arg >> 8)  & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (arg >> 0)  & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, crc, &dummy);                 // CRC + stop bit

    // Poll for the R1 response - keep clocking 0xFF until the top bit clears, or give up
    for (uint8_t i = 0; i < SD_RESPONSE_TIMEOUT_TRIES; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &response);
        if ((response & 0x80) == 0) { // top bit clear - valid response byte
            break;
        }
    }

    // R3/R7 responses have 4 extra trailing bytes - read them before releasing CS
    if (has_extra) {
        for (uint8_t i = 0; i < 4; i++) {
            SPI_TransferByte(spi_port, tim_port, 0xFF, &extra_bytes[i]);
        }
    }

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    return response; // R1 byte - 0xFF still set means we never got a real response
}

uint8_t sd_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t *is_sdhc) {
    uint8_t dummy;
    uint8_t response;
    uint8_t extra[4];

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    // Send 10 dummy bytes to get 74 clock pulses (74/8 = 9.25, round up to 10)
    for (uint8_t i = 0; i < 10; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &dummy);
    }

    // Send CMD0 - expect idle state (0x01)
    response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD0, SD_CMD0_ARG, SD_CMD0_CRC, 0, NULL);
    if (response != 0x01) return 0; // card didn't reset correctly

    // Send CMD8 - check voltage/version, expect idle state + echoed check pattern
    response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD8, SD_CMD8_ARG, SD_CMD8_CRC, 1, extra);
    if (response != 0x01) return 0; // card doesn't support CMD8
    if (extra[2] != 0x01 || extra[3] != 0xAA) return 0; // echo mismatch, something's wrong

    // Loop CMD55 + ACMD41 until the card reports it's ready (0x00), or give up
    uint8_t ready = 0;
    for (uint16_t i = 0; i < 1000; i++) {
        response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD55, 0, SD_DUMMY_CRC, 0, NULL);
        if (response > 0x01) return 0; // CMD55 itself failed unexpectedly

        response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD41, SD_ACMD41_ARG, SD_DUMMY_CRC, 0, NULL);
        if (response == 0x00) {
            ready = 1;
            break;
        }
    }
    if (!ready) return 0; // card never finished initializing

    // Send CMD58 - read OCR, check CCS bit (bit 30, i.e. bit 6 of the first OCR byte) for SDHC/SDXC
    response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD58, 0, SD_DUMMY_CRC, 1, extra);
    if (response != 0x00) return 0;

    *is_sdhc = (extra[0] & 0x40) ? 1 : 0; // bit 6 of OCR byte 0 = CCS

    return 1; // success
}

uint8_t sd_read_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, uint8_t *buf) {
    uint8_t dummy;
    uint8_t response;

    GPIO_Set(cs_port, cs_pin, 0); // select the card

    // Send CMD17's 6-byte frame directly (CS must remain LOW)
    SPI_TransferByte(spi_port, tim_port, 0x40 | SD_CMD17, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 24) & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 16) & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 8)  & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 0)  & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, SD_DUMMY_CRC, &dummy);

    // Poll for the R1 response
    response = 0xFF;
    for (uint8_t i = 0; i < SD_RESPONSE_TIMEOUT_TRIES; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &response);
        if ((response & 0x80) == 0) break;
    }
    if (response != 0x00) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // card rejected the read command
    }

    // Poll for the data start token - needs many more tries, card is physically reading flash
    uint8_t got_token = 0;
    for (uint16_t i = 0; i < SD_DATA_TIMEOUT_TRIES; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &response);
        if (response == SD_DATA_START_TOKEN) {
            got_token = 1;
            break;
        }
    }
    if (!got_token) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // data never arrived
    }

    // Read the 512 data bytes into the caller's buffer
    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &buf[i]);
    }

    // Read and discard the 2 trailing CRC bytes
    SPI_TransferByte(spi_port, tim_port, 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, 0xFF, &dummy);

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    return 1; // success
}

uint8_t sd_write_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, uint8_t *buf) {
    uint8_t dummy;
    uint8_t response;

    GPIO_Set(cs_port, cs_pin, 0); // select the card

    SPI_TransferByte(spi_port, tim_port, 0x40 | SD_CMD24, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 24) & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 16) & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 8)  & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, (addr >> 0)  & 0xFF, &dummy);
    SPI_TransferByte(spi_port, tim_port, SD_DUMMY_CRC, &dummy);

    response = 0xFF;
    for (uint8_t i = 0; i < SD_RESPONSE_TIMEOUT_TRIES; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &response);
        if ((response & 0x80) == 0) break;
    }
    if (response != 0x00) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // card rejected the write command
    }

    SPI_TransferByte(spi_port, tim_port, SD_DATA_START_TOKEN, &dummy);

    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++) {
        SPI_TransferByte(spi_port, tim_port, buf[i], &dummy);
    }

    // Send 2 dummy CRC bytes
    SPI_TransferByte(spi_port, tim_port, SD_DUMMY_CRC, &dummy);
    SPI_TransferByte(spi_port, tim_port, SD_DUMMY_CRC, &dummy);

    // Poll for data response token
    uint8_t got_token = 0;
    for (uint16_t i = 0; i < SD_DATA_TIMEOUT_TRIES; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &response);
        if ((response & SD_DATA_ACCEPTED_MASK) == SD_DATA_ACCEPTED_VALUE) {
            got_token = 1;
            break;
        }
    }
    if (!got_token) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // card rejected the data
    }

    // Wait for the card to finish its internal write - it holds the line busy (0x00) until done
    uint8_t busy_response = 0x00;
    uint8_t finished = 0;
    for (uint16_t i = 0; i < SD_DATA_TIMEOUT_TRIES; i++) {
        SPI_TransferByte(spi_port, tim_port, 0xFF, &busy_response);
        if (busy_response == 0xFF) {
            finished = 1;
            break; // no longer busy
        }
    }

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    if (!finished) {
        return 0; // card never finished writing
    }

    return 1; // success
}

