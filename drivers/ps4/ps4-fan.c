// SPDX-License-Identifier: GPL-2.0-only
/**
 * PS4 Aeolia/Belize Fan + Thermal hwmon Driver
 *
 * Copyright (C) rmux <armandas.kvietkus@proton.me>
 *
 *Based on ps4fancontrol by Ps3itaTeam.
 *Thanks to Zer0xFF for finding the ICC fan threshold command
 *and to shuffle2 for the patch exposing ICC to usermode.
 * ============================================================
 * Overview
 * ============================================================
 * hwmon driver for the PlayStation 4 fan controller on Aeolia
 * and Belize southbridges. Exposes APU temperature, fan RPM,
 * and fan threshold via standard hwmon sysfs attributes.
 *
 * ============================================================
 * Critical: Read-Modify-Write Protocol
 * ============================================================
 * The EMC fan configuration struct (0x0A/0x07) contains factory
 * flags at bytes [9]=0x08 and [12]=0x80 that control the PID
 * loop. If these are zeroed, the EMC disables active cooling
 * and fan RPM reads 0 until a hard power cycle restores them.
 *
 * The legacy ps4fancontrol userspace tool had this exact bug:
 * it allocated 6 bytes but told the ICC the payload was 0x34
 * bytes, injecting random kernel stack memory into the config.
 *
 * The correct write protocol is:
 *   1. Read full 52-byte config from 0x0A/0x07
 *   2. Modify ONLY byte[5] (threshold in integer Celsius)
 *   3. Write the same 52 bytes back to 0x0A/0x06
 *
 * icc_write_fan_threshold() implements this correctly.
 *
 * ============================================================
 * Register Map (discovered via live hardware probing)
 * ============================================================
 *
 * 0x0B/0x01 reply[3]:
 *   APU temperature, plain u8 integer Celsius. Primary source
 *   for temp1_input. Not fixed-point — no division needed.
 *
 * 0x0A/0x07 reply[5]:
 *   Fan threshold, plain u8 integer Celsius. Read for temp1_crit.
 *
 * 0x0A/0x08 reply[8:12]:
 *   Fan RPM, u32 little-endian, 16.16 fixed-point.
 *   RPM = raw_u32 / 65536. Values 0xFFFFFFFF and 0x0FFFFFFF
 *   indicate fan at base idle (below threshold) — reported as 0.
 *
 * 0x0A/0x08 reply[16:18]:
 *   APU temperature, u16 little-endian, 8.8 fixed-point.
 *   temp_C = raw_u16 / 256.0. Alternative source — currently
 *   unused in favour of the simpler 0x0B/0x01 path.
 *
 * ============================================================
 * hwmon Interface
 * ============================================================
 *   /sys/class/hwmon/hwmonX/
 *     temp1_input  (RO) — APU temperature, milli-Celsius
 *     temp1_crit   (RW) — Fan threshold, milli-Celsius (45000–85000)
 *     fan1_input   (RO) — Fan speed, RPM
 *
 * ============================================================
 * License: GPL-2.0-only
 * ============================================================
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/err.h>
#include "aeolia.h"
#include "ps4-fan.h"

/* ============================================================
 * Private Driver State
 * ============================================================
 * @lock: Serializes all ICC transactions to prevent interleaved
 *        read-modify-write sequences from concurrent sysfs access.
 */
struct ps4_fan_priv {
	struct mutex lock;
};

/* ============================================================
 * icc_read_apu_temp - Read live APU temperature
 * ============================================================
 * @temp_mc: Output, milli-Celsius on success.
 *
 * Source: ICC 0x0B/0x01, reply[3], plain integer Celsius.
 * Reply buffer zeroed before call to prevent stale-data reads.
 *
 * Caller must hold priv->lock.
 */
static int icc_read_apu_temp(long *temp_mc)
{
	u8 reply[PS4_FAN_TEMP_REPLY_LEN];
	int ret;

	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_TEMP_ICC_MAJOR, PS4_FAN_TEMP_ICC_MINOR,
			    NULL, 0, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	*temp_mc = (long)reply[PS4_FAN_TEMP_BYTE] * 1000L;
	return 0;
}

