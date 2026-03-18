/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * ps4-led.h — Public header for the PS4 Aeolia front panel LED driver
 *
 * Copyright (C) rmux <armandas.kvietkus@proton.me>
 *
 * This header exposes the constants, command definitions, and mode
 * enum used by the PS4 Aeolia LED driver (ps4-led.c). It is split
 * from the implementation to allow other drivers or in-tree test
 * code to reference LED operating modes and ICC wire-format
 * constants without pulling in driver internals.
 *
 * Consumers: ps4-led.c, any future driver that needs to query or
 * coordinate with the front panel LED state.
 */

#ifndef _PS4_LED_H
#define _PS4_LED_H

/* ============================================================
 * ICC Command Identifiers — LED / Indicator
 * ============================================================
 * Used with apcie_icc_cmd() to drive the front panel LED.
 *
 * PS4_LED_ICC_MAJOR:  ICC major number for the indicator device.
 *                     Registered on Orbis as "icc indicator device".
 * PS4_LED_ICC_MINOR:  Minor command number for setting LED state.
 * PS4_LED_PAYLOAD_LEN: Fixed byte length of every LED payload.
 *                      All led_* arrays in ps4-led.c are this size.
 */
#define PS4_LED_ICC_MAJOR       0x09
#define PS4_LED_ICC_MINOR       0x20
#define PS4_LED_PAYLOAD_LEN     35

/* ============================================================
 * Thermal Indicator Temperature Thresholds (integer Celsius)
 * ============================================================
 * Used by the thermal polling worker to select the LED color.
 * Temperatures are sourced from ICC 0x0B/0x01, reply[3], which
 * returns a plain integer Celsius value (not fixed-point).
 *
 * PS4_LED_TEMP_COOL_MAX:
 *   Upper bound for "cool" state (Blue LED).
 *   APU temp < this value => Blue.
 *
 * PS4_LED_TEMP_WARM_MAX:
 *   Upper bound for "warm" state (White LED).
 *   PS4_LED_TEMP_COOL_MAX <= temp < this value => White.
 *   temp >= this value => Orange.
 */
#define PS4_LED_TEMP_COOL_MAX   65      /* °C: Blue  -> White boundary  */
#define PS4_LED_TEMP_WARM_MAX   80      /* °C: White -> Orange boundary */

/* ============================================================
 * ICC Command Identifiers — APU Temperature
 * ============================================================
 * Used by the thermal worker to read the live APU temperature.
 * Registered on Orbis as "icc device_power info device".
 *
 * PS4_LED_TEMP_ICC_MAJOR: Major for the device_power info device.
 * PS4_LED_TEMP_ICC_MINOR: Minor for the APU temperature query.
 * PS4_LED_TEMP_REPLY_LEN: Minimum reply buffer size in bytes.
 * PS4_LED_TEMP_BYTE:      Byte offset of the integer °C value
 *                         within the ICC reply buffer.
 */
#define PS4_LED_TEMP_ICC_MAJOR  0x0B
#define PS4_LED_TEMP_ICC_MINOR  0x01
#define PS4_LED_TEMP_REPLY_LEN  0x10
#define PS4_LED_TEMP_BYTE       3

/* ============================================================
 * ICC Status Code
 * ============================================================
 * reply[0] of any ICC transaction. 0x00 means the EMC accepted
 * the command and the remaining reply bytes are valid.
 */
#define PS4_ICC_STATUS_OK       0x00

/* ============================================================
 * Default Thermal Polling Interval
 * ============================================================
 * Milliseconds between thermal worker invocations when
 * MODE_THERMAL is active. Writable at runtime via the
 * thermal_interval_ms sysfs attribute.
 */
#define PS4_LED_THERMAL_INTERVAL_MS_DEFAULT     2000U

/* ============================================================
 * enum ps4_led_mode — LED driver operating mode
 * ============================================================
 * @MODE_STATIC:   Default. LED class brightness nodes operate
 *                 normally. No ICC temperature polling occurs.
 *                 The thermal delayed_work is not scheduled.
 *
 * @MODE_THERMAL:  Thermal worker is active. APU temperature is
 *                 polled via ICC on a configurable interval and
 *                 the LED color is updated automatically:
 *                   Blue   = APU temp < PS4_LED_TEMP_COOL_MAX
 *                   White  = PS4_LED_TEMP_COOL_MAX <= temp < PS4_LED_TEMP_WARM_MAX
 *                   Orange = APU temp >= PS4_LED_TEMP_WARM_MAX
 *
 * Controlled via /sys/bus/platform/devices/ps4-led/mode.
 */
enum ps4_led_mode {
	MODE_STATIC,
	MODE_THERMAL,
};

#endif /* _PS4_LED_H */