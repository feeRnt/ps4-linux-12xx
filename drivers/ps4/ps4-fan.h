/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * ps4-fan.h — PS4 Aeolia/Belize fan + thermal hwmon driver header
 *
 * Copyright (C) rmux <armandas.kvietkus@proton.me>
 *
 * ============================================================
 * Hardware Compatibility
 * ============================================================
 * Confirmed on:
 *   PS4 Original        — Aeolia southbridge (CXD90025G)
 *   PS4 Slim            — Belize southbridge (protocol identical)
 *
 * ============================================================
 * ICC Command Map — Fan Info Device (Major 0x0A)
 * ============================================================
 *
 * 0x0A/0x07 — Read thermal configuration (52 bytes)
 *   reply[0]   = ICC status (0x00 = OK)
 *   reply[5]   = fan threshold, integer Celsius (u8)
 *   reply[9]   = factory flag 0x08  — must be preserved on write
 *   reply[12]  = factory flag 0x80  — must be preserved on write
 *   reply[16-27] = 0xff,0xff,0xff,0x0f pattern (sensor slot markers)
 *                  must be preserved on write
 *
 * 0x0A/0x06 — Write thermal configuration (52 bytes)
 *   CRITICAL: This command must receive the FULL 52-byte struct
 *   read from 0x0A/0x07 with ONLY byte[5] modified.
 *   Sending a zero-filled buffer destroys the factory flags at
 *   bytes [9] and [12], which disables the EMC PID loop and
 *   causes fan RPM to read 0 permanently until hard power cycle.
 *   This was the bug in the legacy ps4fancontrol userspace tool.
 *
 * 0x0A/0x08 — Read live fan + thermal data (0x52 bytes)
 *   reply[0]    = ICC status (0x00 = OK)
 *   reply[5]    = fan duty ADC (raw, scale TBD)
 *   reply[8:12] = fan RPM, u32 little-endian, 16.16 fixed-point
 *                 RPM = raw / 65536.0
 *                 Returns 0xFFFFFFFF or 0x0FFFFFFF when fan is
 *                 at base idle (below threshold). Treat as 0.
 *   reply[16:18]= APU temperature, u16 little-endian, 8.8 fixed-point
 *                 temp_C = raw / 256.0
 *
 * ============================================================
 * ICC Command Map — Device Power Info (Major 0x0B)
 * ============================================================
 *
 * 0x0B/0x01 — Read APU temperature (0x10 bytes)
 *   reply[0]  = ICC status (0x00 = OK)
 *   reply[3]  = APU temperature, plain integer Celsius (u8)
 *   NOTE: Plain integer, NOT fixed-point. No division required.
 *   This is the primary temperature source for temp1_input.
 *
 * ============================================================
 * hwmon Attribute Map
 * ============================================================
 *   temp1_input  (RO) — Live APU temp, milli-Celsius
 *                       Source: 0x0B/0x01 reply[3] * 1000
 *   temp1_crit   (RW) — Fan threshold, milli-Celsius
 *                       Read:  0x0A/0x07 reply[5] * 1000
 *                       Write: read-modify-write via 0x0A/0x07→0x0A/0x06
 *                       Valid: 45000–85000 milli-Celsius
 *   fan1_input   (RO) — Fan speed, RPM (integer)
 *                       Source: 0x0A/0x08 reply[8:12], 16.16 fixed-point
 */

#ifndef _PS4_FAN_H
#define _PS4_FAN_H

/* ============================================================
 * ICC Identifiers — Fan Info Device
 * ============================================================ */
#define PS4_FAN_ICC_MAJOR               0x0A
#define PS4_FAN_ICC_MINOR_GET           0x07  /* read thermal config  */
#define PS4_FAN_ICC_MINOR_SET           0x06  /* write thermal config */
#define PS4_FAN_ICC_MINOR_STATUS        0x08  /* live RPM + temp data */

/* ============================================================
 * ICC Identifiers — APU Temperature
 * ============================================================ */
#define PS4_FAN_TEMP_ICC_MAJOR          0x0B
#define PS4_FAN_TEMP_ICC_MINOR          0x01

/* ============================================================
 * Buffer Sizes
 * ============================================================ */
#define PS4_FAN_CONFIG_LEN              0x34  /* 52 bytes — full config struct */
#define PS4_FAN_CONFIG_REPLY_LEN        0x52  /* reply buffer for 0x0A/0x07    */
#define PS4_FAN_STATUS_REPLY_LEN        0x52  /* reply buffer for 0x0A/0x08    */
#define PS4_FAN_TEMP_REPLY_LEN          0x10  /* reply buffer for 0x0B/0x01    */

/* ============================================================
 * Byte Offsets Within ICC Reply Buffers
 * ============================================================ */
#define PS4_ICC_STATUS_BYTE             0     /* reply[0]: 0x00 = success       */
#define PS4_FAN_THRESH_BYTE             5     /* config[5]: threshold °C        */
#define PS4_FAN_RPM_OFFSET              8     /* status[8:12]: u32 LE 16.16 RPM */
#define PS4_FAN_TEMP_ALT_OFFSET         16    /* status[16:18]: u16 LE 8.8 °C   */
#define PS4_FAN_TEMP_BYTE               3     /* temp reply[3]: plain u8 °C     */

/* ============================================================
 * RPM Fixed-Point Scaling
 * ============================================================
 * Fan RPM is encoded as 16.16 fixed-point (u32 LE).
 * Divide raw value by 65536 to get RPM.
 * Values 0xFFFFFFFF and 0x0FFFFFFF indicate fan at base idle
 * (below threshold) — report as 0 RPM.
 */
#define PS4_FAN_RPM_SCALE               65536U
#define PS4_FAN_RPM_INVALID_1           0xFFFFFFFFU
#define PS4_FAN_RPM_INVALID_2           0x0FFFFFFFU

/* ============================================================
 * Fan Threshold Limits
 * ============================================================ */
#define PS4_FAN_THRESH_MIN_C            45
#define PS4_FAN_THRESH_MAX_C            85
#define PS4_FAN_THRESH_DEFAULT_C        79    /* hardware power-on default */

#define PS4_FAN_THRESH_MIN_MC           ((long)(PS4_FAN_THRESH_MIN_C) * 1000L)
#define PS4_FAN_THRESH_MAX_MC           ((long)(PS4_FAN_THRESH_MAX_C) * 1000L)
#define PS4_FAN_THRESH_DEFAULT_MC       ((long)(PS4_FAN_THRESH_DEFAULT_C) * 1000L)

#endif /* _PS4_FAN_H */