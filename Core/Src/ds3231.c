#include "ds3231.h"

uint8_t bcd_to_dec(uint8_t bcd) {
    uint8_t tens = bcd >> 4; // Shift top 4 bits to the right
    uint8_t ones = bcd & 0b1111U; // Mask so we only have bottom 4
    uint8_t dec = tens * 10 + ones; // Turn into decimal
    return dec;
}
uint8_t dec_to_bcd(uint8_t dec) {
    uint8_t tens = dec / 10; // Integer division by 10
    uint8_t ones = dec % 10; // Modulo by 10
    uint8_t bcd = (tens << 4) | ones; // Combine into 8 bits
    return bcd;
}

uint8_t rtc_set_time(I2C_TypeDef *port, TIM_TypeDef *tim_port, RTC_Time *time) {
    // Fill buffer with time
    uint8_t buffer[7];
    buffer[0] = dec_to_bcd(time->seconds);
    buffer[1] = dec_to_bcd(time->minutes);
    buffer[2] = dec_to_bcd(time->hours);
    buffer[3] = dec_to_bcd(time->day);
    buffer[4] = dec_to_bcd(time->date);
    buffer[5] = dec_to_bcd(time->month);
    buffer[6] = dec_to_bcd(time->year);
    // Write bytes to DS3231
    if(!I2C_WriteBytes(port, tim_port, DS3231_ADDR, 0x00, buffer, 7)){
        return 0;
    }
    return 1;
}

uint8_t rtc_get_time(I2C_TypeDef *port, TIM_TypeDef *tim_port, RTC_Time *time) {
        // Create a buffer for raw values
        uint8_t buffer[7];
        // Start at 0x00 (seconds) and the register pointer will auto increment
        if (!I2C_ReadBytes(port, tim_port, DS3231_ADDR, 0x00, buffer, 7)) {
            return 0;
        }

        time->seconds = bcd_to_dec(buffer[0]);
        time->minutes = bcd_to_dec(buffer[1]);
        time->hours   = bcd_to_dec(buffer[2] & 0b00111111U); // mask off 12/24hr mode bits
        time->day     = bcd_to_dec(buffer[3]);
        time->date    = bcd_to_dec(buffer[4]);
        time->month   = bcd_to_dec(buffer[5]);
        time->year    = bcd_to_dec(buffer[6]);

        return 1;
}
