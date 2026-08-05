/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef __UAPI__XIAOMI__TOUCH_H
#define __UAPI__XIAOMI__TOUCH_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * enum touch_mode: - Defines various modes supported by the touchscreen driver.
 * @TOUCH_MODE_SINGLETAP_GESTURE: Enables or disables the single-tap gesture.
 * @TOUCH_MODE_DOUBLETAP_GESTURE: Enables or disables the double-tap gesture.
 * @TOUCH_MODE_FOD_PRESS_GESTURE: Enables or disabled the fingerprint-on-display press gesture.
 * @TOUCH_MODE_FOD_FINGER_STATE: Sysfs node that just reports what it gets told from userspace.
 * @TOUCH_MODE_NONUI_MODE: Disables or enables currently enabled gestures.
 * @TOUCH_MODE_REPORT_RATE: Enables or disables the high-performance touch
 *                          sampling profile.
 * @TOUCH_MODE_FOLD_STATUS: Informs the xiaomi touch driver about current fold status.
 * @TOUCH_MODE_STYLUS_CONNECTION: Reports a SET-only stylus connection event.
 * @TOUCH_MODE_PEN_DETACH_GESTURE: Enables or disables the optional LineageOS
 *                                  wake gesture for a real aggregated pen Hall
 *                                  state edge (attach or detach).
 * @TOUCH_MODE_NUM: Represents the total number of supported modes.
 *
 * This enumeration is used to identify modes when configuring or querying
 * the touchscreen driver via IOCTL commands.
 */
enum touch_mode {
	TOUCH_MODE_SINGLETAP_GESTURE = 0,
	TOUCH_MODE_DOUBLETAP_GESTURE = 1,
	TOUCH_MODE_FOD_PRESS_GESTURE = 2,
	TOUCH_MODE_FOD_FINGER_STATE = 3,
	TOUCH_MODE_NONUI_MODE = 4,
	TOUCH_MODE_REPORT_RATE = 5,
	TOUCH_MODE_FOLD_STATUS = 6,
	TOUCH_MODE_STYLUS_CONNECTION = 7,
	TOUCH_MODE_PEN_DETACH_GESTURE = 8,
	TOUCH_MODE_NUM,
};

/**
 * SET-only stylus connection event encoding for
 * TOUCH_MODE_STYLUS_CONNECTION. GET is not defined for this mode.
 *
 * The low nibble contains the stock DATA_MODE_20 stylus type. The exact
 * liuqin OS3 driver uses K81P (1) as a legacy stylus which shields the pen
 * path and M81P (2) as its counted stylus. The stock HIDL clamps connected
 * framework types 3..7 to M81P; their disconnected values leave the count
 * unchanged but still run the driver's common release/status-update tail. Bit
 * 4 set means connected and clear means disconnected. RESET matches the
 * original liuqin module: it clears only the supported-pen counter and leaves
 * the legacy shield unchanged. The original connection test is count != 0,
 * even after an unmatched disconnect makes the count negative. These are
 * non-idempotent delta events; producers should follow the stock startup
 * sequence of RESET followed by a replay of the currently connected devices,
 * rather than retrying one CONNECT event.
 */
#define TOUCH_STYLUS_CONNECTION_RESET		(-1)
#define TOUCH_STYLUS_CONNECTION_TYPE_MASK	0x0f
#define TOUCH_STYLUS_CONNECTION_CONNECTED	0x10
#define TOUCH_STYLUS_TYPE_LIUQIN_LEGACY_K81P	0x01
#define TOUCH_STYLUS_TYPE_LIUQIN_SUPPORTED_M81P	0x02

/* Deprecated source aliases; use the model-specific constants above. */
#define TOUCH_STYLUS_TYPE_LIUQIN_LEGACY \
	TOUCH_STYLUS_TYPE_LIUQIN_LEGACY_K81P
#define TOUCH_STYLUS_TYPE_LIUQIN_SUPPORTED \
	TOUCH_STYLUS_TYPE_LIUQIN_SUPPORTED_M81P

/**
 * enum touch_mode_cmd: - Defines commands for interacting with touchscreen modes.
 * @TOUCH_MODE_SET: Sets the current value for the specified mode.
 * @TOUCH_MODE_GET: Retrieves the current value of the specified mode.
 *
 * These commands are used with the IOCTL interface to configure or query
 * touchscreen driver modes.
 */
enum touch_mode_cmd {
	TOUCH_MODE_SET,
	TOUCH_MODE_GET,
};

/**
 * struct touch_mode_request: - Represents a request to set or get a mode value.
 * @mode: The mode to configure or query (see enum touch_mode).
 * @value: The value to set or retrieve for the mode.
 *
 * This structure is passed between user space and kernel space through
 * IOCTL commands for touchscreen mode configuration.
 */
struct touch_mode_request {
	enum touch_mode mode;
	int value;
};

/**
 * enum touch_fold_status: - Represents the fold status.
 * @TOUCH_FOLD_STATUS_UNFOLDED: Fold status where the primary touchscreen is active.
 * @TOUCH_FOLD_STATUS_FOLDED: Fold status where the secondary touchscreen is active.
 *
 * These are the supported values for TOUCH_MODE_FOLD_STATUS requests.
 */
enum touch_fold_status {
	TOUCH_FOLD_STATUS_UNFOLDED,
	TOUCH_FOLD_STATUS_FOLDED,
	TOUCH_FOLD_STATUS_NUM,
};

/*
 * IOCTL definitions for touchscreen configuration.
 * Used by user space applications to communicate with the kernel driver.
 */

/**
 * TOUCH_IOC_SET_CUR_VALUE: - IOCTL command to set the value of a mode.
 * Expects a struct touch_mode_request containing the mode and value.
 */
#define TOUCH_IOC_SET_CUR_VALUE                                                \
	_IOW('T', TOUCH_MODE_SET, struct touch_mode_request)

/**
 * TOUCH_IOC_GET_CUR_VALUE: - IOCTL command to get the value of a mode.
 * Expects a struct touch_mode_request and fills its value field with the
 * current mode value.
 */
#define TOUCH_IOC_GET_CUR_VALUE                                                \
	_IOR('T', TOUCH_MODE_GET, struct touch_mode_request)

#endif /* __UAPI__XIAOMI__TOUCH_H */
