#ifndef APP_H
#define APP_H

#include <stdint.h>
#include "gpio.h"
#include "uart.h"
#include "tim.h"
#include "pwm.h"

void App_Init(void);
void App_Loop(void);
uint32_t App_Millis(void);
void App_TickISR(void);

#endif
