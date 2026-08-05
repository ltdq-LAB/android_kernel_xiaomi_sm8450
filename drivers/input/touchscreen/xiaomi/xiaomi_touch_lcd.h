/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __XIAOMI_TOUCH_LCD_H
#define __XIAOMI_TOUCH_LCD_H

#include "xiaomi_touch.h"

/* Keep shared symbol types identical; the pen Hall sensor is LCD-only state. */
#define ONESHOT_SENSOR_PEN_DETACH \
	((enum oneshot_sensor_type)ONESHOT_SENSOR_TYPE_NUM)
#define LCD_ONESHOT_SENSOR_TYPE_NUM (ONESHOT_SENSOR_TYPE_NUM + 1)

/**
 * update_pen_connect_strategy_value: - Publish the active stylus scan state.
 * @active: Whether normal active-mode stylus scanning is enabled.
 *
 * This mirrors Xiaomi's pen_connect_strategy notification ABI. It represents
 * the effective controller state, not merely a Bluetooth connection.
 */
int update_pen_connect_strategy_value(bool active);

#endif
