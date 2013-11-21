/*
 * usb port device code
 *
 * Copyright (C) 2012 Intel Corp
 *
 * Author: Lan Tianyu <tianyu.lan@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <linux/slab.h>
#include <linux/pm_qos.h>

#include "hub.h"

static const struct attribute_group *port_dev_group[];

static ssize_t connect_type_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct usb_port *port_dev = to_usb_port(dev);
	char *result;

	switch (port_dev->connect_type) {
	case USB_PORT_CONNECT_TYPE_HOT_PLUG:
		result = "hotplug";
		break;
	case USB_PORT_CONNECT_TYPE_HARD_WIRED:
		result = "hardwired";
		break;
	case USB_PORT_NOT_USED:
		result = "not used";
		break;
	default:
		result = "unknown";
		break;
	}

	return sprintf(buf, "%s\n", result);
}

static ssize_t connect_type_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct usb_port *port_dev = to_usb_port(dev);
	ssize_t sz = len;
	int i;
	struct action { const char *str; enum usb_port_connect_type type; };
	static const struct action action[] = {
		{ .str = "hotplug", .type = USB_PORT_CONNECT_TYPE_HOT_PLUG, },
		{ .str = "hardwired", .type = USB_PORT_CONNECT_TYPE_HARD_WIRED },
	};

	if (buf[len-1] == '\n' || buf[len-1] == '\0')
		sz--;

	for (i = 0; i < ARRAY_SIZE(action); i++) {
		const struct action *act = &action[i];

		if (sz == strlen(act->str)
		    && strncmp(buf, act->str, sz) == 0) {
			pm_runtime_get_sync(&port_dev->dev);
			port_dev->connect_type = act->type;
			pm_runtime_put_sync(&port_dev->dev);
			return len;
		}
	}

	return -EINVAL;
}

static DEVICE_ATTR_RW(connect_type);

static struct attribute *port_dev_attrs[] = {
	&dev_attr_connect_type.attr,
	NULL,
};

static struct attribute_group port_dev_attr_grp = {
	.attrs = port_dev_attrs,
};

static const struct attribute_group *port_dev_group[] = {
	&port_dev_attr_grp,
	NULL,
};

static void usb_port_device_release(struct device *dev)
{
	struct usb_port *port_dev = to_usb_port(dev);

	cancel_work_sync(&port_dev->ratelimit_work);
	kfree(port_dev);
}

static void pm_ping_child(struct work_struct *w)
{
	struct usb_port *port_dev;
	struct usb_device *udev;

	port_dev = container_of(w, typeof(*port_dev), ratelimit_work);
	udev = usb_port_get_child(port_dev);
	if (udev) {
		pm_runtime_get_sync(&udev->dev);
		pm_runtime_put_autosuspend(&udev->dev);
	}
	usb_port_put_child(udev);
}

#ifdef CONFIG_PM_RUNTIME
static int usb_port_runtime_resume(struct device *dev)
{
	struct usb_port *port_dev = to_usb_port(dev);
	struct usb_device *hdev = to_usb_device(dev->parent->parent);
	struct usb_interface *intf = to_usb_interface(dev->parent);
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	int port1 = port_dev->portnum;
	int retval = 0;

	if (!hub)
		return -EINVAL;

	usb_autopm_get_interface(intf);
	if (test_bit(port1, hub->poweroff_bits))
		retval = usb_set_port_feature(hdev, port1, USB_PORT_FEAT_POWER);

	/* no child? we're done recovering this port, otherwise try to
	 * recover the device connection to rate limit power toggling
	 */
	if (!port_dev->child)
		usb_clear_port_poweroff(hub, port1);
	else
		schedule_work(&port_dev->ratelimit_work);
	usb_autopm_put_interface(intf);

	return retval;
}

static const char *power_on_reason(struct usb_port *port_dev)
{
	s32 flag = PM_QOS_FLAG_NO_POWER_OFF;
	const char *reason = NULL;
	struct usb_device *udev;

	if (dev_pm_qos_flags(&port_dev->dev, flag) == PM_QOS_FLAGS_ALL)
		return "pm_qos_no_power_off";

	if (port_dev->connect_type < USB_PORT_CONNECT_TYPE_HARD_WIRED)
		return "hotplug";

	udev = usb_port_get_child(port_dev);
	if (udev && udev->do_remote_wakeup)
		reason = "wakeup enabled";
	else if (udev && !udev->persist_enabled)
		reason = "persist disabled";
	usb_port_put_child(udev);

	return reason;
}

