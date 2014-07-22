#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/random.h>
#define XHCI_UNIT
#include "../xhci-mem.c"
#include "../xhci-ring.c"

static struct rnd_state rnd_state;
static struct xhci_hcd test_xhci;
static struct usb_hcd test_hcd;
#define DEFAULT_CYCLE 0

static unsigned long random(void)
{
	unsigned long rnd;

	prandom_bytes_state(&rnd_state, &rnd, sizeof(rnd));
	return rnd;
}

static unsigned int xhci_test_mbp;
struct xhci_ep_ctx *__wrap_xhci_get_ep_ctx(struct xhci_hcd *xhci,
				    struct xhci_container_ctx *ctx,
				    unsigned int ep_index)
{
	static struct xhci_ep_ctx ep_ctx;
	unsigned int mbp = xhci_test_mbp;

	ep_ctx.ep_info2 = __cpu_to_le32(MAX_PACKET(mbp) | MAX_BURST(0));
	ep_ctx.ep_info = __cpu_to_le32(EP_STATE_RUNNING);
	return &ep_ctx;
}

typedef int (*setup_test_t)(struct sg_table *table, struct urb **urb,
		struct xhci_ring **ring, unsigned int *result);

#define DECLARE_URB_INFRA(num_sgs)					\
	static struct usb_device dev = {				\
		/* must be less than 0x7ff */				\
		.ep0.desc.wMaxPacketSize = __cpu_to_le16(0x400),	\
	};								\
	static struct {							\
		struct urb_priv urb_priv;				\
		struct xhci_td *ptr;					\
		struct xhci_td td;					\
	} p = {								\
		.urb_priv.length = 1,					\
		.urb_priv.td_cnt = 0,					\
	};								\
	static struct urb test_urb = {					\
		.num_mapped_sgs = num_sgs,				\
		.ep = &dev.ep0,						\
		.hcpriv = &p,						\
		.dev = &dev,						\
	}

#define INIT_URB_INFRA(urb, len, sgl)				\
	do {							\
		dev.bus = &xhci_to_hcd(&test_xhci)->self,	\
		test_urb.transfer_buffer_length = len;		\
		p.urb_priv.td[0] = &p.td;			\
		test_urb.sg = sgl;				\
		*(urb) = &test_urb;				\
	} while (0)



/*
 * 1/ queue 32 trbs starting at index 0 and on mbp boundary
 * 2/ attempt to queue 248 more without crossing a mbp boundary (trigger
 *    a mid-segment link and ring expansion)
 * 3/ queue 8 more, mbp aligned, triggers one more ring expansion
 *
 * Result: enqueue advanced to index 513
 */
static int setup_test_32_248_8(struct sg_table *table, struct urb **urb,
		struct xhci_ring **ring, unsigned int *result)
{
	struct xhci_hcd *xhci = &test_xhci;
	unsigned int total_len = 0;
	struct scatterlist *sg;
	int rc, i;

	DECLARE_URB_INFRA(32+248+8);

	xhci_test_mbp = 248;

	*ring = xhci_ring_alloc(xhci, 0, DEFAULT_CYCLE, TYPE_BULK, GFP_KERNEL);
	if (!*ring)
		return -ENOMEM;

	rc = sg_alloc_table(table, test_urb.num_mapped_sgs, GFP_KERNEL);
	if (rc) {
		xhci_ring_free(*ring);
		return -ENOMEM;
	}

	for_each_sg(table->sgl, sg, test_urb.num_mapped_sgs, i) {
		unsigned int len;
		dma_addr_t dma;
		void *buf;

		buf = (void *) roundup(random(), xhci_test_mbp);
		dma = (dma_addr_t) roundup(random(), xhci_test_mbp);
		if (i < 32)
			len = xhci_test_mbp;
		else if (i < 32+248)
			len = xhci_test_mbp / 248;
		else if (i < 32+248+8)
			len = xhci_test_mbp;
		sg_set_buf(sg, buf, len);
		sg->dma_address = dma;
		sg_dma_len(sg) = len;
		total_len += len;
	}

	INIT_URB_INFRA(urb, total_len, table->sgl);
	*result = 513;

	return 0;
}

