#ifndef INC_DS18B20_H_
#define INC_DS18B20_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "onewire.h"

uint8_t ds18b20_start_conversion(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port);
float ds18b20_read_temp(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port);

#endif
