// SPDX-License-Identifier: GPL-2.0-only
/**
 * PS4 Aeolia Sysfs LED Driver — with Thermal Indicator Mode
 *
 * Copyright (C) rmux <armandas.kvietkus@proton.me>
 *
- **Saya** — documented supported colors and provided the Belize `led_config` source that cracked the payload format open
- **PsxItArch** — original LED control script the driver is based on
 * ============================================================
 * Overview
 * ============================================================
 * This Linux kernel driver provides sysfs LED control for the
 * PlayStation 4's Aeolia southbridge (CXD90025G). It exposes
 * multiple LED nodes via the Linux LED subsystem, allowing
 * user-space to control the color and state of the front panel
 * LEDs, and optionally enables a "Thermal Indicator Mode" that
 * automatically changes the LED color based on APU temperature.
 *
 * ============================================================
 * Hardware Context
 * ============================================================
 *  Southbridge : Aeolia CXD90025G
 *  ICC interface: APU x86 <-> Aeolia EMC ARM Cortex-M3
 *  Communication occurs via shared memory. The exported kernel
 *  function apcie_icc_cmd() is used throughout.
 *
 * ============================================================
 * Features
 * ============================================================
 *  - Manual LED color/effect control via LED class sysfs nodes.
 *  - Thermal Indicator Mode: polls APU temperature via ICC and
 *    drives LED color automatically:
 *      Blue   = APU temp < PS4_LED_TEMP_COOL_MAX (cool)
 *      White  = PS4_LED_TEMP_COOL_MAX to PS4_LED_TEMP_WARM_MAX (warm)
 *      Orange = APU temp >= PS4_LED_TEMP_WARM_MAX (hot)
 *  - Configurable polling interval via sysfs attribute.
 *  - Clean integration with the platform device/driver model.
 *  - Thermal worker is never active until explicitly enabled
 *    by user-space; zero overhead in static mode.
 *  - Worker is always cancelled synchronously on driver removal
 *    to prevent use-after-free.
 *
 * ============================================================
 * Sysfs Interface
 * ============================================================
 *
 * LED class nodes (manual/static control):
 *   /sys/class/leds/ps4:<color>:status/brightness
 *     Write >0 to turn on, write 0 to turn off.
 *     Available: blue, white, orange, orange_blue, orange_white,
 *     pulsate_orange, orange_white_blue, white_blue, violet_blue,
 *     pink, pink_blue.
 *
 * Platform device attributes (thermal mode):
 *   /sys/bus/platform/devices/ps4-led/mode
 *     "static" (default) or "thermal".
 *
 *   /sys/bus/platform/devices/ps4-led/thermal_interval_ms
 *     Polling interval in milliseconds. Default: 2000. Must be >0.
 *
 * ============================================================
 * Usage Examples
 * ============================================================
 *   # Manual control:
 *   echo 255 > /sys/class/leds/ps4:blue:status/brightness
 *   echo 0   > /sys/class/leds/ps4:blue:status/brightness
 *
 *   # Enable thermal mode:
 *   echo 3000    > /sys/bus/platform/devices/ps4-led/thermal_interval_ms
 *   echo thermal > /sys/bus/platform/devices/ps4-led/mode
 *
 *   # Return to static:
 *   echo static > /sys/bus/platform/devices/ps4-led/mode
 *
 * ============================================================
 * Implementation Notes
 * ============================================================
 *  - Binary payloads per color/effect are statically defined and
 *    sent via apcie_icc_cmd(PS4_LED_ICC_MAJOR, PS4_LED_ICC_MINOR).
 *  - Temperature is sourced from ICC PS4_LED_TEMP_ICC_MAJOR /
 *    PS4_LED_TEMP_ICC_MINOR. reply[PS4_LED_TEMP_BYTE] is a plain
 *    integer Celsius — not 8.8 fixed-point. No division required.
 *  - delayed_work is initialized in probe() but NOT scheduled
 *    until mode transitions to MODE_THERMAL.
 *  - Mutex protects mode and interval. cancel_delayed_work_sync()
 *    is always called with the mutex released to avoid deadlock.
 *  - devm_* allocations are used wherever possible.
 *
 * ============================================================
 * Limitations
 * ============================================================
 *  - Driving multiple brightness nodes simultaneously results
 *    in undefined hardware behavior (last write wins on EMC).
 *  - In thermal mode, a manual brightness write is overridden
 *    on the next poll. Return to static mode first.
 *  - Payloads are reverse-engineered and may not cover all
 *    possible LED states.
 *
 * ============================================================
 * Dependencies
 * ============================================================
 *  - Aeolia ICC interface (apcie_icc_cmd) via aeolia.h.
 *  - CONFIG_LEDS_CLASS must be enabled.
 *  - See Kconfig: PS4_LED depends on PS4_APCIE && LEDS_CLASS.
 */

