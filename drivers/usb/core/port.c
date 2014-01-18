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

static DEFINE_MUTEX(peer_lock);
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
static DEVICE_ATTR_RO(connect_type);

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

	kfree(port_dev);
}

#ifdef CONFIG_PM_RUNTIME
static int usb_port_runtime_resume(struct device *dev)
{
	struct usb_port *port_dev = to_usb_port(dev);
	struct usb_device *hdev = to_usb_device(dev->parent->parent);
	struct usb_interface *intf = to_usb_interface(dev->parent);
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	struct usb_port *peer = port_dev->peer;
	int port1 = port_dev->portnum;
	int retval;

	if (!hub)
		return -EINVAL;

	/*
	 * Power on our usb3 peer before this usb2 port to prevent a usb3
	 * device from degrading to its usb2 connection
	 */
	if (!hub_is_superspeed(hdev) && peer)
		pm_runtime_get_sync(&peer->dev);

	usb_autopm_get_interface(intf);
	retval = usb_hub_set_port_power(hdev, hub, port1, true);
	if (port_dev->child && !retval) {
		/*
		 * Attempt to wait for usb hub port to be reconnected in order
		 * to make the resume procedure successful.  The device may have
		 * disconnected while the port was powered off, so ignore the
		 * return status.
		 */
		retval = hub_port_debounce_be_connected(hub, port1);
		if (retval < 0)
			dev_dbg(&port_dev->dev, "can't get reconnection after setting port  power on, status %d\n",
					retval);
		retval = 0;
	}

	usb_autopm_put_interface(intf);

	return retval;
}

