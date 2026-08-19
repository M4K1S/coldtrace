#include "app.h"
#include <stdint.h>
#include "gpio.h"
#include "uart.h"
#include "tim.h"
#include "pwm.h"
#include "stm32f4xx_hal.h"

// Define HIGH and LOW for GPIO library
#define HIGH 1
#define LOW 0

// Delay for loop
uint32_t nextTick = 250;


void App_Init()
{
    PWM_Init(TIM2, 2, 10, 50);
    PWM_Start(TIM2, 2);
}

void App_Loop(void)
{


}
