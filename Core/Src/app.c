#include "app.h"
#include <stdint.h>
#include "uart.h"
#include "ds18b20.h"

// DS18B20 data line + the timer dedicated to OneWire's microsecond delays
#define TEMP_PORT GPIOA
#define TEMP_PIN  10
#define TEMP_TIM  TIM2

// TIM_Delay_us can't span more than one timer period (ARR = 65535 @ 1MHz tick,
// so ~65ms max per call) — chain calls to build longer waits.
static void delay_ms(TIM_TypeDef *tim_port, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        TIM_Delay_us(tim_port, 1000);
    }
}

// Prints a signed float with 2 decimal places, no libc float-printf dependency
static void UART_SendTemp(USART_TypeDef *port, float tempC) {
    int32_t centi = (int32_t)(tempC * 100.0f + (tempC >= 0.0f ? 0.5f : -0.5f));
    if (centi < 0) {
        UART_SendChar(port, '-');
        centi = -centi;
    }

    int32_t whole = centi / 100;
    int32_t frac = centi % 100;

    char digits[4];
    uint8_t n = 0;
    if (whole == 0) {
        digits[n++] = '0';
    } else {
        while (whole > 0) {
            digits[n++] = '0' + (whole % 10);
            whole /= 10;
        }
    }
    while (n > 0) {
        UART_SendChar(port, digits[--n]);
    }

    UART_SendChar(port, '.');
    UART_SendChar(port, '0' + (frac / 10));
    UART_SendChar(port, '0' + (frac % 10));
}

void App_Init(void)
{
    UART_Init(USART2, 115200);
    ow_init(TEMP_PORT, TEMP_PIN, TEMP_TIM);
}

void App_Loop(void)
{
    if (!ds18b20_start_conversion(TEMP_PORT, TEMP_PIN, TEMP_TIM)) {
        UART_SendString(USART2, "DS18B20 not found\r\n");
        delay_ms(TEMP_TIM, 1000);
        return;
    }

    delay_ms(TEMP_TIM, 750); // tCONV max for 12-bit resolution

    float tempC;
    if (!ds18b20_read_temp(TEMP_PORT, TEMP_PIN, TEMP_TIM, &tempC)) {
        UART_SendString(USART2, "DS18B20 read failed\r\n");
        delay_ms(TEMP_TIM, 1000);
        return;
    }
    UART_SendString(USART2, "Temp: ");
    UART_SendTemp(USART2, tempC);
    UART_SendString(USART2, " C\r\n");

    delay_ms(TEMP_TIM, 1000);
}
