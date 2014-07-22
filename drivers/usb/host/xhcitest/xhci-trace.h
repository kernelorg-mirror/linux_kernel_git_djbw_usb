/*
 * Reduced version of ../xhci-trace.h for unit-test usage
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM xhci-test

#if !defined(__XHCI_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define __XHCI_TRACE_H

#include <linux/tracepoint.h>
#include "../xhci.h"

#define XHCI_MSG_MAX	500

DECLARE_EVENT_CLASS(xhci_log_msg,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf),
	TP_STRUCT__entry(__dynamic_array(char, msg, XHCI_MSG_MAX)),
	TP_fast_assign(
		vsnprintf(__get_str(msg), XHCI_MSG_MAX, vaf->fmt, *vaf->va);
	),
	TP_printk("%s", __get_str(msg))
);

DEFINE_EVENT(xhci_log_msg, xhci_dbg_context_change,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(xhci_log_msg, xhci_dbg_quirks,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(xhci_log_msg, xhci_dbg_reset_ep,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(xhci_log_msg, xhci_dbg_cancel_urb,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(xhci_log_msg, xhci_dbg_init,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(xhci_log_msg, xhci_dbg_ring_expansion,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DECLARE_EVENT_CLASS(xhci_log_event,
	TP_PROTO(void *trb_va, struct xhci_generic_trb *ev),
	TP_ARGS(trb_va, ev),
	TP_STRUCT__entry(
		__field(void *, va)
		__field(u64, dma)
		__field(u32, status)
		__field(u32, flags)
		__dynamic_array(u8, trb, sizeof(struct xhci_generic_trb))
	),
	TP_fast_assign(
		__entry->va = trb_va;
		__entry->dma = ((u64)le32_to_cpu(ev->field[1])) << 32 |
					le32_to_cpu(ev->field[0]);
		__entry->status = le32_to_cpu(ev->field[2]);
		__entry->flags = le32_to_cpu(ev->field[3]);
		memcpy(__get_dynamic_array(trb), trb_va,
			sizeof(struct xhci_generic_trb));
	),
	TP_printk("\ntrb_dma=@%pad, trb_va=@%p, status=%08x, flags=%08x",
			&__entry->dma, __entry->va,
			__entry->status, __entry->flags
	)
);

DEFINE_EVENT(xhci_log_event, xhci_cmd_completion,
	TP_PROTO(void *trb_va, struct xhci_generic_trb *ev),
	TP_ARGS(trb_va, ev)
);

#endif /* __XHCI_TRACE_H */

/* this part must be outside header guard */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE xhci-trace

#include <trace/define_trace.h>
