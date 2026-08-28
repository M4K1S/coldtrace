#include "gpio.h"

void GPIO_Init(GPIO_TypeDef *port, uint8_t pin, GPIO_Mode mode, GPIO_OutputType type) {
    // Turn on GPIO clocks
    if      (port == GPIOA) RCC->AHB1ENR |= (1U << 0); // Turn on GPIOA Clock
    else if (port == GPIOB) RCC->AHB1ENR |= (1U << 1); // Turn on GPIOB Clock
    else if (port == GPIOC) RCC->AHB1ENR |= (1U << 2); // Turn on GPIOC Clock
    else if (port == GPIOD) RCC->AHB1ENR |= (1U << 3); // Turn on GPIOD Clock
    else if (port == GPIOE) RCC->AHB1ENR |= (1U << 4); // Turn on GPIOE Clock

    // Set MODER - enum values match MODER's bit encoding
    port->MODER &= ~(0b11U << (pin * 2)); // Clear MODER bits
    port->MODER |= (mode << (pin * 2));   // Set MODER to requested mode

    // Set OTYPER - applies to OUTPUT and ALT_FUNC modes (both drive the pin)
    if (mode == OUTPUT || mode == ALT_FUNC) {
        port->OTYPER &= ~(0b1U << pin); // Clear OTYPER bit
        port->OTYPER |= (type << pin);  // Set OTYPER
    }
}
void GPIO_SetMode(GPIO_TypeDef *port, uint8_t pin, GPIO_Mode mode) {
    port->MODER &= ~(0b11U << (pin * 2)); // Clear MODER bits
    port->MODER |= (mode << (pin * 2));   // Set MODER
}
void GPIO_Set(GPIO_TypeDef *port, uint8_t pin, uint8_t state) {
    if (state) port->ODR |= (0b1U << pin); // Sets pin HIGH if state is 1
    else port->ODR &= ~(0b1U << pin); // Sets pin LOW if state is 0
}

void GPIO_Toggle(GPIO_TypeDef *port, uint8_t pin) {
    port->ODR  ^= (0b1U << pin);
}
uint8_t GPIO_Read(GPIO_TypeDef *port, uint8_t pin) {
    return (port->IDR >> pin) & 0b1U;
}
