#ifndef INC_ONEWIRE_H_
#define INC_ONEWIRE_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "gpio.h"
#include "tim.h"

void ow_init(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port);
uint8_t ow_reset(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port);
void ow_write_bit(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port, uint8_t bit);
uint8_t ow_read_bit(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port);
void ow_write_byte(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port, uint8_t byte);
uint8_t ow_read_byte(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port);

#endif
