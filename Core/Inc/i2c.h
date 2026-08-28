#ifndef INC_I2C_H_
#define INC_I2C_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "tim.h"

typedef enum {
    SPEED_STANDARD,
    SPEED_FAST
} I2C_SPEED;

typedef enum {
    WRITE,
    READ
} I2C_RW;

typedef enum {
    NACK,
    ACK
} I2C_ACK;

void I2C_Init(I2C_TypeDef *port, I2C_SPEED speed, TIM_TypeDef *tim_port);
uint8_t I2C_Start(I2C_TypeDef *port, TIM_TypeDef *tim_port);
void I2C_Stop(I2C_TypeDef *port);
uint8_t I2C_SendAddress(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t address, I2C_RW rw);
uint8_t I2C_WriteByte(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t byte);
uint8_t I2C_ReadByte(I2C_TypeDef *port, TIM_TypeDef *tim_port, I2C_ACK ack, uint8_t *byte_out);
uint8_t I2C_WriteReg(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t value);
uint8_t I2C_ReadReg(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *value_out);
uint8_t I2C_WriteBytes(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len);
uint8_t I2C_ReadBytes(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len);

#endif /* INC_I2C_H_ */
