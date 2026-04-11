// SPDX-License-Identifier: GPL-2.0-only
/*
 * PS4 Mesa kernel lock verification endpoint.
 *
 * Build-time contract:
 *   make PS4_MESA_LOCK_TOKEN=<token>
 *
 * Runtime contract:
 *   - Mesa opens /dev/ps4-mesa-lock for write.
 *   - Mesa writes its compiled-in token with no newline.
 *   - Kernel compares the supplied token against its compiled-in token.
 *   - On match: write succeeds and returns the supplied byte count.
 *   - On mismatch: write fails with -EPERM and dmesg shows:
 *       "ps4-mesa-lock: kernel mismatch"
 *
 * The token is never exposed back to user-space.
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "ps4-mesa-lock.h"

#define PS4_MESA_LOCK_MAX_INPUT 128

static bool ps4_mesa_lock_token_configured(void)
{
	return PS4_MESA_LOCK_TOKEN[0] != '\0';
}

static ssize_t ps4_mesa_lock_write(struct file *file, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	size_t token_len = strlen(PS4_MESA_LOCK_TOKEN);
	size_t cmp_len = len;
	char *kbuf;
	bool match;

	if (!ps4_mesa_lock_token_configured()) {
		pr_err_ratelimited("ps4-mesa-lock: kernel mismatch\n");
		return -ENODEV;
	}

	if (!len || len > PS4_MESA_LOCK_MAX_INPUT)
		return -EINVAL;

	kbuf = memdup_user_nul(buf, len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	if (cmp_len && kbuf[cmp_len - 1] == '\n')
		kbuf[--cmp_len] = '\0';

	match = cmp_len == token_len &&
		!memcmp(kbuf, PS4_MESA_LOCK_TOKEN, token_len);

	kfree(kbuf);

	if (!match) {
		pr_err_ratelimited("ps4-mesa-lock: kernel mismatch\n");
		return -EPERM;
	}

	return len;
}

static const struct file_operations ps4_mesa_lock_fops = {
	.owner = THIS_MODULE,
	.write = ps4_mesa_lock_write,
	.llseek = noop_llseek,
};

static struct miscdevice ps4_mesa_lock_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = PS4_MESA_LOCK_DEV_NAME,
	.fops = &ps4_mesa_lock_fops,
	.mode = 0222,
};

static int __init ps4_mesa_lock_init(void)
{
	int ret;

	ret = misc_register(&ps4_mesa_lock_miscdev);
	if (ret) {
		pr_err("ps4-mesa-lock: failed to register misc device: %d\n", ret);
		return ret;
	}

	if (!ps4_mesa_lock_token_configured())
		pr_warn("ps4-mesa-lock: built without PS4_MESA_LOCK_TOKEN; all verification requests will fail\n");
	else
		pr_info("ps4-mesa-lock: verification endpoint ready at /dev/%s\n",
			PS4_MESA_LOCK_DEV_NAME);

	return 0;
}

static void __exit ps4_mesa_lock_exit(void)
{
	misc_deregister(&ps4_mesa_lock_miscdev);
}

module_init(ps4_mesa_lock_init);
module_exit(ps4_mesa_lock_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Armandas Kvietkus");
MODULE_DESCRIPTION("PS4 Mesa kernel lock verification endpoint");
MODULE_ALIAS("misc:" PS4_MESA_LOCK_DEV_NAME);
