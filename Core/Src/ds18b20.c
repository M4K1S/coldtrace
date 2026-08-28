#include "ds18b20.h"

#define DS18B20_SKIP_ROM        0xCC
#define DS18B20_CONVERT_T       0x44
#define DS18B20_READ_SCRATCHPAD 0xBE
#define DS18B20_SCRATCHPAD_LEN  9 // temp LSB/MSB, TH, TL, config, 3 reserved, CRC

// Shared reset, check presence, address device sequence
static uint8_t ds18b20_begin(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    // If no presence detected return 0
    if (!ow_reset(gpio_port, pin, tim_port)) return 0;
    // Send skip ROM
    ow_write_byte(gpio_port, pin, tim_port, DS18B20_SKIP_ROM);
    return 1;
}

// Dallas/Maxim 1-Wire CRC8 (poly x^8+x^5+x^4+1, reflected form 0x8C), LSB first
static uint8_t ow_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t in_byte = data[i];
        for (uint8_t b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ in_byte) & 0x01U;
            crc >>= 1;
            if (mix) crc ^= 0x8CU;
            in_byte >>= 1;
        }
    }
    return crc;
}

uint8_t ds18b20_start_conversion(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    if (!ds18b20_begin(gpio_port, pin, tim_port)) return 0;
    // Send convert T command
    ow_write_byte(gpio_port, pin, tim_port, DS18B20_CONVERT_T);
    // Return 1 as success
    return 1;
}

uint8_t ds18b20_read_temp(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port, float *temp_out) {
    if (!ds18b20_begin(gpio_port, pin, tim_port)) return 0; // Error case
    // Send read scratchpad
    ow_write_byte(gpio_port, pin, tim_port, DS18B20_READ_SCRATCHPAD);

    // Read the full scratchpad so the CRC byte can validate the transfer
    uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN];
    for (uint8_t i = 0; i < DS18B20_SCRATCHPAD_LEN; i++) {
        scratchpad[i] = ow_read_byte(gpio_port, pin, tim_port);
    }

    // Reject the reading if the 1-Wire transfer got corrupted
    if (ow_crc8(scratchpad, DS18B20_SCRATCHPAD_LEN - 1) != scratchpad[DS18B20_SCRATCHPAD_LEN - 1]) {
        return 0;
    }

    // Combine LS/MS bytes into a signed 16 bit value
    int16_t temp_byte = (scratchpad[1] << 8) | scratchpad[0];
    // Divide by 16 to get actual temp in C
    *temp_out = ((float)temp_byte) / 16;
    return 1; // Success
}
