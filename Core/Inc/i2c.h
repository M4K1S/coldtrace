#ifndef INC_I2C_H_
#define INC_I2C_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include <tim.h>

typedef enum {
    SPEED_STANDARD,
    SPEED_FAST
} I2C_SPEED;

typedef enum {
    READ,
    WRITE
} I2C_RW;

void I2C_Init(I2C_TypeDef *port, I2C_SPEED speed);
void I2C_Start(I2C_TypeDef *port);

#endif /* INC_I2C_H_ */