/*
 * 1/ set up a 2 segment ring where enqueue wraps when it advances (32
 *    trbs free)
 *
 * 2/ queue 64 segments that never cross an mbp, but cross a 64K dma
 *    boundary, causes a ring expansion before wrap
 *
 * Result: enqueue advanced to index 577
 */
static int setup_test_skip64(struct sg_table *table, struct urb **urb,
		struct xhci_ring **ring, unsigned int *result)
{
	struct xhci_hcd *xhci = &test_xhci;
	struct xhci_segment *enq_seg;
	struct xhci_ring_pointer rp;
	unsigned int total_len = 0;
	struct scatterlist *sg;
	dma_addr_t dma;
	void *buf;
	int rc, i;

	DECLARE_URB_INFRA(64);

	xhci_test_mbp = 4096;

	*ring = xhci_ring_alloc(xhci, 1, DEFAULT_CYCLE, TYPE_BULK, GFP_KERNEL);
	if (!*ring)
		return -ENOMEM;

	enq_seg = list_last_entry(&(*ring)->segments, typeof(*enq_seg), list);
	rp.ptr = &enq_seg->trbs[TRBS_PER_SEGMENT - 32];
	rp.seg = enq_seg;
	xhci_ring_set_enqueue(*ring, &rp);

	(*ring)->num_trbs_free = xhci_ring_num_trbs_free(*ring);

	rc = sg_alloc_table(table, test_urb.num_mapped_sgs, GFP_KERNEL);
	if (rc) {
		xhci_ring_free(*ring);
		return -ENOMEM;
	}

	buf = (void *) (TRB_MAX_BUFF_SIZE - 32);
	dma = (dma_addr_t) (TRB_MAX_BUFF_SIZE - 32);

	for_each_sg(table->sgl, sg, test_urb.num_mapped_sgs, i) {
		unsigned int len = xhci_test_mbp / 64;

		sg_set_buf(sg, buf, len);
		sg->dma_address = dma;
		sg_dma_len(sg) = len;
		total_len += len;
		buf += len;
		dma += len;
	}

	INIT_URB_INFRA(urb, total_len, table->sgl);
	*result = 577;

	return 0;
}

/*
 * Check cycle-bit wrapping a td over a ring-toggle boundary with 32 segments
 * free in the enqueue segment and 128 free in the dequeue segment.
 *
 * Result: enqueue advanced to index 34
 */
static int setup_test_wrap64(struct sg_table *table, struct urb **urb,
		struct xhci_ring **ring, unsigned int *result)
{
	struct xhci_segment *enq_seg, *deq_seg;
	struct xhci_hcd *xhci = &test_xhci;
	struct xhci_ring_pointer rp;
	unsigned int total_len = 0;
	struct scatterlist *sg;
	dma_addr_t dma;
	void *buf;
	int rc, i;

	DECLARE_URB_INFRA(64);

	xhci_test_mbp = 4096;

	*ring = xhci_ring_alloc(xhci, 2, DEFAULT_CYCLE, TYPE_BULK, GFP_KERNEL);
	if (!*ring)
		return -ENOMEM;

	/* enqueue in the last segment in the ring */
	enq_seg = list_last_entry(&(*ring)->segments, typeof(*enq_seg), list);
	rp.ptr = &enq_seg->trbs[TRBS_PER_SEGMENT - 32];
	rp.seg = enq_seg;
	xhci_ring_set_enqueue(*ring, &rp);

	/* dequeue in the 2nd segment (room to wrap an keep queuing) */
	deq_seg = list_first_entry(&(*ring)->segments, typeof(*deq_seg), list);
	deq_seg = xhci_segment_next(*ring, deq_seg);
	rp.ptr = &deq_seg->trbs[0];
	rp.seg = deq_seg;
	xhci_ring_set_dequeue(*ring, &rp);

	(*ring)->num_trbs_free = xhci_ring_num_trbs_free(*ring);

	rc = sg_alloc_table(table, test_urb.num_mapped_sgs, GFP_KERNEL);
	if (rc) {
		xhci_ring_free(*ring);
		return -ENOMEM;
	}

	for_each_sg(table->sgl, sg, test_urb.num_mapped_sgs, i) {
		unsigned int len = xhci_test_mbp + 1;

		buf = (void *) (random() % xhci_test_mbp);
		dma = (dma_addr_t) (random() % xhci_test_mbp);
		sg_set_buf(sg, buf, len);
		sg->dma_address = dma;
		sg_dma_len(sg) = len;
		total_len += len;
		buf += len;
		dma += len;
	}