/* ============================================================
 * icc_read_fan_threshold - Read current fan threshold
 * ============================================================
 * @thresh_mc: Output, milli-Celsius on success.
 *
 * Source: ICC 0x0A/0x07, reply[5], plain integer Celsius.
 *
 * Caller must hold priv->lock.
 */
static int icc_read_fan_threshold(long *thresh_mc)
{
	u8 reply[PS4_FAN_CONFIG_REPLY_LEN];
	int ret;

	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_GET,
			    NULL, 0, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;
	if (reply[PS4_FAN_THRESH_BYTE] < PS4_FAN_THRESH_MIN_C ||
	    reply[PS4_FAN_THRESH_BYTE] > PS4_FAN_THRESH_MAX_C)
		return -ERANGE;

	*thresh_mc = (long)reply[PS4_FAN_THRESH_BYTE] * 1000L;
	return 0;
}

/* ============================================================
 * icc_write_fan_threshold - Set fan threshold (read-modify-write)
 * ============================================================
 * @thresh_mc: Desired threshold, milli-Celsius (45000–85000).
 *
 * IMPORTANT: Implements a full read-modify-write cycle.
 *
 *   Step 1: Read the full 52-byte config from 0x0A/0x07.
 *           This preserves the factory flags at bytes [9] and
 *           [12] that control the EMC PID loop. Zeroing these
 *           disables active cooling until a hard power cycle.
 *
 *   Step 2: Overwrite only byte[5] with the new threshold.
 *
 *   Step 3: Write the full 52 bytes back to 0x0A/0x06.
 *
 * Caller must hold priv->lock.
 */
static int icc_write_fan_threshold(long thresh_mc)
{
	u8 config[PS4_FAN_CONFIG_REPLY_LEN];
	u8 reply[0x20];
	u8 thresh_c;
	int ret;

	if (thresh_mc < PS4_FAN_THRESH_MIN_MC ||
	    thresh_mc > PS4_FAN_THRESH_MAX_MC)
		return -EINVAL;

	thresh_c = (u8)(thresh_mc / 1000L);

	/* Step 1: Read current config — preserves factory flags */
	memset(config, 0, sizeof(config));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_GET,
			    NULL, 0, config, sizeof(config));
	if (ret < 0)
		return ret;
	if (config[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	/* Step 2: Modify only the threshold byte */
	config[PS4_FAN_THRESH_BYTE] = thresh_c;

	/* Step 3: Write full config back — PS4_FAN_CONFIG_LEN = 52 bytes */
	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_SET,
			    config, PS4_FAN_CONFIG_LEN,
			    reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	return 0;
}

/* ============================================================
 * icc_read_fan_rpm - Read live fan RPM
 * ============================================================
 * @rpm: Output, fan speed in RPM (integer) on success.
 *
 * Source: ICC 0x0A/0x08, reply[8:12], u32 LE, 16.16 fixed-point.
 * Divide raw value by PS4_FAN_RPM_SCALE (65536) to get RPM.
 *
 * Sentinel values 0xFFFFFFFF and 0x0FFFFFFF indicate the fan
 * is running at base idle speed (below threshold). Reported
 * as 0 RPM — the hwmon layer handles display appropriately.
 *
 * Caller must hold priv->lock.
 */
static int icc_read_fan_rpm(long *rpm)
{
	u8 reply[PS4_FAN_STATUS_REPLY_LEN];
	u32 raw;
	int ret;

	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_STATUS,
			    NULL, 0, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	raw = le32_to_cpup((__le32 *)(reply + PS4_FAN_RPM_OFFSET));

	if (raw == PS4_FAN_RPM_INVALID_1 || raw == PS4_FAN_RPM_INVALID_2)
		*rpm = 0;
	else
		*rpm = (long)(raw / PS4_FAN_RPM_SCALE);

	return 0;
}

/* ============================================================
 * hwmon ops: is_visible
 * ============================================================
 */
static umode_t ps4_fan_is_visible(const void *drvdata,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (channel != 0)
			return 0;
		switch (attr) {
		case hwmon_temp_input: return 0444;
		case hwmon_temp_crit:  return 0644;
		default:               return 0;
		}
	case hwmon_fan:
		if (channel != 0)
			return 0;
		if (attr == hwmon_fan_input)
			return 0444;
		return 0;
	default:
		return 0;
	}
}

