#ifndef INC_GPIO_H_
#define INC_GPIO_H_

#include <stdint.h>
#include "stm32f4xx.h"

typedef enum {
    PUSH_PULL,
    OPEN_DRAIN
} GPIO_OutputType;

typedef enum {
    INPUT,
    OUTPUT,
    ALT_FUNC,
    ANALOG
} GPIO_Mode;

void GPIO_Init(GPIO_TypeDef *port, uint8_t pin, GPIO_Mode mode, GPIO_OutputType type);
void GPIO_SetMode(GPIO_TypeDef *port, uint8_t pin, GPIO_Mode mode);
void GPIO_Set(GPIO_TypeDef *port, uint8_t pin, uint8_t state);
void GPIO_Toggle(GPIO_TypeDef *port, uint8_t pin);
uint8_t GPIO_Read(GPIO_TypeDef *port, uint8_t pin);

#endif /* INC_GPIO_H_ */
