#ifndef APP_H
#define APP_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "gpio.h"
#include "uart.h"
#include "tim.h"
#include "pwm.h"

void App_Init(void);
void App_Loop(void);

#endif
