#include "i2c.h"

#define I2C_TIMEOUT_US 5000U // 5ms timeout for I2C

static const uint32_t fCK = 16000000U; // Clock speed of i2C
static const uint32_t fastCK = 400000U;
static const uint32_t standCK = 100000U;

void I2C_Init(I2C_TypeDef *port, I2C_SPEED speed, TIM_TypeDef *tim_port){
    if (port == I2C1) {
        RCC->APB1ENR |= (0b1U << 21); // Start I2C1 Clock
        RCC->AHB1ENR |= (0b1U << 1);  // Start GPIOB Clock

        GPIOB->MODER &= ~((0b11U << 16) | (0b11U << 18)); // Clear PB8 & PB9
        GPIOB->MODER |= (0b10U << 16) | (0b10U << 18);     // Set PB8 & PB9 to AF
        GPIOB->OTYPER |= (0b1U << 8) | (0b1U << 9); // Set PB8 & PB9 to open-drain
        GPIOB->PUPDR &= ~((0b11U << 16) | (0b11U << 18)); // Clear PB8 & PB9 pull config
        GPIOB->PUPDR |= (0b01U << 16) | (0b01U << 18);     // Set PB8 & PB9 to pull-up


        GPIOB->AFR[1] &= ~((0b1111U << 0) | (0b1111U << 4)); // Clear AF for PB8 (bits 0-3) & PB9 (bits 4-7)
        GPIOB->AFR[1] |= (0b0100U << 0) | (0b0100U << 4);    // Set AF4
    }

    port->CR2 &= ~(0b111111U); // Clear FREQ bits (bits 5:0)
    port->CR2 |= (fCK / 1000000U); // Set FREQ to fCK in MHz

    if(speed == SPEED_FAST) {
        port->CCR = (0b1U << 15) | (fCK / (3 * fastCK)); // If FAST mode then set F/S bit to 1 and set CCR for fast mode
        port->TRISE = (fCK / 1000000U) * 3U / 10U + 1U; // (FREQ_MHz * 300 / 1000) + 1
    }
    if(speed == SPEED_STANDARD) {
        port->CCR = (fCK / (2 * standCK)); // If STANDARD mode then set F/S bit to 0 and set CCR for standard mode
        port->TRISE = (fCK / 1000000U) + 1U; // FREQ_MHz + 1
    }

    port->CR1 |= (0b1U); // Set PE bit to 1 - enable I2C

    TIM_Init(tim_port, 16, 65535); // configure 1MHz tick rate for I2C timeouts
}

uint8_t I2C_Start(I2C_TypeDef *port, TIM_TypeDef *tim_port) {
    port->CR1 |= (0b1U << 8); // Set START bit

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // start the timeout clock

    while (!(port->SR1 & (0b1U << 0))) { // wait for SB flag
        if (tim_port->CNT >= I2C_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U); // stop the timeout clock
            return 0; // timed out
        }
    }

    tim_port->CR1 &= ~(0b1U); // stop the timeout clock
    return 1; // success
}

void I2C_Stop(I2C_TypeDef *port) {
    port->CR1 |= (0b1U << 9); // Set STOP bit
}
