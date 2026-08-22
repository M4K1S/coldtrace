#include "ds18b20.h"

#define DS18B20_SKIP_ROM        0xCC
#define DS18B20_CONVERT_T       0x44
#define DS18B20_READ_SCRATCHPAD 0xBE

// Shared reset, check presence, address device sequence
static uint8_t ds18b20_begin(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    // If no presence detected return 0
    if (!ow_reset(gpio_port, pin, tim_port)) return 0;
    // Send skip ROM
    ow_write_byte(gpio_port, pin, tim_port, DS18B20_SKIP_ROM);
    return 1;
}

uint8_t ds18b20_start_conversion(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    if (!ds18b20_begin(gpio_port, pin, tim_port)) return 0;
    // Send convert T command
    ow_write_byte(gpio_port, pin, tim_port, DS18B20_CONVERT_T);
    // Return 1 as success
    return 1;
}

float ds18b20_read_temp(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    if (!ds18b20_begin(gpio_port, pin, tim_port)) return -999; // Error case
    // Send read scratchpad
    ow_write_byte(gpio_port, pin, tim_port, DS18B20_READ_SCRATCHPAD);
    // Read the first byte (LS byte)
    uint8_t ls_byte = ow_read_byte(gpio_port, pin, tim_port);
    // Read the second byte (MS byte)
    uint8_t ms_byte = ow_read_byte(gpio_port, pin, tim_port);
    // Combine both bytes into a signed 16 bit value
    int16_t temp_byte = (ms_byte << 8) | ls_byte;
    // Divide by 16 to get actual temp in C
    float temp = ((float)temp_byte) / 16;
    return temp;
}
