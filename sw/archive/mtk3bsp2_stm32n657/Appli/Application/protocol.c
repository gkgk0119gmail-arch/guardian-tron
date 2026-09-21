#include "protocol.h"
#include <string.h>

uint8_t gk_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00u;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ 0x07u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_f32(uint8_t *p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

static float get_f32(const uint8_t *p) {
    uint32_t bits = get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

void gk_encode_cmd(const gk_cmd_t *cmd, uint8_t out[GK_CMD_FRAME_LEN]) {
    out[0] = GK_CMD_STX;
    put_u32(&out[1], cmd->seq);
    put_f32(&out[5], cmd->steer_deg);
    put_f32(&out[9], cmd->accel_mps2);
    out[13] = gk_crc8(&out[1], 12);
    out[14] = GK_ETX;
}

int gk_decode_cmd(const uint8_t buf[GK_CMD_FRAME_LEN], gk_cmd_t *cmd) {
    if (buf[0] != GK_CMD_STX || buf[14] != GK_ETX) {
        return 0;
    }
    uint8_t crc = gk_crc8(&buf[1], 12);
    if (crc != buf[13]) {
        return 0;
    }
    cmd->seq = get_u32(&buf[1]);
    cmd->steer_deg = get_f32(&buf[5]);
    cmd->accel_mps2 = get_f32(&buf[9]);
    return 1;
}

void gk_encode_verdict(const gk_verdict_frame_t *v, uint8_t out[GK_VERDICT_FRAME_LEN]) {
    out[0] = GK_VERDICT_STX;
    put_u32(&out[1], v->seq);
    out[5] = (uint8_t)v->verdict;
    put_u32(&out[6], v->latency_us);
    out[10] = gk_crc8(&out[1], 9);
    out[11] = GK_ETX;
}

int gk_decode_verdict(const uint8_t buf[GK_VERDICT_FRAME_LEN], gk_verdict_frame_t *v) {
    if (buf[0] != GK_VERDICT_STX || buf[11] != GK_ETX) {
        return 0;
    }
    uint8_t crc = gk_crc8(&buf[1], 9);
    if (crc != buf[10]) {
        return 0;
    }
    v->seq = get_u32(&buf[1]);
    v->verdict = (gk_verdict_t)buf[5];
    v->latency_us = get_u32(&buf[6]);
    return 1;
}