#include <linux/module.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "aeolia.h"
#include "ps4-led.h"

/* ============================================================
 * ICC LED Payloads
 * ============================================================
 * Each array encodes a full ICC command payload for:
 *   Major: PS4_LED_ICC_MAJOR (0x09)
 *   Minor: PS4_LED_ICC_MINOR (0x20)
 *   Length: PS4_LED_PAYLOAD_LEN (35 bytes)
 *
 * These define the diode mix and PWM behavior sent to the
 * Aeolia EMC, which drives the front panel LED hardware.
 * Format is a proprietary binary protocol reverse-engineered
 * from hardware observation.
 *
 * The three thermal-mode payloads (led_blue, led_white,
 * led_orange) are referenced directly by ps4_thermal_work_fn().
 * Their ordering must remain consistent with the thresholds
 * PS4_LED_TEMP_COOL_MAX and PS4_LED_TEMP_WARM_MAX in ps4-led.h.
 */

/** led_off: All LED channels disabled. Front panel dark. */
static const u8 led_off[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x00,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0x00, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_blue: Solid blue. Thermal mode: APU temp < PS4_LED_TEMP_COOL_MAX. */
static const u8 led_blue[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0xff,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0x00, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_white: Solid white. Thermal mode: PS4_LED_TEMP_COOL_MAX <= temp < PS4_LED_TEMP_WARM_MAX. */
static const u8 led_white[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x00,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0xff, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0x00, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_orange: Solid orange. Thermal mode: APU temp >= PS4_LED_TEMP_WARM_MAX. */
static const u8 led_orange[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x00,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x02, 0xff, 0x02, 0x01,
	0x00, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0xff,
	0x05, 0x01, 0x00
};

/** led_orange_blue: Orange and blue channels simultaneously. */
static const u8 led_orange_blue[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0xff,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_orange_white: Orange and white channels simultaneously. */
static const u8 led_orange_white[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x00,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0xff, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_pulsate_orange: Orange channel with PWM pulsation. */
static const u8 led_pulsate_orange[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x00,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0xff, 0x04, 0x01,
	0x00, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0xff,
	0x05, 0x01, 0x00
};

/** led_orange_white_blue: All three channels active together. */
static const u8 led_orange_white_blue[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0xff,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0xff, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_white_blue: White and blue channels simultaneously. */
static const u8 led_white_blue[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0xff,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0xff, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x01, 0x00, 0x04, 0x01,
	0xbf, 0x02, 0x00, 0x05, 0x01, 0xff, 0x02, 0x00,
	0x05, 0x01, 0xff
};

/** led_violet_blue: Violet-tinted blue (partial blue channel). */
static const u8 led_violet_blue[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x57,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x02, 0xff, 0x02, 0x01,
	0x00, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0xff,
	0x05, 0x01, 0x00
};

/** led_pink: Pink hue (partial white + orange blend). */
static const u8 led_pink[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x00,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x30, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x02, 0xff, 0x02, 0x01,
	0x00, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0xff,
	0x05, 0x01, 0x00
};

/** led_pink_blue: Pink with a blue tint (partial blue + orange). */
static const u8 led_pink_blue[] = {
	0x03, 0x01, 0x00, 0x00, 0x10, 0x01, 0x02, 0x20,
	0x02, 0x01, 0x00, 0x11, 0x01, 0x02, 0x00, 0x02,
	0x01, 0x00, 0x02, 0x03, 0x02, 0xff, 0x02, 0x01,
	0x00, 0x02, 0xff, 0x05, 0x01, 0xff, 0x02, 0xff,
	0x05, 0x01, 0x00
};

/* ============================================================
 * Private Driver State
 * ============================================================
 * One instance per platform_device. Allocated via devm_kzalloc
 * in probe() and stored via platform_set_drvdata().
 *
 * @pdev:               Back-pointer to the owning platform device.
 *                      Used for dev_dbg() calls inside the worker.
 * @thermal_work:       Delayed work for thermal polling. Initialized
 *                      in probe() but NOT scheduled until user-space
 *                      writes "thermal" to the mode attribute.
 * @lock:               Mutex protecting @mode and @thermal_interval_ms
 *                      against concurrent sysfs and worker access.
 *                      Never held across cancel_delayed_work_sync().
 * @mode:               Current operating mode. See enum ps4_led_mode
 *                      in ps4-led.h. Protected by @lock.
 * @thermal_interval_ms: Polling interval in ms. Default:
 *                      PS4_LED_THERMAL_INTERVAL_MS_DEFAULT. Must be >0.
 *                      Protected by @lock.
 */
struct ps4_led_priv {
	struct platform_device  *pdev;
	struct delayed_work      thermal_work;
	struct mutex             lock;
	enum ps4_led_mode        mode;
	unsigned int             thermal_interval_ms;
};

/* ============================================================
 * ps4_thermal_work_fn - Thermal polling worker
 * ============================================================
 * @work: Embedded work_struct inside ps4_led_priv.thermal_work.
 *
 * Reads the APU temperature via ICC (major PS4_LED_TEMP_ICC_MAJOR,
 * minor PS4_LED_TEMP_ICC_MINOR) and updates the front panel LED
 * color according to the thresholds in ps4-led.h.
 *
 * ICC response format:
 *   reply[PS4_ICC_STATUS_BYTE] = 0x00 on success.
 *   reply[PS4_LED_TEMP_BYTE]   = APU temperature, integer Celsius.
 *   NOTE: Plain integer — NOT 8.8 fixed-point. No division needed.
 *
 * The reply buffer is zero-initialised before every call to prevent
 * acting on stale data from prior ICC transactions (the Aeolia ICC
 * reply buffer is not cleared between calls per the reference doc).
 *
 * Lifecycle:
 *   If mode has reverted to MODE_STATIC before this invocation runs,
 *   the worker returns immediately without rescheduling, terminating
 *   the polling chain. Otherwise it reschedules itself for
 *   thermal_interval_ms milliseconds.
 *
 *   cancel_delayed_work_sync() in ps4_led_remove() guarantees this
 *   function has fully returned before the device is freed.
 */
static void ps4_thermal_work_fn(struct work_struct *work)
{
	struct ps4_led_priv *priv =
		container_of(to_delayed_work(work),
			     struct ps4_led_priv, thermal_work);
	u8 temp_reply[PS4_LED_TEMP_REPLY_LEN];
	u8 led_reply[0x30];
	const u8 *payload;
	unsigned int interval_ms;
	int ret;

	/*
	 * Check mode under lock. If switched back to static while
	 * this work was pending, bail without rescheduling.
	 */
	mutex_lock(&priv->lock);
	if (priv->mode != MODE_THERMAL) {
		mutex_unlock(&priv->lock);
		return;
	}
	interval_ms = priv->thermal_interval_ms;
	mutex_unlock(&priv->lock);

	/* Zero buffer to guard against ICC stale-data behaviour. */
	memset(temp_reply, 0, sizeof(temp_reply));

	ret = apcie_icc_cmd(PS4_LED_TEMP_ICC_MAJOR, PS4_LED_TEMP_ICC_MINOR,
			    NULL, 0, temp_reply, sizeof(temp_reply));
	if (ret < 0 || temp_reply[PS4_ICC_STATUS_OK] != 0x00) {
		dev_dbg(&priv->pdev->dev,
			"thermal: ICC temp read failed (ret=%d status=0x%02x)\n",
			ret, temp_reply[0]);
		goto reschedule;
	}

	/*
	 * Select payload based on thresholds defined in ps4-led.h.
	 *   < PS4_LED_TEMP_COOL_MAX  => Blue  (cool)
	 *   < PS4_LED_TEMP_WARM_MAX  => White (warm)
	 *   >= PS4_LED_TEMP_WARM_MAX => Orange (hot)
	 */
	if (temp_reply[PS4_LED_TEMP_BYTE] < PS4_LED_TEMP_COOL_MAX)
		payload = led_blue;
	else if (temp_reply[PS4_LED_TEMP_BYTE] < PS4_LED_TEMP_WARM_MAX)
		payload = led_white;
	else
		payload = led_orange;

	dev_dbg(&priv->pdev->dev,
		"thermal: APU %u°C => %s\n",
		temp_reply[PS4_LED_TEMP_BYTE],
		(payload == led_blue)  ? "blue"  :
		(payload == led_white) ? "white" : "orange");

	apcie_icc_cmd(PS4_LED_ICC_MAJOR, PS4_LED_ICC_MINOR,
		      (void *)payload, PS4_LED_PAYLOAD_LEN,
		      led_reply, sizeof(led_reply));

reschedule:
	schedule_delayed_work(&priv->thermal_work,
			      msecs_to_jiffies(interval_ms));
}

/* ============================================================
 * ps4_led_set - LED class brightness callback (static mode)
 * ============================================================
 * @led_cdev: The LED class device whose brightness changed.
 * @value:    LED_OFF (0) to turn off, any positive value to enable.
 *
 * Called by the LED subsystem when user-space writes to a
 * /sys/class/leds/ps4:<color>:status/brightness node.
 *
 * Color selection uses strstr() on the led_classdev name. More
 * specific compound names (e.g. "orange_white_blue") are checked
 * before their substrings ("orange", "white", "blue") to avoid
 * false early matches.
 *
 * In thermal mode, a write here will be overridden on the next
 * poll cycle. Switch to static mode first for manual control.
 */
static void ps4_led_set(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	const u8 *data = led_off;
	u8 reply[0x30];

	if (value != LED_OFF) {
		if (strstr(led_cdev->name, "orange_white_blue"))
			data = led_orange_white_blue;
		else if (strstr(led_cdev->name, "pulsate_orange"))
			data = led_pulsate_orange;
		else if (strstr(led_cdev->name, "orange_white"))
			data = led_orange_white;
		else if (strstr(led_cdev->name, "orange_blue"))
			data = led_orange_blue;
		else if (strstr(led_cdev->name, "white_blue"))
			data = led_white_blue;
		else if (strstr(led_cdev->name, "violet_blue"))
			data = led_violet_blue;
		else if (strstr(led_cdev->name, "pink_blue"))
			data = led_pink_blue;
		else if (strstr(led_cdev->name, "pink"))
			data = led_pink;
		else if (strstr(led_cdev->name, "orange"))
			data = led_orange;
		else if (strstr(led_cdev->name, "white"))
			data = led_white;
		else if (strstr(led_cdev->name, "blue"))
			data = led_blue;
	}

	apcie_icc_cmd(PS4_LED_ICC_MAJOR, PS4_LED_ICC_MINOR,
		      (void *)data, PS4_LED_PAYLOAD_LEN,
		      reply, sizeof(reply));
}

/* ============================================================
 * LED Class Device Nodes
 * ============================================================
 * One struct led_classdev per color/effect. All share ps4_led_set.
 * Registered via devm_led_classdev_register() in probe().
 * Exposed at /sys/class/leds/ps4:<color>:status/
 */
static struct led_classdev ps4_led_nodes[] = {
	{ .name = "ps4:blue:status",              .brightness_set = ps4_led_set },
	{ .name = "ps4:white:status",             .brightness_set = ps4_led_set },
	{ .name = "ps4:orange:status",            .brightness_set = ps4_led_set },
	{ .name = "ps4:orange_blue:status",       .brightness_set = ps4_led_set },
	{ .name = "ps4:orange_white:status",      .brightness_set = ps4_led_set },
	{ .name = "ps4:pulsate_orange:status",    .brightness_set = ps4_led_set },
	{ .name = "ps4:orange_white_blue:status", .brightness_set = ps4_led_set },
	{ .name = "ps4:white_blue:status",        .brightness_set = ps4_led_set },
	{ .name = "ps4:violet_blue:status",       .brightness_set = ps4_led_set },
	{ .name = "ps4:pink:status",              .brightness_set = ps4_led_set },
	{ .name = "ps4:pink_blue:status",         .brightness_set = ps4_led_set },
};

/* ============================================================
 * Sysfs Attribute: mode
 * ============================================================
 * Path: /sys/bus/platform/devices/ps4-led/mode
 *
 * mode_show - Read current operating mode.
 * Returns "static\n" or "thermal\n".
 */
static ssize_t mode_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct ps4_led_priv *priv = dev_get_drvdata(dev);
	const char *s;

	mutex_lock(&priv->lock);
	s = (priv->mode == MODE_THERMAL) ? "thermal" : "static";
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%s\n", s);
}

/**
 * mode_store - Write new operating mode.
 * @buf: "static" or "thermal". Other values return -EINVAL.
 *
 * static -> thermal:
 *   Sets mode under lock, unlocks, then schedules the thermal
 *   worker immediately (delay = 0). The worker self-reschedules.
 *
 * thermal -> static:
 *   Sets mode under lock (so a concurrent worker wakeup exits
 *   cleanly), unlocks, then calls cancel_delayed_work_sync()
 *   to wait for any in-progress invocation to complete.
 *   cancel_delayed_work_sync() MUST be called with @lock released
 *   — the work function acquires @lock itself; holding it here
 *   would deadlock.
 */
static ssize_t mode_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ps4_led_priv *priv = dev_get_drvdata(dev);
	bool want_thermal;

	if (sysfs_streq(buf, "thermal"))
		want_thermal = true;
	else if (sysfs_streq(buf, "static"))
		want_thermal = false;
	else
		return -EINVAL;

	mutex_lock(&priv->lock);

	if (want_thermal && priv->mode != MODE_THERMAL) {
		priv->mode = MODE_THERMAL;
		mutex_unlock(&priv->lock);
		schedule_delayed_work(&priv->thermal_work, 0);
	} else if (!want_thermal && priv->mode == MODE_THERMAL) {
		priv->mode = MODE_STATIC;
		mutex_unlock(&priv->lock);
		cancel_delayed_work_sync(&priv->thermal_work);
	} else {
		mutex_unlock(&priv->lock);
	}

	return count;
}
static DEVICE_ATTR_RW(mode);

/* ============================================================
 * Sysfs Attribute: thermal_interval_ms
 * ============================================================
 * Path: /sys/bus/platform/devices/ps4-led/thermal_interval_ms
 *
 * thermal_interval_ms_show - Read current polling interval.
 * Returns the interval as a decimal string in milliseconds.
 */
static ssize_t thermal_interval_ms_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ps4_led_priv *priv = dev_get_drvdata(dev);
	unsigned int v;

	mutex_lock(&priv->lock);
	v = priv->thermal_interval_ms;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", v);
}

/**
 * thermal_interval_ms_store - Set polling interval.
 * @buf: Decimal string, milliseconds. Must be > 0.
 *
 * The new interval takes effect on the next worker reschedule.
 * The currently pending delay is not modified retroactively.
 */
static ssize_t thermal_interval_ms_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct ps4_led_priv *priv = dev_get_drvdata(dev);
	unsigned int val;

	if (kstrtouint(buf, 10, &val) || val == 0)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->thermal_interval_ms = val;
	mutex_unlock(&priv->lock);

	return count;
}
static DEVICE_ATTR_RW(thermal_interval_ms);

/*
 * Sysfs attribute group.
 * ATTRIBUTE_GROUPS() generates ps4_led_groups[], passed to
 * platform_driver.driver.dev_groups and registered automatically
 * by the driver core on probe — no sysfs_create_group() needed.
 */
static struct attribute *ps4_led_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_thermal_interval_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ps4_led);

/* ============================================================
 * ps4_led_probe - Initialize driver state and register LED nodes
 * ============================================================
 * @pdev: The platform device being bound.
 *
 * Allocates private state (devm-managed). Initializes the mutex
 * and delayed_work WITHOUT scheduling it. The thermal worker
 * starts only when user-space explicitly requests thermal mode,
 * satisfying the zero-overhead requirement in static mode.
 *
 * Returns 0 on success, -ENOMEM on allocation failure, or a
 * negative errno from devm_led_classdev_register().
 */
static int ps4_led_probe(struct platform_device *pdev)
{
	struct ps4_led_priv *priv;
	int i, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev                = pdev;
	priv->mode                = MODE_STATIC;
	priv->thermal_interval_ms = PS4_LED_THERMAL_INTERVAL_MS_DEFAULT;

	mutex_init(&priv->lock);
	INIT_DELAYED_WORK(&priv->thermal_work, ps4_thermal_work_fn);

	platform_set_drvdata(pdev, priv);

	for (i = 0; i < ARRAY_SIZE(ps4_led_nodes); i++) {
		ret = devm_led_classdev_register(&pdev->dev,
						 &ps4_led_nodes[i]);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to register LED node %d: %d\n",
				i, ret);
			return ret;
		}
	}

	dev_info(&pdev->dev,
		 "PS4 LED driver ready. "
		 "Write 'thermal' to mode attribute to enable thermal indicator.\n");

	return 0;
}