/* ============================================================
 * hwmon ops: read
 * ============================================================
 * Dispatches:
 *   temp1_input  → icc_read_apu_temp()      (0x0B/0x01 reply[3])
 *   temp1_crit   → icc_read_fan_threshold() (0x0A/0x07 reply[5])
 *   fan1_input   → icc_read_fan_rpm()       (0x0A/0x08 reply[8:12])
 */
static int ps4_fan_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct ps4_fan_priv *priv = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&priv->lock);

	switch (type) {
	case hwmon_temp:
		if (channel != 0) { ret = -EOPNOTSUPP; break; }
		switch (attr) {
		case hwmon_temp_input:
			ret = icc_read_apu_temp(val);
			break;
		case hwmon_temp_crit:
			ret = icc_read_fan_threshold(val);
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_fan:
		if (channel != 0 || attr != hwmon_fan_input) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = icc_read_fan_rpm(val);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	mutex_unlock(&priv->lock);
	return ret;
}

/* ============================================================
 * hwmon ops: write
 * ============================================================
 * Only temp1_crit is writable. Triggers read-modify-write.
 */
static int ps4_fan_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct ps4_fan_priv *priv = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_temp || channel != 0 || attr != hwmon_temp_crit)
		return -EOPNOTSUPP;

	mutex_lock(&priv->lock);
	ret = icc_write_fan_threshold(val);
	mutex_unlock(&priv->lock);

	return ret;
}

/* ============================================================
 * hwmon Chip Info
 * ============================================================
 * Two channels:
 *   temp[0]: temp1_input (RO), temp1_crit (RW)
 *   fan[0]:  fan1_input (RO)
 */
static const struct hwmon_channel_info * const ps4_fan_channel_info[] = {
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_CRIT),
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT),
	NULL,
};

static const struct hwmon_ops ps4_fan_hwmon_ops = {
	.is_visible = ps4_fan_is_visible,
	.read       = ps4_fan_read,
	.write      = ps4_fan_write,
};

static const struct hwmon_chip_info ps4_fan_chip_info = {
	.ops  = &ps4_fan_hwmon_ops,
	.info = ps4_fan_channel_info,
};

/* ============================================================
 * Platform Driver: probe / remove
 * ============================================================ */
static int ps4_fan_probe(struct platform_device *pdev)
{
	struct ps4_fan_priv *priv;
	struct device *hwmon_dev;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	hwmon_dev = devm_hwmon_device_register_with_info(
			&pdev->dev, "ps4_fan", priv,
			&ps4_fan_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(&pdev->dev,
		 "PS4 fan hwmon ready — "
		 "temp1_input, temp1_crit (%d–%d°C), fan1_input\n",
		 PS4_FAN_THRESH_MIN_C, PS4_FAN_THRESH_MAX_C);
	return 0;
}

static void ps4_fan_remove(struct platform_device *pdev) {}

/* ============================================================
 * Platform Driver / Module Registration
 * ============================================================ */
static struct platform_driver ps4_fan_driver = {
	.probe  = ps4_fan_probe,
	.remove = ps4_fan_remove,
	.driver = { .name = "ps4-fan" },
};

static struct platform_device *ps4_fan_pdev;

static int __init ps4_fan_init(void)
{
	int ret;

	ret = platform_driver_register(&ps4_fan_driver);
	if (ret) {
		pr_err("ps4-fan: failed to register driver: %d\n", ret);
		return ret;
	}

	ps4_fan_pdev = platform_device_register_simple("ps4-fan", -1, NULL, 0);
	if (IS_ERR(ps4_fan_pdev)) {
		ret = PTR_ERR(ps4_fan_pdev);
		pr_err("ps4-fan: failed to register device: %d\n", ret);
		platform_driver_unregister(&ps4_fan_driver);
		ps4_fan_pdev = NULL;
		return ret;
	}

	return 0;
}

static void __exit ps4_fan_exit(void)
{
	if (ps4_fan_pdev)
		platform_device_unregister(ps4_fan_pdev);
	platform_driver_unregister(&ps4_fan_driver);
}

module_init(ps4_fan_init);
module_exit(ps4_fan_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("rmux <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 Aeolia/Belize fan threshold and RPM hwmon driver");
MODULE_ALIAS("platform:ps4-fan");