	INIT_URB_INFRA(urb, total_len, table->sgl);
	*result = 34;

	return 0;
}

/* place a mbp boundary crossing right next to a link */
static int setup_test_dont_trim(struct sg_table *table, struct urb **urb,
		struct xhci_ring **ring, unsigned int *result)
{
	struct xhci_segment *enq_seg, *deq_seg;
	struct xhci_hcd *xhci = &test_xhci;
	struct xhci_ring_pointer rp;
	unsigned int total_len = 0;
	struct scatterlist *sg;
	dma_addr_t dma;
	void *buf;
	int rc, i;

	DECLARE_URB_INFRA(1);

	xhci_test_mbp = 4096;

	*ring = xhci_ring_alloc(xhci, 1, DEFAULT_CYCLE, TYPE_BULK, GFP_KERNEL);
	if (!*ring)
		return -ENOMEM;

	/* enqueue at the end of the first segment */
	enq_seg = list_first_entry(&(*ring)->segments, typeof(*enq_seg), list);
	rp.ptr = &enq_seg->trbs[TRBS_PER_SEGMENT - 2];
	rp.seg = enq_seg;
	xhci_ring_set_enqueue(*ring, &rp);

	/* dequeue in the 2nd segment (room to wrap an keep queuing) */
	deq_seg = list_last_entry(&(*ring)->segments, typeof(*deq_seg), list);
	rp.ptr = &deq_seg->trbs[0];
	rp.seg = deq_seg;
	xhci_ring_set_dequeue(*ring, &rp);

	(*ring)->num_trbs_free = xhci_ring_num_trbs_free(*ring);

	rc = sg_alloc_table(table, test_urb.num_mapped_sgs, GFP_KERNEL);
	if (rc) {
		xhci_ring_free(*ring);
		return -ENOMEM;
	}

	for_each_sg(table->sgl, sg, test_urb.num_mapped_sgs, i) {
		unsigned int len = xhci_test_mbp + 1;

		buf = (void *) (random() % xhci_test_mbp);
		dma = (dma_addr_t) (random() % xhci_test_mbp);
		sg_set_buf(sg, buf, len);
		sg->dma_address = dma;
		sg_dma_len(sg) = len;
		total_len += len;
		buf += len;
		dma += len;
	}

	INIT_URB_INFRA(urb, total_len, table->sgl);
	*result = 255;

	return 0;
}

/*
 * Ensure the trbs after a mid-segment-link will have software-owned
 * cycle bits after the ring wraps and the mid-segment link is
 * invalidated
 */
static bool check_shaded_trbs(struct xhci_ring *ring, unsigned int start_idx,
		u32 cycle)
{
	struct xhci_segment *seg;
	unsigned int i, num_trbs;

	seg = to_xhci_ring_segment(ring, ring->enq.seg, start_idx);
	num_trbs = ALIGN(start_idx, TRBS_PER_SEGMENT) - start_idx;
	for (i = 0; i < num_trbs; i++) {
		unsigned int idx = to_xhci_ring_index(ring, start_idx + i);
		union xhci_trb *trb = to_xhci_ring_trb(ring, seg, idx);
		u32 val = __le32_to_cpu(trb->generic.field[3]);

		if ((val & TRB_CYCLE) != cycle)
			return false;
	}
	return true;
}