/* ============================================================
 * ps4_led_remove - Tear down driver state
 * ============================================================
 * @pdev: The platform device being unbound.
 *
 * Cancels any pending or in-progress thermal work synchronously.
 * This MUST happen before the device structure is freed; without
 * it, ps4_thermal_work_fn() could access freed memory.
 *
 * All devm-managed resources (priv, led_classdev registrations)
 * are released automatically after this function returns.
 */
static void ps4_led_remove(struct platform_device *pdev)
{
	struct ps4_led_priv *priv = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&priv->thermal_work);
}

/* ============================================================
 * Platform Driver Struct
 * ============================================================
 * dev_groups wires the sysfs attributes to the platform device
 * via the driver core — no manual sysfs_create_group() needed.
 */
static struct platform_driver ps4_led_driver = {
	.probe  = ps4_led_probe,
	.remove = ps4_led_remove,
	.driver = {
		.name       = "ps4-led",
		.dev_groups = ps4_led_groups,
	},
};

/* ============================================================
 * Module Init / Exit
 * ============================================================
 * Manual init/exit (not module_platform_driver) because we must
 * register both the driver and the platform_device from within
 * the module.
 *
 * Init : register driver first, then device (probe fires).
 * Exit : unregister device first (remove fires), then driver.
 */
static struct platform_device *ps4_led_pdev;

