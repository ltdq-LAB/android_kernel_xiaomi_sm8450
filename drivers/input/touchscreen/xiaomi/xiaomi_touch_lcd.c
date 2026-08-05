// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi LCD touch interface for userspace gesture and stylus features.
 *
 * Copyright (C) 2024 The LineageOS Project
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "xiaomi_touch_lcd.h"

#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

static struct class *touch_class;
static struct device *touch_dev;

static struct xiaomi_touch_interface *touch_interface;
static DEFINE_MUTEX(interface_lock);

struct oneshot_sensor_attribute {
	struct device_attribute dev_attr;
	enum oneshot_sensor_type type;
};

/**
 * struct oneshot_sensor: - Internal representation of a oneshot sensor.
 * @status_name: Sysfs node name for reporting the sensor's status.
 * @enabled_attr: Sysfs attributes for enabled node.
 * @status_attr: Sysfs attributes for status node.
 * @mode: Touch-driver mode associated with this oneshot sensor.
 * @pending_event: Stores the event value (> 0 if an event is pending, 0 if cleared).
 *
 * A oneshot sensor is not a continuous-reading sensor.
 * Instead, it fires discrete events, such as a gesture occurrence.
 * The pending event value is cleared after the event is read
 * by user space.
 */
struct oneshot_sensor {
	char *status_name;
	struct oneshot_sensor_attribute enabled_attr;
	struct oneshot_sensor_attribute status_attr;
	enum touch_mode mode;
	atomic_t pending_event;
};

/*
 * Maps a oneshot sensor to its required attributes.
 * Used in generic methods to provide sensor specific options.
 */
static struct oneshot_sensor *oneshot_sensor_map[LCD_ONESHOT_SENSOR_TYPE_NUM];

/* Requested and successfully delivered gesture state for the LCD controller. */
static atomic_t oneshot_sensor_enabled_requested[LCD_ONESHOT_SENSOR_TYPE_NUM];
static atomic_t oneshot_sensor_enabled[LCD_ONESHOT_SENSOR_TYPE_NUM];

/* Event-delivery subscriptions are independent from persistent LCD modes. */
static atomic_t oneshot_sensor_subscribed[LCD_ONESHOT_SENSOR_TYPE_NUM];
static DEFINE_SPINLOCK(oneshot_sensor_event_lock);

/* Requested boolean high-performance profile, retained across client reloads. */
static atomic_t report_rate_requested = ATOMIC_INIT(0);

/*
 * Effective active-mode stylus scanning state. This is intentionally kept
 * separate from Bluetooth connection state: charging and the performance
 * profile can both make a connected stylus temporarily unavailable.
 */
static atomic_t pen_connect_strategy = ATOMIC_INIT(0);

struct touch_mode_attribute {
	struct device_attribute dev_attr;
	enum touch_mode touch_mode;
};

static void oneshot_sensor_apply_requested_locked(void);

static void oneshot_sensor_update_cache(enum touch_mode mode, int value)
{
	int i;
	unsigned long flags;

	for (i = 0; i < LCD_ONESHOT_SENSOR_TYPE_NUM; i++) {
		if (oneshot_sensor_map[i] &&
		    oneshot_sensor_map[i]->mode == mode) {
			struct oneshot_sensor *sensor = oneshot_sensor_map[i];
			bool changed =
				atomic_xchg(&oneshot_sensor_enabled_requested[i],
					    !!value) != !!value;

			atomic_set(&oneshot_sensor_enabled[i], !!value);
			if (!value) {
				spin_lock_irqsave(&oneshot_sensor_event_lock, flags);
				atomic_set(&sensor->pending_event, 0);
				spin_unlock_irqrestore(&oneshot_sensor_event_lock,
						       flags);
			}
			if (changed && touch_dev && !IS_ERR(touch_dev))
				sysfs_notify(&touch_dev->kobj, NULL,
					     sensor->enabled_attr.dev_attr.attr.name);
			return;
		}
	}
}

int register_xiaomi_touch_client(enum touch_id touch_id,
				 struct xiaomi_touch_interface *interface)
{
	int i;

	if (touch_id != TOUCH_ID_PRIMARY || !interface ||
	    !interface->set_mode_value || !interface->get_mode_value)
		return -EINVAL;

	mutex_lock(&interface_lock);
	if (touch_interface) {
		mutex_unlock(&interface_lock);
		return -EINVAL;
	}

