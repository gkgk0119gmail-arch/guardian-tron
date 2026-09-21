# Safety-Critical UART Link Protocol

Defines the control command frame from the Jetson (main ECU) to the STM32
(gatekeeper), and the verdict response frame from the gatekeeper to the Jetson.
Since UART is a stream, STX/ETX and CRC8 are used to find byte boundaries.

Default settings: **115200 baud, 8N1**. When actually wiring, `config.h` (STM32
side) and `--baud` (Jetson side) must both be set to the UART port/baud rate
that matches the board.

## CMD frame (Jetson → gatekeeper), 15 bytes

| offset | size | field | type | description |
|---|---|---|---|---|
| 0 | 1 | STX | u8 | fixed 0xAA |
| 1 | 4 | seq | u32 LE | transmit sequence number (monotonically increasing) |
| 5 | 4 | steer_deg | f32 LE | steering angle (degrees) |
| 9 | 4 | accel_mps2 | f32 LE | longitudinal acceleration (m/s^2) |
| 13 | 1 | crc8 | u8 | CRC-8 (poly 0x07) over offset 1..12 |
| 14 | 1 | ETX | u8 | fixed 0x55 |

## VERDICT frame (gatekeeper → Jetson), 12 bytes

| offset | size | field | type | description |
|---|---|---|---|---|
| 0 | 1 | STX2 | u8 | fixed 0xBB |
| 1 | 4 | seq | u32 LE | seq of the corresponding CMD |
| 5 | 1 | verdict | u8 | 0=APPROVED, 1=VETO (envelope violation), 2=MALFORMED (CRC/parse failure) |
| 6 | 4 | latency_us | u32 LE | receive→verdict latency (microseconds) |
| 10 | 1 | crc8 | u8 | CRC-8 over offset 1..9 |
| 11 | 1 | ETX2 | u8 | fixed 0x55 |

## Safety envelope (identical to safety_envelope.c)

- Absolute: `|steer_deg| <= 540`, `accel_mps2 ∈ [-10, 4]`
- Rate of change (1 cycle, at 100Hz): `|Δsteer_deg| <= 10`, `|Δaccel_mps2| <= 1`
- Violation of either condition → VETO, hold the last approved command (u_safe)

## Threat scenarios and frame handling

- **Threat 1: adversarial perception attack (FGSM)**: same format as a normal
  CMD frame, but carries dangerous steer/accel values produced by perception →
  the gatekeeper validates against the envelope based on the values (the frame
  itself is well-formed, so this is the key validation target).
- **Threat 2: CAN injection/spoofing-like attack**: arbitrary/spoofed CMD frames
  are injected directly onto the UART link (raw bytes inserted into the
  socket/serial port without going through the Jetson process).
  The gatekeeper defends using sequence anomalies, envelope violations and CRC failures.
- **Threat 3: OTA/firmware compromise**: an attempt to reload a tampered
  gatekeeper binary → the RoT (signature verification, see `firmware_sign.py`)
  refuses to load it on signature mismatch.
