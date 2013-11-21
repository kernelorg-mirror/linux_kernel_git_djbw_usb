/*
 * usb hub driver head file
 *
 * Copyright (C) 1999 Linus Torvalds
 * Copyright (C) 1999 Johannes Erdfelt
 * Copyright (C) 1999 Gregory P. Smith
 * Copyright (C) 2001 Brad Hards (bhards@bigpond.net.au)
 * Copyright (C) 2012 Intel Corp (tianyu.lan@intel.com)
 *
 *  move struct usb_hub to this file.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/usb.h>
#include <linux/usb/ch11.h>
#include <linux/usb/hcd.h>
#include "usb.h"

struct usb_hub {
	struct device		*intfdev;	/* the "interface" device */
	struct usb_device	*hdev;
	struct urb		*urb;		/* for interrupt polling pipe */

	/* buffer for urb ... with extra space in case of babble */
	u8			(*buffer)[8];
	union {
		struct usb_hub_status	hub;
		struct usb_port_status	port;
	}			*status;	/* buffer for status reports */
	struct mutex		status_mutex;	/* for the status buffer */

	int			error;		/* last reported error */
	int			nerrors;	/* track consecutive errors */

	struct work_struct	event_work;	/* hubs w/data or errs ready */
	unsigned long		event_bits[1];	/* status change bitmask */
	unsigned long		change_bits[1];	/* ports with logical connect
							status change */
	unsigned long		busy_bits[1];	/* ports being reset or
							resumed */
	unsigned long		poweroff_bits[1]; /* ports logically off */
	unsigned long		removed_bits[1]; /* ports with a "removed"
							device present */
	unsigned long		wakeup_bits[1];	/* ports that have signaled
							remote wakeup */
#if USB_MAXCHILDREN > 31 /* 8*sizeof(unsigned long) - 1 */
#error event_bits[] is too short!
#endif

	struct usb_hub_descriptor *descriptor;	/* class descriptor */
	struct usb_tt		tt;		/* Transaction Translator */

	unsigned		mA_per_port;	/* current for each child */
#ifdef	CONFIG_PM
	unsigned		wakeup_enabled_descendants;
#endif

	unsigned		limited_power:1;
	unsigned		quiescing:1;

	unsigned		quirk_check_port_auto_suspend:1;

	unsigned		has_indicators:1;
	u8			indicator[USB_MAXCHILDREN];
	struct delayed_work	leds;
	struct delayed_work	init_work;
	struct usb_port		**ports;
};

/**
 * struct usb port - kernel's representation of a usb port
 * @child: usb device attatched to the port
 * @dev: generic device interface
 * @port_owner: port's owner
 * @connect_type: port's connect type
 * @portnum: port index num based one
 */
struct usb_port {
	struct usb_device *child;
	struct device dev;
	struct dev_state *port_owner;
	struct work_struct ratelimit_work;
	enum usb_port_connect_type connect_type;
	u8 portnum;
};

#define to_usb_port(_dev) \
	container_of(_dev, struct usb_port, dev)

extern int usb_hub_create_port_device(struct usb_hub *hub,
		int port1);
extern void usb_hub_remove_port_device(struct usb_hub *hub,
		int port1);
extern struct usb_hub *usb_hub_to_struct_hub(struct usb_device *hdev);
extern int usb_clear_port_feature(struct usb_device *hdev,
		int port1, int feature);
extern int usb_set_port_feature(struct usb_device *hdev,
		int port1, int feature);
extern void usb_assign_port_poweroff(struct usb_hub *hub, int port1, int val);
#define usb_set_port_poweroff(h, p) usb_assign_port_poweroff(h, p, 1)
#define usb_clear_port_poweroff(h, p) usb_assign_port_poweroff(h, p, 0)
extern struct usb_device *usb_port_get_child(struct usb_port *port_dev);
static inline void usb_port_put_child(struct usb_device *udev)
{
	if (!udev)
		return;
	put_device(&udev->dev);
}