static bool do_test(struct xhci_hcd *xhci, setup_test_t setup_test, int test)
{
	unsigned int expect, end_idx, trbs_free, ring_trbs_free;
	struct xhci_ring_pointer rp, start;
	u32 cycle = DEFAULT_CYCLE;
	struct urb_priv *urb_priv;
	struct xhci_ring *ring;
	int rc, cycle_err = 0;
	struct sg_table table;
	union xhci_trb *trb;
	struct xhci_td *td;
	struct urb *urb;

	if (setup_test(&table, &urb, &ring, &expect) != 0) {
		pr_err("test%d setup fail\n", test);
		return false;
	}
	rc = ring->ops->queue_bulk_sg_tx(xhci, ring, GFP_KERNEL, urb, urb->sg,
			urb->num_mapped_sgs, 0, 0);
	end_idx = xhci_ring_pointer_to_index(&ring->enq);
	ring_trbs_free = xhci_ring_num_trbs_free(ring);
	trbs_free = ring->num_trbs_free;

	/* walk the trbs in the td and validate cycle and chain bits */
	urb_priv = urb->hcpriv;
	td = urb_priv->td[0];
	rp.seg = td->start_seg;
	rp.ptr = td->first_trb;
	start = rp;
	do {
		unsigned int idx = xhci_ring_pointer_to_index(&rp);
		unsigned int seg_idx = idx % TRBS_PER_SEGMENT;
		unsigned int segid = idx / TRBS_PER_SEGMENT;
		unsigned int end_segid = xhci_ring_last_seg(ring)->segid;
		u32 control;

		trb = rp.ptr;
		control = __le32_to_cpu(trb->generic.field[3]);

		pr_debug("test%d idx: %d%s%s%s%s\n", test, idx,
			TRB_TYPE_LINK(control) ? " link" : "",
			control & TRB_CHAIN ? " chain" : "",
			control & TRB_CYCLE ? " cycle1" : " cycle0",
			TRB_TYPE_LINK(control)
			&& (control & LINK_TOGGLE) ? " toggle" : "");

		if (!trb_in_td(ring, &start, td->last_trb,
					xhci_trb_virt_to_dma(&rp))) {
			pr_err_ratelimited("test%d: trb_in_td failed at index %u\n",
					test, idx);
			cycle_err++;
		}

		if ((control & TRB_CYCLE) != cycle) {
			pr_err_ratelimited("test%d: wrong cycle at index %u\n",
					test, idx);
			cycle_err++;
		}

		/* check for no chain in starting link trbs */
		if (xhci->hci_version >= 0x100 && trb == td->first_trb
				&& TRB_TYPE_LINK(control)) {
			if (control & TRB_CHAIN) {
				pr_err_ratelimited("test%d: td must not start on a link\n",
						test);
				cycle_err++;
			}
		}

		if (seg_idx < (TRBS_PER_SEGMENT - 1) && seg_idx
				&& TRB_TYPE_LINK(control)
				&& !check_shaded_trbs(ring, idx + 1,
					control & TRB_CYCLE)) {
			pr_err_ratelimited("test%d: wrong cycle in shaded trbs\n",
					test);
			cycle_err++;
		}

		if (TRB_TYPE_LINK(control) && (control & LINK_TOGGLE)
				&& segid != end_segid) {
			pr_err_ratelimited("test%d: toggle at wrong segment %u instead of %u\n",
					test, segid, end_segid);
			cycle_err++;
		}

		if (TRB_TYPE_LINK(control)) {
			cycle ^= !!(control & LINK_TOGGLE);
			xhci_ring_pointer_advance_seg(ring, &rp);
		} else
			xhci_ring_pointer_advance(&rp);
	} while (trb != td->last_trb);

	sg_free_table(&table);
	xhci_ring_free(ring);
	if (rc == 0 && expect == end_idx
			&& trbs_free == ring_trbs_free && cycle_err == 0)
		return true;
	else {
		pr_err("test%d: %pf failed rc: %d end_idx: %u:%u free: %d:%d cycle_err: %d\n",
				test, setup_test, rc, expect, end_idx,
				ring_trbs_free, trbs_free, cycle_err);
		return false;
	}
}

static const struct file_operations xhci_test_fops = {
	.owner =   THIS_MODULE,
};

static void dev_release(struct device *dev)
{
	kfree(dev);
}

static struct class xhci_test_class = {
	.name		= KBUILD_MODNAME,
	.dev_release	= dev_release,
};

static int major;

