#include "uart.h"
#include "clock.h"
#include <math.h>

/*
 * ================= UART Pin Map =================
 *
 * USART1
 *   TX -> PA9
 *   RX -> PA10
 *   AF7
 *
 * USART2
 *   TX -> PA2
 *   RX -> PA3
 *   AF7
 *
 * USART3
 *   TX -> PB10
 *   RX -> PB11
 *   AF7
 *
 * UART4
 *   TX -> PC10
 *   RX -> PC11
 *   AF8
 *
 * UART5
 *   TX -> PC12
 *   RX -> PD2
 *   AF8
 *
 * USART6
 *   TX -> PC6
 *   RX -> PC7
 *   AF8
 *
 * Default Configuration:
 *   - 8 data bits
 *   - 1 stop bit
 *   - No parity
 *   - No flow control
 */

static const uint32_t fCK = FCK_HZ; // Clock speed of USART

void UART_Init(USART_TypeDef *port, uint32_t baudRate) {

    // TURN ON USART CLOCK & CONFIGURE PINS FOR RX & TX & Set AF

    if (port == USART1) {
        RCC->APB2ENR |= (0b1U << 4); // Start USART1 Clock
        RCC->AHB1ENR |= (0b1U << 0); // Start GPIOA Clock
        GPIOA->MODER &= ~((0b11U << 18) | (0b11U << 20)); // Clear PA9 & PA10
        GPIOA->MODER |= (0b10U << 18) | (0b10U << 20); // Set PA9 & PA10 to AF
        GPIOA->AFR[1] &= ~((0b1111U << 4) | (0b1111U << 8)); // Clear AF
        GPIOA->AFR[1] |= (0b0111U << 4) | (0b0111U << 8); // Set AF7
    }
    else if (port == USART2) {
        RCC->APB1ENR |= (0b1U << 17); // Start USART2 Clock
        RCC->AHB1ENR |= (0b1U << 0); // Start GPIOA Clock
        GPIOA->MODER &= ~((0b11U << 4) | (0b11U << 6)); // Clear PA2 & PA3
        GPIOA->MODER |= (0b10U << 4) | (0b10U << 6); // Set PA2 & PA3 to AF
        GPIOA->AFR[0] &= ~((0b1111U << 8) | (0b1111U << 12)); // Clear AF
        GPIOA->AFR[0] |= (0b0111U << 8) | (0b0111U << 12); // Set AF7
    }
    else if (port == USART3) {
        RCC->APB1ENR |= (0b1U << 18); // Start USART3 Clock
        RCC->AHB1ENR |= (0b1U << 1); // Start GPIOB Clock
        GPIOB->MODER &= ~((0b11U << 20) | (0b11U << 22)); // Clear PB10 & PB11
        GPIOB->MODER |= (0b10U << 20) | (0b10U << 22); // Set PB10 & PB11 to AF
        GPIOB->AFR[1] &= ~((0b1111U << 8) | (0b1111U << 12)); // Clear AF
        GPIOB->AFR[1] |= (0b0111U << 8) | (0b0111U << 12); // Set AF7
    }
    else if (port == UART4) {
        RCC->APB1ENR |= (0b1U << 19); // Start UART4 Clock
        RCC->AHB1ENR |= (0b1U << 2); // Start GPIOC Clock
        GPIOC->MODER &= ~((0b11U << 20) | (0b11U << 22)); // Clear PC10 & PC11
        GPIOC->MODER |= (0b10U << 20) | (0b10U << 22); // Set PC10 & PC11 to AF
        GPIOC->AFR[1] &= ~((0b1111U << 8) | (0b1111U << 12)); // Clear AF
        GPIOC->AFR[1] |= (0b1000U << 8) | (0b1000U << 12); // Set AF8
    }
    else if (port == UART5) {
        RCC->APB1ENR |= (0b1U << 20); // Start UART5 Clock
        RCC->AHB1ENR |= (0b1U << 2); // Start GPIOC Clock
        RCC->AHB1ENR |= (0b1U << 3); // Start GPIOD Clock
        GPIOC->MODER &= ~(0b11U << 24); // Clear PC12
        GPIOC->MODER |= (0b10U << 24); // Set PC12 to AF
        GPIOD->MODER &= ~(0b11U << 4); // Clear PD2
        GPIOD->MODER |= (0b10U << 4); // Set PD2 to AF
        GPIOC->AFR[1] &= ~(0b1111U << 16); // Clear AF
        GPIOC->AFR[1] |= (0b1000U << 16); // Set AF8
        GPIOD->AFR[0] &= ~(0b1111U << 8); // Clear AF
        GPIOD->AFR[0] |= (0b1000U << 8); // Set AF8
    }
    else if (port == USART6) {
        RCC->APB2ENR |= (0b1U << 5);  // Start USART6 Clock
        RCC->AHB1ENR |= (0b1U << 2); // Start GPIOC Clock
        GPIOC->MODER &= ~((0b11U << 12) | (0b11U << 14)); // Clear PC6 & PC7
        GPIOC->MODER |= (0b10U << 12) | (0b10U << 14); // Set PC6 & PC7 to AF
        GPIOC->AFR[0] &= ~((0b1111U << 24) | (0b1111U << 28)); // Clear AF
        GPIOC->AFR[0] |= (0b1000U << 24) | (0b1000U << 28); // Set AF8
    }

    // CONFIGURE BAUD RATE

    float USARTDIV = fCK / (16.0 * baudRate);
    uint32_t mantissa = (uint32_t)USARTDIV; // Mantissa is the whole number part taken from USARTDIV
    uint32_t DIV_Fraction = (uint32_t)round(16.0 * (USARTDIV - mantissa)); // Fraction Part of USARTDIV: Multiply DIV_Fraction by 16 (4 bits) and round to nearest real #

    // If fraction is 16 (1) set it to 0 carry into the mantissa
    if (DIV_Fraction == 16) {
        DIV_Fraction = 0;
        mantissa++;
    }

    port->BRR = (mantissa << 4) | DIV_Fraction; // DIV_Fraction is at bit 0-3 and Mantissa is at bit 4-15 of BRR

    // Enable UE, RE, and TE, Set M to 0 (8 bit)

    port->CR1 = (0b1U << 13 ) | (0b1U << 3) | (0b1U << 2) | (0b0U << 12);

    // Set stop bit to 00 (1 stop bit) - STOP[1:0] is CR2 bits 13:12

    port->CR2 = (0b00U << 12);

}

void UART_SendChar(USART_TypeDef *port, char c) {
    // Wait for TXE to be 1
    while((port->SR & (0b1U << 7)) == 0) {
    }
    port->DR = c; // Set the data register to the char being transmitted
}

void UART_SendString(USART_TypeDef *port, const char *s) {
    // Repeat until char == '\0'
    for(uint32_t i = 0; s[i] != '\0'; i++) {
        UART_SendChar(port, s[i]); // Send one char a time
    }
}

char UART_ReceiveChar(USART_TypeDef *port) {
    // Wait until RXNE == 1
    while((port->SR & (0b1U << 5)) == 0) {
    }
    // Return char
    return(port->DR);
}

void UART_WaitForTC(USART_TypeDef *port) {
    // Wait until TC == 1 (Transmission complete)
    while((port->SR & (0b1U << 6)) == 0) {
    }
}

uint8_t UART_DataAvailable(USART_TypeDef *port) {
    return (port->SR & (0b1U << 5)) != 0; // Checks RXNE bit if there is any data
}