/**
 * ps4_led_init - Module entry point.
 *
 * Registers the platform driver, then the platform device.
 * On device registration failure, the driver is unregistered
 * before returning to leave the system in a clean state.
 */
static int __init ps4_led_init(void)
{
	int ret;

	ret = platform_driver_register(&ps4_led_driver);
	if (ret) {
		pr_err("ps4-led: failed to register platform driver: %d\n",
		       ret);
		return ret;
	}

	ps4_led_pdev = platform_device_register_simple("ps4-led", -1,
						       NULL, 0);
	if (IS_ERR(ps4_led_pdev)) {
		ret = PTR_ERR(ps4_led_pdev);
		pr_err("ps4-led: failed to register platform device: %d\n",
		       ret);
		platform_driver_unregister(&ps4_led_driver);
		ps4_led_pdev = NULL;
		return ret;
	}

	return 0;
}

/**
 * ps4_led_exit - Module exit point.
 *
 * Unregisters the platform device first (triggering ps4_led_remove
 * and synchronous work cancellation), then the driver.
 */
static void __exit ps4_led_exit(void)
{
	if (ps4_led_pdev)
		platform_device_unregister(ps4_led_pdev);

	platform_driver_unregister(&ps4_led_driver);
}

module_init(ps4_led_init);
module_exit(ps4_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("rmux <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 Aeolia front panel LED driver with Thermal Indicator Mode");
MODULE_ALIAS("platform:ps4-led");