#include "tim.h"

static const uint32_t fCK = 16000000U; // Clock speed of TIM

void TIM_Init(TIM_TypeDef *port, uint32_t prescaler, uint32_t autoReload) {

    if(port == TIM2) {
        RCC->APB1ENR |= (0b1); // Start TIM2 clock
    }

    if(prescaler < 1 || autoReload < 1) return;

    port->PSC = (prescaler - 1U); // Set pre-scaler
    port->ARR = (autoReload - 1U); // Set auto reload
    port->CNT = 0U; // Set count to 0
    port->EGR |= (0b1U); // Generate update event
    port->SR &= ~(0b1U); // Clear UIF

}

void TIM_Init_ms(TIM_TypeDef *port, uint16_t ms) {

    if(port == TIM2) {
        RCC->APB1ENR |= (0b1); // Start TIM2 clock
    }

    if (ms == 0U) return;

    port->ARR = (1000U - 1U); // Set auto reload to 1000
    port->PSC = (uint32_t)(((uint64_t)fCK * ms) / 1000000U) - 1U; // Set pre-scaler to ((ms/1000) * fCK) / (ARR + 1) - 1
    port->CNT = 0U; // Set count to 0
    port->EGR |= (0b1U); // Generate update event
    port->SR &= ~(0b1U); // Clear UIF

}

void TIM_DelayMicros(TIM_TypeDef *port, uint32_t micros){
    port->CNT = 0;
    port->CR1 |= (0b1U);
    while(port->CNT < micros) {}
    port->CR1 &= ~(0b1U);
}

void TIM_Start(TIM_TypeDef *port) {
    port->CR1 |= (0b1U);
}

void TIM_Stop(TIM_TypeDef *port) {
    port->CR1 &= ~(0b1U);
}

uint8_t TIM_Ready(TIM_TypeDef *port) {
    if(port->SR & (0b1U)) {
        port->SR &= ~(0b1U);
        return 1U;
    }

    return 0U;
}
