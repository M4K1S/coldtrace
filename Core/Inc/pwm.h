
#ifndef INC_PWM_H_
#define INC_PWM_H_

void PWM_Init(TIM_TypeDef *port, uint8_t channel, uint32_t frequency, uint8_t dutyCycle);
void PWM_Start(TIM_TypeDef *port, uint8_t channel);
void PWM_SetDuty(TIM_TypeDef *port, uint8_t channel, uint8_t dutyCycle);
void PWM_Stop(TIM_TypeDef *port, uint8_t channel);

#endif /* INC_PWM_H_ */
