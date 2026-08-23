#include "tim.h"

static const uint32_t fCK = 16000000U; // Clock speed of TIM

static void TIM_EnableClock(TIM_TypeDef *port) {
    if      (port == TIM2) RCC->APB1ENR |= (0b1U << 0); // Start TIM2 clock
    else if (port == TIM3) RCC->APB1ENR |= (0b1U << 1); // Start TIM3 clock
    else if (port == TIM4) RCC->APB1ENR |= (0b1U << 2); // Start TIM4 clock
    else if (port == TIM5) RCC->APB1ENR |= (0b1U << 3); // Start TIM5 clock
}

void TIM_Init(TIM_TypeDef *port, uint32_t prescaler, uint32_t autoReload) {

    TIM_EnableClock(port);

    if(prescaler < 1 || autoReload < 1) return;

    port->PSC = (prescaler - 1U); // Set pre-scaler
    port->ARR = (autoReload - 1U); // Set auto reload
    port->CNT = 0U; // Set count to 0
    port->EGR |= (0b1U); // Generate update event
    port->SR &= ~(0b1U); // Clear UIF

}

void TIM_Init_ms(TIM_TypeDef *port, uint16_t ms) {

    TIM_EnableClock(port);

    if (ms == 0U) return;

    port->ARR = (1000U - 1U); // Set auto reload to 1000
    port->PSC = (uint32_t)(((uint64_t)fCK * ms) / 1000000U) - 1U; // Set pre-scaler to ((ms/1000) * fCK) / (ARR + 1) - 1
    port->CNT = 0U; // Set count to 0
    port->EGR |= (0b1U); // Generate update event
    port->SR &= ~(0b1U); // Clear UIF

}

void TIM_Delay_us(TIM_TypeDef *port, uint32_t micros){
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
        return 1;
    }

    return 0;
}