static int usb_port_runtime_suspend(struct device *dev)
{
	struct usb_port *port_dev = to_usb_port(dev);
	struct usb_device *hdev = to_usb_device(dev->parent->parent);
	struct usb_interface *intf = to_usb_interface(dev->parent);
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	struct usb_port *peer = port_dev->peer;
	int port1 = port_dev->portnum;
	int retval;

	if (!hub)
		return -EINVAL;

	if (dev_pm_qos_flags(&port_dev->dev, PM_QOS_FLAG_NO_POWER_OFF)
			== PM_QOS_FLAGS_ALL)
		return -EAGAIN;

	usb_autopm_get_interface(intf);
	retval = usb_hub_set_port_power(hdev, hub, port1, false);
	usb_clear_port_feature(hdev, port1, USB_PORT_FEAT_C_CONNECTION);
	if (!hub_is_superspeed(hdev))
		usb_clear_port_feature(hdev, port1, USB_PORT_FEAT_C_ENABLE);
	usb_autopm_put_interface(intf);

	/*
	 * Our peer usb3 port may now be able to suspend, asynchronously
	 * queue a suspend request to observe that this usb2 peer port
	 * is now off.
	 */
	if (!hub_is_superspeed(hdev) && peer)
		pm_runtime_put(&peer->dev);

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

/*
 * Set the default peer port for root hubs.  Platform firmware will have
 * already set the peer if tier-mismatch is present.  Assumes the
 * primary_hcd is registered first
 */
static struct usb_port *find_default_peer(struct usb_hub *hub, int port1)
{
	struct usb_device *hdev = hub ? hub->hdev : NULL;
	struct usb_device *peer_hdev;
	struct usb_port *peer = NULL;
	struct usb_hub *peer_hub;

	if (!hub)
		return NULL;

	if (!hdev->parent) {
		struct usb_hcd *hcd = bus_to_hcd(hdev->bus);
		struct usb_hcd *peer_hcd = hcd->primary_hcd;

		if (!peer_hcd || hcd == peer_hcd)
			return NULL;

		peer_hdev = peer_hcd->self.root_hub;
		peer_hub = usb_hub_to_struct_hub(peer_hdev);
		if (peer_hub && port1 <= peer_hdev->maxchild)
			peer = peer_hub->ports[port1 - 1];
	} else {
		struct usb_port *upstream;
		struct usb_device *parent = hdev->parent;
		struct usb_hub *parent_hub = usb_hub_to_struct_hub(parent);

		if (!parent_hub)
			return NULL;

		upstream = parent_hub->ports[hdev->portnum - 1];
		if (!upstream->peer)
			return NULL;

		peer_hdev = upstream->peer->child;
		peer_hub = usb_hub_to_struct_hub(peer_hdev);
		if (!peer_hub || port1 > peer_hdev->maxchild)
			return NULL;

		peer = peer_hub->ports[port1 - 1];
	}

	return peer;
}

/*
 * Modifying ->peer affects usb_port_runtime_{suspend|resume} so make
 * sure devices are active before the change and re-evaluate
 * afterwards
 */
static void pre_modify_peers(struct usb_port *left, struct usb_port *right)
{
	get_device(&left->dev);
	get_device(&right->dev);
	pm_runtime_get_sync(&left->dev);
	pm_runtime_get_sync(&right->dev);
}

static void post_modify_peers(struct usb_port *left, struct usb_port *right)
{
	pm_runtime_put(&left->dev);
	pm_runtime_put(&right->dev);
	put_device(&left->dev);
	put_device(&right->dev);
}

static int link_peers(struct usb_port *left, struct usb_port *right)
{
	struct usb_device *ldev, *rdev;
	int rc;

	if (left->peer == right && right->peer == left)
		return 0;

	if (left->peer || right->peer) {
		struct usb_port *lpeer = left->peer;
		struct usb_port *rpeer = right->peer;

		WARN(1, "failed to peer %s and %s (%s -> %s) (%s -> %s)\n",
			dev_name(&left->dev), dev_name(&right->dev),
			dev_name(&left->dev),
			lpeer ? dev_name(&lpeer->dev) : "[none]",
			dev_name(&right->dev),
			rpeer ? dev_name(&rpeer->dev) : "[none]");
		return -EBUSY;
	}

	rc = sysfs_create_link(&left->dev.kobj, &right->dev.kobj, "peer");
	if (rc)
		return rc;
	rc = sysfs_create_link(&right->dev.kobj, &left->dev.kobj, "peer");
	if (rc) {
		sysfs_remove_link(&left->dev.kobj, "peer");
		return rc;
	}

	pre_modify_peers(left, right);
	get_device(&right->dev);
	left->peer = right;
	get_device(&left->dev);
	right->peer = left;

	/*
	 * Ports are peer linked, hold a reference on the superspeed
	 * port which the hispeed port drops when it suspends.  This
	 * ensures that superspeed ports only suspend after their
	 * hispeed peer.
	 */
	ldev = to_usb_device(left->dev.parent->parent);
	rdev = to_usb_device(right->dev.parent->parent);
	if (hub_is_superspeed(ldev))
		pm_runtime_get_noresume(&left->dev);
	else {
		WARN_ON(!hub_is_superspeed(rdev));
		pm_runtime_get_noresume(&right->dev);
	}
	post_modify_peers(left, right);

	return 0;
}

static void link_peers_report(struct usb_port *left, struct usb_port *right)
{
	int rc;

	rc = link_peers(left, right);
	if (rc == 0) {
		dev_dbg(&left->dev, "peered to %s\n", dev_name(&right->dev));
	} else {
		dev_warn(&left->dev, "failed to peer to %s (%d)\n",
				dev_name(&right->dev), rc);
		pr_warn_once("usb: port power management may be unreliable\n");
	}
}

static void unlink_peers(struct usb_port *left, struct usb_port *right)
{
	struct usb_device *ldev, *rdev;

	WARN(right->peer != left || left->peer != right,
			"%s and %s are not peers?\n",
			dev_name(&left->dev), dev_name(&right->dev));

	pre_modify_peers(left, right);
	sysfs_remove_link(&left->dev.kobj, "peer");
	put_device(&left->dev);
	right->peer = NULL;
	sysfs_remove_link(&right->dev.kobj, "peer");
	put_device(&right->dev);
	left->peer = NULL;

	/*
	 * Ports are no longer peer linked, drop the reference that
	 * keeps the superspeed port (may be 'right' or 'left') powered
	 * when its peer is active
	 */
	ldev = to_usb_device(left->dev.parent->parent);
	rdev = to_usb_device(right->dev.parent->parent);
	if (hub_is_superspeed(ldev))
		pm_runtime_put_noidle(&left->dev);
	else {
		WARN_ON(!hub_is_superspeed(rdev));
		pm_runtime_put_noidle(&right->dev);
	}
	post_modify_peers(left, right);
}

/**
 * for_each_child_port() - invoke 'fn' on all usb_port instances beneath 'hdev'
 * @hdev: potential hub usb_device (validated by usb_hub_to_struct_hub)
 * @level: track recursion level to stop after reaching USB spec max depth
 * @p: parameter to pass to 'fn'
 * @fn: routine to invoke on each port
 *
 * Recursively iterate all ports (depth-first) beneath 'hdev' until 'fn'
 * returns a non-NULL usb_port or all ports have been visited.
 */
static struct usb_port *for_each_child_port(struct usb_device *hdev, int level,
		void *p, struct usb_port * (*fn)(struct usb_port *, void *))
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	int port1;

#define MAX_HUB_DEPTH 5
	if (!hub || level > MAX_HUB_DEPTH)
		return NULL;

	level++;
	for (port1 = 1; port1 <= hdev->maxchild; port1++) {
		struct usb_port *ret, *port_dev = hub->ports[port1 - 1];

		ret = fn(port_dev, p);
		if (ret)
			return ret;
		ret = for_each_child_port(port_dev->child, level, p, fn);
		if (ret)
			return ret;
	}

	return NULL;
}

static struct usb_port *do_match_location(struct usb_port *port_dev, void *_loc)
{
	struct usb_port_location *loc = _loc;

	if (memcmp(&port_dev->location, loc, sizeof(*loc)) == 0)
		return port_dev;
	return NULL;
}