	touch_interface = interface;
	/* A replacement client has not received any cached gesture modes. */
	for (i = 0; i < LCD_ONESHOT_SENSOR_TYPE_NUM; i++)
		atomic_set(&oneshot_sensor_enabled[i], 0);

	/* The LCD client may load after userspace published persistent modes. */
	oneshot_sensor_apply_requested_locked();
	interface->set_mode_value(interface->private, TOUCH_MODE_REPORT_RATE,
				  atomic_read(&report_rate_requested));
	mutex_unlock(&interface_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(register_xiaomi_touch_client);

int unregister_xiaomi_touch_client(enum touch_id touch_id)
{
	int i;

	if (touch_id != TOUCH_ID_PRIMARY)
		return -EINVAL;

	mutex_lock(&interface_lock);
	touch_interface = NULL;
	for (i = 0; i < LCD_ONESHOT_SENSOR_TYPE_NUM; i++)
		atomic_set(&oneshot_sensor_enabled[i], 0);
	mutex_unlock(&interface_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(unregister_xiaomi_touch_client);

int notify_oneshot_sensor(enum oneshot_sensor_type sensor_type, int value)
{
	struct oneshot_sensor *sensor;
	unsigned long flags;
	bool deliver = false;

	if ((unsigned int)sensor_type >= LCD_ONESHOT_SENSOR_TYPE_NUM ||
	    !oneshot_sensor_map[sensor_type]) {
		pr_err("tried to notify for invalid oneshot sensor %d\n",
		       sensor_type);
		return -EINVAL;
	}
	sensor = oneshot_sensor_map[sensor_type];
	spin_lock_irqsave(&oneshot_sensor_event_lock, flags);
	if (atomic_read(&oneshot_sensor_subscribed[sensor_type])) {
		atomic_set(&sensor->pending_event, value);
		deliver = true;
	}
	spin_unlock_irqrestore(&oneshot_sensor_event_lock, flags);

	if (!deliver) {
		pr_debug("gesture of type %d ignored without a subscriber\n",
			 sensor_type);
		return 0;
	}

	sysfs_notify(&touch_dev->kobj, NULL, sensor->status_name);

	return 0;
}
EXPORT_SYMBOL_GPL(notify_oneshot_sensor);

/**
 * oneshot_sensor_apply_requested_locked: - Commit requested gesture states.
 * Caller must hold interface_lock.
 */
static void oneshot_sensor_apply_requested_locked(void)
{
	int i;
	struct xiaomi_touch_interface *interface;

	interface = touch_interface;
	if (!interface || !interface->set_mode_value)
		return;

	for (i = 0; i < LCD_ONESHOT_SENSOR_TYPE_NUM; i++) {
		struct oneshot_sensor *sensor = oneshot_sensor_map[i];
		int requested_value;
		int ret;

		if (!sensor)
			continue;

		requested_value =
			atomic_read(&oneshot_sensor_enabled_requested[i]);
		if (atomic_read(&oneshot_sensor_enabled[i]) != requested_value) {
			pr_debug("setting mode %d to %d!\n", i,
				 requested_value);
			ret = interface->set_mode_value(
				interface->private, sensor->mode, requested_value);
			if (!ret)
				atomic_set(&oneshot_sensor_enabled[i], requested_value);
			else
				pr_warn("failed to set mode %d to %d: %d\n", i,
					requested_value, ret);
		}
	}
}

static ssize_t oneshot_sensor_status_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct oneshot_sensor_attribute *sensor_attribute =
		container_of(attr, struct oneshot_sensor_attribute, dev_attr);
	struct oneshot_sensor *sensor =
		oneshot_sensor_map[sensor_attribute->type];
	unsigned long flags;
	int value = 0;

	/* Reading consumes an event only while userspace remains subscribed. */
	spin_lock_irqsave(&oneshot_sensor_event_lock, flags);
	if (atomic_read(&oneshot_sensor_subscribed[sensor_attribute->type]))
		value = atomic_xchg(&sensor->pending_event, 0);
	else
		atomic_set(&sensor->pending_event, 0);
	spin_unlock_irqrestore(&oneshot_sensor_event_lock, flags);

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t oneshot_sensor_enabled_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct oneshot_sensor_attribute *sensor_attribute =
		container_of(attr, struct oneshot_sensor_attribute, dev_attr);

	return snprintf(
		buf, PAGE_SIZE, "%d\n",
		atomic_read(&oneshot_sensor_enabled_requested[sensor_attribute
								      ->type]));
}

static ssize_t oneshot_sensor_enabled_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *arg, size_t count)
{
	struct oneshot_sensor_attribute *sensor_attribute =
		container_of(attr, struct oneshot_sensor_attribute, dev_attr);
	struct oneshot_sensor *sensor =
		oneshot_sensor_map[sensor_attribute->type];
	bool changed;
	unsigned int enable;
	unsigned long flags;

	if (kstrtouint(arg, 10, &enable))
		return -EINVAL;

	// only boolean input is allowed
	if (enable > 1)
		return -EINVAL;

	/* Pen Hall retains the legacy combined mode/subscription ABI. */
	if (sensor_attribute->type == ONESHOT_SENSOR_PEN_DETACH) {
		spin_lock_irqsave(&oneshot_sensor_event_lock, flags);
		atomic_set(&oneshot_sensor_subscribed[sensor_attribute->type],
			   enable);
		if (!enable)
			atomic_set(&sensor->pending_event, 0);
		spin_unlock_irqrestore(&oneshot_sensor_event_lock, flags);
	}

	mutex_lock(&interface_lock);
	changed = atomic_xchg(
			  &oneshot_sensor_enabled_requested[sensor_attribute->type],
			  enable) != enable;
	if (changed ||
	    atomic_read(&oneshot_sensor_enabled[sensor_attribute->type]) != enable) {
		/*
		 * Commit the request synchronously while the LCD controller is
		 * available; its driver owns power-transition handling.
		 */
		oneshot_sensor_apply_requested_locked();
	}
	mutex_unlock(&interface_lock);

	if (changed)
		sysfs_notify(&dev->kobj, NULL, attr->attr.name);

	return count;
}

static ssize_t oneshot_sensor_subscribed_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct oneshot_sensor_attribute *sensor_attribute =
		container_of(attr, struct oneshot_sensor_attribute, dev_attr);

	return sysfs_emit(buf, "%d\n",
			  atomic_read(&oneshot_sensor_subscribed[sensor_attribute
								 ->type]));
}

static ssize_t oneshot_sensor_subscribed_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *arg, size_t count)
{
	struct oneshot_sensor_attribute *sensor_attribute =
		container_of(attr, struct oneshot_sensor_attribute, dev_attr);
	struct oneshot_sensor *sensor =
		oneshot_sensor_map[sensor_attribute->type];
	unsigned int subscribe;
	bool changed;
	unsigned long flags;

	if (kstrtouint(arg, 10, &subscribe))
		return -EINVAL;
	if (subscribe > 1)
		return -EINVAL;

	spin_lock_irqsave(&oneshot_sensor_event_lock, flags);
	changed = atomic_xchg(&oneshot_sensor_subscribed[sensor_attribute->type],
			      subscribe) != subscribe;
	if (!subscribe)
		atomic_set(&sensor->pending_event, 0);
	spin_unlock_irqrestore(&oneshot_sensor_event_lock, flags);
	if (changed)
		sysfs_notify(&dev->kobj, NULL, attr->attr.name);

	return count;
}