static __init int xhci_test_init(void)
{
	setup_test_t setup_test_fns[] = {
		setup_test_skip64,
		setup_test_32_248_8,
		setup_test_wrap64,
		setup_test_dont_trim,
	};
	struct xhci_hcd *xhci = &test_xhci;
	static struct xhci_virt_device xdev;
	int v, u, pass = 0, test = 0;
	u16 versions[] = { 0x100 };
	struct device *dev;
	int rc;

	rc = class_register(&xhci_test_class);
	if (rc)
		return rc;

	major = register_chrdev(0, KBUILD_MODNAME, &xhci_test_fops);
	if (major < 0) {
		rc = major;
		goto err_register_chrdev;
	}

	dev = device_create(&xhci_test_class, NULL, MKDEV(major, 0), NULL,
			KBUILD_MODNAME "0");

	if (IS_ERR(dev)) {
		rc = PTR_ERR(dev);
		goto err_device_create;
	}

	prandom_seed_state(&rnd_state, 1);
	xhci->main_hcd = &test_hcd;
	xhci->devs[0] = &xdev;
	test_hcd.self.controller = dev;

	rc = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (rc) {
		xhci_warn(xhci, "unable to set dma\n");
		goto err_dma_set_mask;
	}

	for (v = 0; v < ARRAY_SIZE(versions); v++) {
		xhci->hci_version = versions[v];
		for (u = 0; u < ARRAY_SIZE(setup_test_fns); u++) {
			test++;
			if (do_test(xhci, setup_test_fns[u], test))
				pass++;
		}
	}

	pr_info("pass: %d fail: %d total: %d\n", pass, test - pass, test);

	if (pass == test)
		return 0;
	else
		rc = -EINVAL;

 err_dma_set_mask:
	device_unregister(dev);
 err_device_create:
	unregister_chrdev(major, KBUILD_MODNAME);
 err_register_chrdev:
	class_unregister(&xhci_test_class);
	return rc;
}

static __exit void xhci_test_exit(void)
{
	struct device *dev = xhci_to_dev(&test_xhci);

	device_unregister(dev);
	unregister_chrdev(major, KBUILD_MODNAME);
	class_unregister(&xhci_test_class);
}

module_init(xhci_test_init);
module_exit(xhci_test_exit);
MODULE_LICENSE("GPL v2");

/* compiler boiler plate for unused routines from the missing xhci.c, or
 * routines that we want to stub out
 */
int __wrap_usb_hcd_link_urb_to_ep(struct usb_hcd *hcd, struct urb *urb)
{
	return 0;
}

void __wrap_xhci_ring_ep_doorbell(struct xhci_hcd *xhci, unsigned int slot_id,
		unsigned int ep_index, unsigned int stream_id)
{
}

int __wrap_xhci_find_slot_id_by_port(struct usb_hcd *hcd, struct xhci_hcd *xhci,
		u16 port)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

int __wrap_xhci_handshake(struct xhci_hcd *xhci, void __iomem *ptr,
		u32 mask, u32 done, int usec)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

void __wrap_xhci_quiesce(struct xhci_hcd *xhci)
{
	pr_warn("%s: unimplemented\n", __func__);
}

int __wrap_xhci_find_raw_port_number(struct usb_hcd *hcd, int port1)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

int __wrap_xhci_reset(struct xhci_hcd *xhci)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

void __wrap_xhci_free_device_endpoint_resources(struct xhci_hcd *xhci,
	struct xhci_virt_device *virt_dev, bool drop_control_ep)
{
	pr_warn("%s: unimplemented\n", __func__);
}

int __wrap_xhci_halt(struct xhci_hcd *xhci)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

unsigned int __wrap_xhci_get_endpoint_index(struct usb_endpoint_descriptor *desc)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

void __wrap_xhci_test_and_clear_bit(struct xhci_hcd *xhci,
		__le32 __iomem **port_array, int port_id, u32 port_bit)
{
	pr_warn("%s: unimplemented\n", __func__);
}

unsigned int __wrap_xhci_get_endpoint_address(unsigned int ep_index)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

unsigned int __wrap_xhci_last_valid_endpoint(u32 added_ctxs)
{
	pr_warn("%s: unimplemented\n", __func__);
	return 0;
}

void __wrap_xhci_update_tt_active_eps(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		int old_active_eps)
{
	pr_warn("%s: unimplemented\n", __func__);
}

void __wrap_xhci_cleanup_stalled_ring(struct xhci_hcd *xhci,
		struct usb_device *udev, unsigned int ep_index)
{
	pr_warn("%s: unimplemented\n", __func__);
}

void __wrap_xhci_set_link_state(struct xhci_hcd *xhci, __le32 __iomem **port_array,
				int port_id, u32 link_state)
{
	pr_warn("%s: unimplemented\n", __func__);
}

void __wrap_xhci_ring_device(struct xhci_hcd *xhci, int slot_id)
{
	pr_warn("%s: unimplemented\n", __func__);
}
