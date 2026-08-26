#ifndef INC_SPI_H_
#define INC_SPI_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "tim.h"

typedef enum {
    SPI_PRESCALER_2,
    SPI_PRESCALER_4,
    SPI_PRESCALER_8,
    SPI_PRESCALER_16,
    SPI_PRESCALER_32,
    SPI_PRESCALER_64,
    SPI_PRESCALER_128,
    SPI_PRESCALER_256
} SPI_Prescaler;

void SPI_Init(SPI_TypeDef *port, SPI_Prescaler prescaler, TIM_TypeDef *tim_port);
uint8_t SPI_TransferByte(SPI_TypeDef *port, TIM_TypeDef *tim_port, uint8_t byte_in, uint8_t *byte_out);

#endif
