#ifndef GK_PROTOCOL_H
#define GK_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define GK_CMD_STX   0xAAu
#define GK_VERDICT_STX 0xBBu
#define GK_ETX       0x55u

#define GK_CMD_FRAME_LEN     15u
#define GK_VERDICT_FRAME_LEN 12u

typedef enum {
    GK_VERDICT_APPROVED = 0,
    GK_VERDICT_VETO      = 1,
    GK_VERDICT_MALFORMED = 2,
} gk_verdict_t;

typedef struct {
    uint32_t seq;
    float steer_deg;
    float accel_mps2;
} gk_cmd_t;

typedef struct {
    uint32_t seq;
    gk_verdict_t verdict;
    uint32_t latency_us;
} gk_verdict_frame_t;

uint8_t gk_crc8(const uint8_t *data, size_t len);

/* Encodes a CMD frame into out[GK_CMD_FRAME_LEN]. Used by the Jetson-side
 * simulator / test harness that speaks this protocol in C; the real Jetson
 * ECU uses protocol.py, kept byte-for-byte identical to this layout. */
void gk_encode_cmd(const gk_cmd_t *cmd, uint8_t out[GK_CMD_FRAME_LEN]);

/* Returns 1 and fills *cmd if buf holds a structurally valid CMD frame
 * (correct STX/ETX and CRC). Returns 0 otherwise (malformed/tampered). */
int gk_decode_cmd(const uint8_t buf[GK_CMD_FRAME_LEN], gk_cmd_t *cmd);

void gk_encode_verdict(const gk_verdict_frame_t *v, uint8_t out[GK_VERDICT_FRAME_LEN]);
int gk_decode_verdict(const uint8_t buf[GK_VERDICT_FRAME_LEN], gk_verdict_frame_t *v);

#endif /* GK_PROTOCOL_H */