#define ONESHOT_SUBSCRIPTION_ATTR(_varname, _name, _type)                     \
	static struct oneshot_sensor_attribute _varname = {                    \
		.dev_attr = __ATTR(_name, 0644, oneshot_sensor_subscribed_show, \
				  oneshot_sensor_subscribed_store),              \
		.type = _type,                                                 \
	}

/**
 * ONESHOT_SENSOR: - Declares a oneshot sensor including related sysfs attributes.
 * @_varname: A struct oneshot_sensor with this name is declared.
 * @_enabled: Name of the sensor enable sysfs attribute.
 * @_status: status_name of struct oneshot_sensor.
 * @_type: enum oneshot_sensor_type used to find oneshot_sensors in sensors_map.
 * @_mode: enum touch_mode for communication with the touchscreen driver.
 */
#define ONESHOT_SENSOR(_varname, _enabled, _status, _type, _mode)              \
	struct oneshot_sensor _varname = {                                     \
		.status_name = #_status,                                       \
		.enabled_attr = { __ATTR(_enabled, 0644,                       \
					 oneshot_sensor_enabled_show,          \
					 oneshot_sensor_enabled_store),        \
				  _type },                                     \
		.status_attr = { __ATTR(_status, 0444,                         \
					oneshot_sensor_status_show, NULL),     \
				 _type },                                      \
		.mode = _mode,                                                 \
		.pending_event = ATOMIC_INIT(0),                               \
	}

