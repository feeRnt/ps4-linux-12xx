/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PS4_MESA_LOCK_H_
#define _PS4_MESA_LOCK_H_

#ifndef PS4_MESA_LOCK_TOKEN
#define PS4_MESA_LOCK_TOKEN ""
#endif

#define PS4_MESA_LOCK_DEV_NAME "ps4-mesa-lock"

/*
 * User-space contract:
 *   open("/dev/ps4-mesa-lock", O_WRONLY | O_CLOEXEC)
 *   write(fd, token, strlen(token))
 */

#endif /* _PS4_MESA_LOCK_H_ */
