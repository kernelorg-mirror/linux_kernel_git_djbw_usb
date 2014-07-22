CFLAGS_xhci-unit-trace.o := -I$(src)

# Boilplate wrappers for functions defined in xhci.c (not included)
ldflags-y := --wrap=xhci_find_slot_id_by_port
ldflags-y += --wrap=xhci_handshake
ldflags-y += --wrap=xhci_quiesce
ldflags-y += --wrap=xhci_find_raw_port_number
ldflags-y += --wrap=xhci_reset
ldflags-y += --wrap=xhci_free_device_endpoint_resources
ldflags-y += --wrap=xhci_halt
ldflags-y += --wrap=xhci_get_endpoint_index
ldflags-y += --wrap=xhci_test_and_clear_bit
ldflags-y += --wrap=xhci_get_endpoint_address
ldflags-y += --wrap=xhci_last_valid_endpoint
ldflags-y += --wrap=xhci_update_tt_active_eps
ldflags-y += --wrap=xhci_cleanup_stalled_ring
ldflags-y += --wrap=xhci_set_link_state
ldflags-y += --wrap=xhci_ring_device

# Core functions we want to stub out
ldflags-y += --wrap=usb_hcd_link_urb_to_ep

# Mocked functions for passing test parameters or preventing hardware
# communication.  These are guarded by ifndef XHCI_UNIT in xhci-ring.c
# or xhci-mem.c.
ldflags-y += --wrap=xhci_get_ep_ctx
ldflags-y += --wrap=xhci_ring_ep_doorbell

obj-$(CONFIG_USB_XHCI_TEST) += xhci-test.o

# main test
xhci-test-y := xhci-unit.o

# infrastructure
xhci-test-y += xhci-unit-dbg.o xhci-unit-trace.o