static struct usb_port *find_port_by_location(struct usb_device *hdev,
		struct usb_port_location *loc)
{
	return for_each_child_port(hdev, 1, loc, do_match_location);
}

static struct usb_port *do_default_link(struct usb_port *port_dev, void *p)
{
	struct usb_device *hdev = to_usb_device(port_dev->dev.parent->parent);
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	struct usb_port *peer;

	peer = find_default_peer(hub, port_dev->portnum);

	/*
	 * Assign the peer, but since we may have run
	 * enumerate_dependent_peers() on the peer bus it may already be
	 * set
	 */
	if (peer && !port_dev->peer)
		link_peers_report(port_dev, peer);
	return NULL;
}

static void enumerate_dependent_peers(struct usb_device *hdev)
{
	for_each_child_port(hdev, 1, NULL, do_default_link);
}

static struct usb_port *do_unlink_peers(struct usb_port *port_dev, void *p)
{
	if (port_dev->peer)
		unlink_peers(port_dev, port_dev->peer);
	return NULL;
}

static void invalidate_dependent_peers(struct usb_port *port_dev)
{
	unlink_peers(port_dev, port_dev->peer);
	for_each_child_port(port_dev->child, 1, NULL, do_unlink_peers);
}

void usb_set_hub_port_location(struct usb_device *hdev, int port1,
		u32 cookie)
{
	struct usb_hub *hub = usb_hub_to_struct_hub(hdev);
	struct usb_hcd *hcd = bus_to_hcd(hdev->bus);
	struct usb_hcd *peer_hcd = hcd->shared_hcd;
	int enum_port_dev = 0, enum_peer = 0;
	struct usb_port *peer, *port_dev;
	struct usb_device *peer_hdev;

	if (cookie == 0)
		return;

	if (!hub)
		return;

	port_dev = hub->ports[port1 - 1];
	port_dev->location.cookie = cookie;

	/*
	 * Once we set the location we need to check if this invalidates
	 * the current peer mapping for this port
	 */
	if (!peer_hcd)
		return;

	mutex_lock(&peer_lock);
	peer_hdev = peer_hcd->self.root_hub;
	peer = find_port_by_location(peer_hdev, &port_dev->location);

	/*
	 * If the peer we found does not match the current one then we
	 * need to invalidate all the peer relationships beneath each
	 * port
	 */
	if (port_dev->peer && port_dev->peer != peer) {
		invalidate_dependent_peers(port_dev);
		enum_port_dev = 1;
	}
	if (peer->peer && peer->peer != port_dev) {
		invalidate_dependent_peers(peer);
		enum_peer = 1;
	}

	link_peers_report(port_dev, peer);

	/*
	 * If a peer relationship was invalidated then we need to
	 * re-enumerate all the descendants.  We descend both 'port_dev'
	 * and 'peer' since tier-mismatch implies a mismatch in the
	 * number of descendants.
	 */
	if (enum_port_dev)
		enumerate_dependent_peers(port_dev->child);
	if (enum_peer)
		enumerate_dependent_peers(peer->child);
	mutex_unlock(&peer_lock);
}

int usb_hub_create_port_device(struct usb_hub *hub, int port1)
{
	struct usb_port *port_dev;
	int retval;

	port_dev = kzalloc(sizeof(*port_dev), GFP_KERNEL);
	if (!port_dev) {
		retval = -ENOMEM;
		goto exit;
	}

	hub->ports[port1 - 1] = port_dev;
	port_dev->portnum = port1;
	port_dev->power_is_on = true;
	port_dev->dev.parent = hub->intfdev;
	port_dev->dev.groups = port_dev_group;
	port_dev->dev.type = &usb_port_device_type;
	dev_set_name(&port_dev->dev, "%s-port%d", dev_name(&hub->hdev->dev),
			port1);
	mutex_init(&port_dev->status_lock);

	retval = device_register(&port_dev->dev);
	if (retval)
		goto error_register;

	mutex_lock(&peer_lock);
	if (!port_dev->peer) {
		struct usb_port *peer = find_default_peer(hub, port1);

		if (peer)
			link_peers_report(port_dev, peer);
	}
	mutex_unlock(&peer_lock);

	pm_runtime_set_active(&port_dev->dev);

	/* It would be dangerous if user space couldn't prevent usb
	 * device from being powered off. So don't enable port runtime
	 * pm if failed to expose port's pm qos, or if the hub does not
	 * support power switching
	 */
	if (hub_is_port_power_switchable(hub)
			&& dev_pm_qos_expose_flags(&port_dev->dev,
			PM_QOS_FLAG_NO_POWER_OFF) == 0)
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
	struct usb_port *peer;

	mutex_lock(&peer_lock);
	peer = port_dev->peer;
	if (peer)
		unlink_peers(port_dev, peer);
	mutex_unlock(&peer_lock);

	device_unregister(&port_dev->dev);
}
