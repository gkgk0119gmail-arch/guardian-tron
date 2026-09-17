#include "vesc_uart.h"

#include <string.h>

#define COMM_SET_DUTY          5
#define COMM_SET_CURRENT       6
#define COMM_SET_CURRENT_BRAKE 7
#define COMM_SET_SERVO_POS     12
#define COMM_ALIVE              30

static UART_HandleTypeDef *s_huart;

static uint16_t vesc_crc16(const uint8_t *data, size_t len) {
    /* CRC-16/XMODEM (poly 0x1021, init 0x0000), matches jetson/ecu/vesc.py
     * _crc16() and real VESC firmware's crc.c. */
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void vesc_send(const uint8_t *payload, size_t len) {
    if (s_huart == NULL) {
        return;
    }
    uint8_t frame[32];
    size_t idx = 0;

    frame[idx++] = 0x02; /* short-frame start byte, payload assumed <=255 bytes here */
    frame[idx++] = (uint8_t)len;
    memcpy(&frame[idx], payload, len);
    idx += len;

    uint16_t crc = vesc_crc16(payload, len);
    frame[idx++] = (uint8_t)((crc >> 8) & 0xFF);
    frame[idx++] = (uint8_t)(crc & 0xFF);
    frame[idx++] = 0x03; /* end byte */

    HAL_UART_Transmit(s_huart, frame, (uint16_t)idx, 50 /* ms timeout */);
}

static void put_i32_be(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

void vesc_uart_init(UART_HandleTypeDef *huart) {
    s_huart = huart;
}

void vesc_set_servo_pos(float pos) {
    /* Confirmed against vedderb/bldc comm/commands.c:
     * buffer_get_float16(data, 1000.0, &ind) -- a big-endian int16
     * scaled by 1000, NOT the int32-scaled-by-1000/100 pattern the other
     * commands use. Get this wrong and the servo either doesn't move or
     * jumps to the wrong position -- not motor-dangerous, but re-check
     * against your exact firmware if servo behavior looks off. */
    if (pos > 1.0f) pos = 1.0f;
    if (pos < 0.0f) pos = 0.0f;
    int16_t scaled = (int16_t)(pos * 1000.0f);
    uint8_t payload[3];
    payload[0] = COMM_SET_SERVO_POS;
    payload[1] = (uint8_t)((scaled >> 8) & 0xFF);
    payload[2] = (uint8_t)(scaled & 0xFF);
    vesc_send(payload, sizeof(payload));
}

void vesc_set_duty(float duty) {
    if (duty > 1.0f) duty = 1.0f;
    if (duty < -1.0f) duty = -1.0f;
    uint8_t payload[5];
    payload[0] = COMM_SET_DUTY;
    put_i32_be(&payload[1], (int32_t)(duty * 100000.0f));
    vesc_send(payload, sizeof(payload));
}

void vesc_set_current(float amps) {
    uint8_t payload[5];
    payload[0] = COMM_SET_CURRENT;
    put_i32_be(&payload[1], (int32_t)(amps * 1000.0f));
    vesc_send(payload, sizeof(payload));
}

void vesc_set_current_brake(float amps) {
    uint8_t payload[5];
    payload[0] = COMM_SET_CURRENT_BRAKE;
    put_i32_be(&payload[1], (int32_t)(amps * 1000.0f));
    vesc_send(payload, sizeof(payload));
}

void vesc_alive(void) {
    uint8_t payload[1] = { COMM_ALIVE };
    vesc_send(payload, sizeof(payload));
}