static ONESHOT_SENSOR(single_tap_sensor, gesture_single_tap_enabled,
		      gesture_single_tap_state, ONESHOT_SENSOR_SINGLE_TAP,
		      TOUCH_MODE_SINGLETAP_GESTURE);
static ONESHOT_SENSOR(double_tap_sensor, gesture_double_tap_enabled,
		      gesture_double_tap_state, ONESHOT_SENSOR_DOUBLE_TAP,
		      TOUCH_MODE_DOUBLETAP_GESTURE);
static ONESHOT_SENSOR(pen_detach_sensor, gesture_pen_detach_enabled,
		      gesture_pen_detach_state, ONESHOT_SENSOR_PEN_DETACH,
		      TOUCH_MODE_PEN_DETACH_GESTURE);

ONESHOT_SUBSCRIPTION_ATTR(single_tap_subscription_attr,
			  gesture_single_tap_subscribed,
			  ONESHOT_SENSOR_SINGLE_TAP);
ONESHOT_SUBSCRIPTION_ATTR(double_tap_subscription_attr,
			  gesture_double_tap_subscribed,
			  ONESHOT_SENSOR_DOUBLE_TAP);

static struct attribute *oneshot_sensor_attrs[] = {
	&single_tap_sensor.enabled_attr.dev_attr.attr,
	&single_tap_subscription_attr.dev_attr.attr,
	&single_tap_sensor.status_attr.dev_attr.attr,
	&double_tap_sensor.enabled_attr.dev_attr.attr,
	&double_tap_subscription_attr.dev_attr.attr,
	&double_tap_sensor.status_attr.dev_attr.attr,
	&pen_detach_sensor.enabled_attr.dev_attr.attr,
	&pen_detach_sensor.status_attr.dev_attr.attr,
	NULL,
};

static const struct attribute_group oneshot_sensor_group = {
	// name defaults to NULL (device name will be used)
	.attrs = oneshot_sensor_attrs,
};

static int touch_mode_get(enum touch_mode mode)
{
	struct xiaomi_touch_interface *interface;
	int ret;

	if ((unsigned int)mode >= TOUCH_MODE_NUM)
		return -EINVAL;

	mutex_lock(&interface_lock);
	interface = touch_interface;
	if (!interface || !interface->get_mode_value)
		ret = -EFAULT;
	else
		ret = interface->get_mode_value(interface->private, mode);
	mutex_unlock(&interface_lock);

	return ret;
}

static int touch_mode_set(enum touch_mode mode, int value)
{
	struct xiaomi_touch_interface *interface;
	int ret;

	if ((unsigned int)mode >= TOUCH_MODE_NUM)
		return -EINVAL;

	mutex_lock(&interface_lock);
	interface = touch_interface;
	if (!interface || !interface->set_mode_value) {
		mutex_unlock(&interface_lock);
		return -EFAULT;
	}

	ret = interface->set_mode_value(interface->private, mode, value);
	if (mode == TOUCH_MODE_REPORT_RATE && (value == 0 || value == 1))
		atomic_set(&report_rate_requested, value);
	if (!ret)
		oneshot_sensor_update_cache(mode, value);
	mutex_unlock(&interface_lock);

	return ret;
}

static ssize_t pen_connect_strategy_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&pen_connect_strategy));
}

static DEVICE_ATTR_RO(pen_connect_strategy);

int update_pen_connect_strategy_value(bool active)
{
	char *envp[2];
	int ret;

	envp[0] = "SOURCE=sysfs";
	envp[1] = NULL;
	atomic_set(&pen_connect_strategy, !!active);

	if (!touch_dev || IS_ERR(touch_dev))
		return 0;

	ret = kobject_uevent_env(&touch_dev->kobj, KOBJ_CHANGE, envp);
	sysfs_notify(&touch_dev->kobj, NULL, "pen_connect_strategy");

	return ret;
}
EXPORT_SYMBOL_GPL(update_pen_connect_strategy_value);

