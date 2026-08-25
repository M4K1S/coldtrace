#ifndef INC_TIM_H_
#define INC_TIM_H_

#include <stdint.h>
#include "stm32f4xx.h"

void TIM_Init(TIM_TypeDef *port, uint32_t prescaler, uint32_t autoReload);
void TIM_Delay_us(TIM_TypeDef *port, uint32_t micros);
void TIM_Delay_ms(TIM_TypeDef *port, uint32_t ms);
void TIM_Start(TIM_TypeDef *port);
void TIM_Stop(TIM_TypeDef *port);
uint8_t TIM_Ready(TIM_TypeDef *port);

#endif /* INC_TIM_H_ */
