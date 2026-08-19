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
uint32_t nextTick = 1;
uint32_t i = 1;
static uint8_t duty;

void App_Init()
{
    PWM_Init(TIM2, 2, 1000, 50);
    PWM_Start(TIM2, 2);
    UART_Init(USART2, 112500);
}

void App_Loop(void)
{
    uint32_t now = uwTick;

    if (UART_DataAvailable(USART2)) {
        char c = UART_ReceiveChar(USART2);
        UART_SendChar(USART2, c);
        if (c >= '0' && c <= '9') {
            duty = duty * 10 + (c - '0');
        }
        else if (c == '\r' || c == '\n') {
            UART_SendChar(USART2, '\n');
            UART_SendChar(USART2, '\r');
            if (duty > 100) duty = 100;
            PWM_SetDuty(TIM2, 2, duty);
            duty = 0;
        }
    }
}