static ssize_t touch_mode_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct touch_mode_attribute *mode_attribute =
		container_of(attr, struct touch_mode_attribute, dev_attr);
	int value;

	value = touch_mode_get(mode_attribute->touch_mode);
	if (value < 0)
		return value;

	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t touch_mode_store(struct device *dev,
				struct device_attribute *attr, const char *arg,
				size_t count)
{
	struct touch_mode_attribute *mode_attribute =
		container_of(attr, struct touch_mode_attribute, dev_attr);
	unsigned int value;
	int set_success;

	if (kstrtouint(arg, 10, &value))
		return -EINVAL;

	set_success = touch_mode_set(mode_attribute->touch_mode, value);
	if (set_success < 0)
		return set_success;

	return count;
}

/**
 * TOUCH_MODE_ATTR_RW - Define a read-write touch mode attribute.
 * @_name: Attribute name.
 * @_mode: Value of enum touch_mode corresponding to this attribute.
 *
 * Convenience macro for defining a struct touch_mode_attribute
 */
#define TOUCH_MODE_ATTR_RW(_name, _mode)                                       \
	struct touch_mode_attribute touch_mode_attr_##_name = {                \
		.dev_attr = __ATTR(_name, 0644, touch_mode_show,               \
				   touch_mode_store),                          \
		.touch_mode = _mode,                                           \
	}

TOUCH_MODE_ATTR_RW(bump_sample_rate, TOUCH_MODE_REPORT_RATE);

static struct attribute *touch_mode_attrs[] = {
	&touch_mode_attr_bump_sample_rate.dev_attr.attr,
	&dev_attr_pen_connect_strategy.attr,
	NULL,
};

static const struct attribute_group touch_mode_group = {
	// name defaults to NULL (device name will be used)
	.attrs = touch_mode_attrs,
};

static const struct attribute_group *touch_attr_groups[] = {
	&oneshot_sensor_group,
	&touch_mode_group,
	NULL,
};

static long xiaomi_touch_dev_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct touch_mode_request request;
	void __user *argp = (void __user *)arg;
	int retval = 0;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;

	pr_debug("cmd: %d, mode: %d, value: %d\n", _IOC_NR(cmd), request.mode,
		 request.value);

	switch (cmd) {
	case TOUCH_IOC_SET_CUR_VALUE:
		retval = touch_mode_set(request.mode, request.value);
		break;
	case TOUCH_IOC_GET_CUR_VALUE:
		request.value = touch_mode_get(request.mode);
		if (request.value < 0)
			retval = request.value;
		else if (copy_to_user(argp, &request, sizeof(request)))
			retval = -EFAULT;
		break;
	default:
		retval = -EINVAL;
	}

	return retval;
}

static const struct file_operations xiaomitouch_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = xiaomi_touch_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = xiaomi_touch_dev_ioctl,
#endif
};

static struct miscdevice misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xiaomi-touch",
	.fops = &xiaomitouch_dev_fops,
};

static int __init xiaomi_touch_init(void)
{
	int ret = 0;

	oneshot_sensor_map[ONESHOT_SENSOR_SINGLE_TAP] = &single_tap_sensor;
	oneshot_sensor_map[ONESHOT_SENSOR_DOUBLE_TAP] = &double_tap_sensor;
	oneshot_sensor_map[ONESHOT_SENSOR_PEN_DETACH] = &pen_detach_sensor;

	ret = misc_register(&misc_dev);
	if (ret) {
		pr_err("failed to register misc device, err :%d\n", ret);
		return ret;
	}

	touch_class = class_create(THIS_MODULE, "touch");
	if (IS_ERR(touch_class)) {
		ret = PTR_ERR(touch_class);
		touch_class = NULL;
		pr_err("failed to create class: %d\n", ret);
		goto class_create_err;
	}

	touch_dev =
		device_create_with_groups(touch_class, NULL, MKDEV(0, 0), NULL,
					  touch_attr_groups, "touch_dev");
	if (IS_ERR(touch_dev)) {
		ret = PTR_ERR(touch_dev);
		touch_dev = NULL;
		pr_err("failed to create device with sysfs group: %d\n", ret);
		goto device_create_err;
	}

	return ret;

device_create_err:
	class_destroy(touch_class);
class_create_err:
	misc_deregister(&misc_dev);
	return ret;
}

static void __exit xiaomi_touch_exit(void)
{
	device_unregister(touch_dev);
	class_destroy(touch_class);
	misc_deregister(&misc_dev);
}

module_init(xiaomi_touch_init);
module_exit(xiaomi_touch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("The LineageOS Project");
MODULE_DESCRIPTION("User space interface for LCD TDDI touch features");