static int usb_port_runtime_suspend(struct device *dev)
{
	struct usb_port *port_dev = to_usb_port(dev);
	struct usb_device *hdev = to_usb_device(dev->parent->parent);
	struct usb_interface *intf = to_usb_interface(dev->parent);
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	int port1 = port_dev->portnum;
	int retval;

	flush_work(&port_dev->ratelimit_work);
	if (!hub)
		return -EINVAL;

	if (power_on_reason(port_dev))
		return 0;

	usb_autopm_get_interface(intf);
	usb_set_port_poweroff(hub, port1);
	retval = usb_clear_port_feature(hdev, port1, USB_PORT_FEAT_POWER);
	if (retval)
		usb_clear_port_poweroff(hub, port1);
	usb_autopm_put_interface(intf);

	return retval;
}
#endif

static const struct dev_pm_ops usb_port_pm_ops = {
#ifdef CONFIG_PM_RUNTIME
	.runtime_suspend =	usb_port_runtime_suspend,
	.runtime_resume =	usb_port_runtime_resume,
#endif
};

struct device_type usb_port_device_type = {
	.name =		"usb_port",
	.release =	usb_port_device_release,
	.pm =		&usb_port_pm_ops,
};

#ifdef CONFIG_PM
static ssize_t power_state_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	const char *state;
	struct usb_port *port_dev = to_usb_port(dev);
	struct usb_device *child = usb_port_get_child(port_dev);

	if (child) {
		pm_runtime_barrier(&child->dev);
		put_device(&child->dev);
	}

	pm_runtime_barrier(dev);

	if (pm_runtime_active(dev))
		state = "active";
	else
		state = power_on_reason(port_dev);

	if (!state)
		state = "off";

	return sprintf(buf, "%s\n", state);
}
static DEVICE_ATTR_RO(power_state);

static int add_port_power_state(struct usb_port *port_dev)
{
	return sysfs_add_file_to_group(&port_dev->dev.kobj,
				       &dev_attr_power_state.attr,
				       power_group_name);
}

static void remove_port_power_state(struct usb_port *port_dev)
{
	sysfs_remove_file_from_group(&port_dev->dev.kobj,
				     &dev_attr_power_state.attr,
				     power_group_name);
}
#else
static int add_port_power_state(struct usb_port *port_dev)
{
	return 0;
}

static void remove_port_power_state(struct usb_port *port_dev) { }
#endif

int usb_hub_create_port_device(struct usb_hub *hub, int port1)
{
	struct usb_port *port_dev = NULL;
	int retval;

	port_dev = kzalloc(sizeof(*port_dev), GFP_KERNEL);
	if (!port_dev) {
		retval = -ENOMEM;
		goto exit;
	}

	hub->ports[port1 - 1] = port_dev;
	port_dev->portnum = port1;
	port_dev->dev.parent = hub->intfdev;
	port_dev->dev.groups = port_dev_group;
	port_dev->dev.type = &usb_port_device_type;
	INIT_WORK(&port_dev->ratelimit_work, pm_ping_child);
	dev_set_name(&port_dev->dev, "port%d", port1);

	retval = device_register(&port_dev->dev);
	if (retval)
		goto error_register;

	add_port_power_state(port_dev);

	pm_runtime_set_active(&port_dev->dev);

	/* It would be dangerous if user space couldn't
	 * prevent usb device from being powered off. So don't
	 * enable port runtime pm if failed to expose port's pm qos.
	 */
	if (!dev_pm_qos_expose_flags(&port_dev->dev,
			PM_QOS_FLAG_NO_POWER_OFF))
		pm_runtime_enable(&port_dev->dev);

	device_enable_async_suspend(&port_dev->dev);
	return 0;

error_register:
	put_device(&port_dev->dev);
exit:
	return retval;
}

void usb_hub_remove_port_device(struct usb_hub *hub, int port1)
{
	struct usb_port *port_dev = hub->ports[port1 - 1];

	remove_port_power_state(port_dev);
	device_unregister(&port_dev->dev);
}
