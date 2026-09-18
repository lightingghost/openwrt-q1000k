// SPDX-License-Identifier: GPL-2.0-only
/* Independent of live PHY/MAC state: available until the final BSP unload. */
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <q1000k_trace.h>

#define QT_CAPACITY 4096
#define QT_IDS 256
#define QT_CRITICAL_CAPACITY 8192
struct qt_record {
	u64 seq, ns;
	u32 generation, event, id;
	s32 result;
	u32 a, b, c, d;
};
static DEFINE_SPINLOCK(qt_lock);
static struct qt_record qt_ring[QT_CAPACITY], qt_first[QT_EVENT_COUNT];
static u64 qt_count[QT_EVENT_COUNT][QT_IDS], qt_errors[QT_EVENT_COUNT][QT_IDS];
static u64 qt_results[QT_EVENT_COUNT][256];
static u64 qt_seq, qt_critical_seq;
struct qt_critical_record { u64 position; struct qt_record record; };
static struct qt_critical_record qt_critical[QT_CRITICAL_CAPACITY];
static bool qt_is_critical(const struct qt_record *r)
{
 return r->event == QT_FAULT || r->event == QT_ASSIGN || r->event == QT_RESET ||
  r->event == QT_STATE || r->event == QT_DISCOVERY || r->event == QT_RECOVERY_PHASE ||
  r->event == QT_ACTIVATION || r->event == QT_CONTROL ||
  r->event == QT_OMCI_GEM || r->event == QT_OMCI_OPERATION ||
  r->event == QT_OMCI || r->event == QT_OMCI_NATIVE_TX || r->event == QT_GWAN_APPEND ||
  (r->event == QT_MAC_IRQ && r->id >= 2 && r->id <= 5) ||
  ((r->event == QT_PLOAM_VERIFY || r->event == QT_PLOAM_DISPATCH) && r->id != 1);
}
static u32 qt_generation;
static struct proc_dir_entry *qt_entry;

void q1000k_trace(unsigned int event, unsigned int id, int result,
		   u32 a, u32 b, u32 c, u32 d)
{
	struct qt_record r;
	unsigned long flags;
	if (!event || event >= QT_EVENT_COUNT || id >= QT_IDS)
		return;
	spin_lock_irqsave(&qt_lock, flags);
	r = (struct qt_record){ ++qt_seq, ktime_get_boottime_ns(), qt_generation,
				event, id, result, a, b, c, d };
	qt_count[event][id]++;
	qt_results[event][result < 0 ? min_t(unsigned int, -(s64)result, 255) : 0]++;
	if (result < 0) qt_errors[event][id]++;
	qt_ring[(r.seq - 1) % QT_CAPACITY] = r;
	if (!qt_first[event].seq) qt_first[event] = r;
 if (qt_is_critical(&r)) {
  u64 position = ++qt_critical_seq;
  qt_critical[(position - 1) % QT_CRITICAL_CAPACITY] =
   (struct qt_critical_record){ position, r };
 }
	spin_unlock_irqrestore(&qt_lock, flags);
}
EXPORT_SYMBOL(q1000k_trace);

void q1000k_trace_generation(unsigned int reason)
{
	unsigned long flags;
	spin_lock_irqsave(&qt_lock, flags);
	qt_generation++;
	spin_unlock_irqrestore(&qt_lock, flags);
	q1000k_trace(QT_GENERATION, reason, 0, 0, 0, 0, 0);
}
EXPORT_SYMBOL(q1000k_trace_generation);

static void qt_print(struct seq_file *s, const struct qt_record *r, bool first)
{
	seq_printf(s, "{\"trace_version\":1,\"first\":%s,\"seq\":%llu,\"ns\":%llu,\"generation\":%u,\"event\":%u,\"id\":%u,\"result\":%d,\"a\":%u,\"b\":%u,\"c\":%u,\"d\":%u}\n",
		first ? "true" : "false", r->seq, r->ns, r->generation,
		r->event, r->id, r->result, r->a, r->b, r->c, r->d);
}

static int qt_show(struct seq_file *s, void *unused)
{
	unsigned long flags;
	u64 end, begin, n, count, errors;
	u32 gen, event, id;
	struct qt_record r;
	spin_lock_irqsave(&qt_lock, flags);
	end = qt_seq; gen = qt_generation;
	spin_unlock_irqrestore(&qt_lock, flags);
	begin = end >= QT_CAPACITY ? end - QT_CAPACITY + 1 : 1;
	seq_printf(s, "{\"trace_header\":1,\"capacity\":%u,\"record_bytes\":%zu,\"oldest\":%llu,\"newest\":%llu,\"wrapped\":%llu,\"generation\":%u}\n",
		QT_CAPACITY, sizeof(r), begin, end, begin - 1, gen);
	/* Lock for one small copy at a time, never for formatting or a full ring.
	 * Concurrent wrap is explicit missing evidence, not a mixed record. */
	for (n = begin; n <= end; n++) {
		spin_lock_irqsave(&qt_lock, flags);
		r = qt_ring[(n - 1) % QT_CAPACITY];
		spin_unlock_irqrestore(&qt_lock, flags);
		if (r.seq == n) qt_print(s, &r, false);
		else seq_printf(s, "{\"trace_gap\":%llu}\n", n);
	}
	/* Independent retention: a profile/job storm cannot evict discovery.
  * Original sequence numbers permit deduplication; independent positions
  * make critical-stream loss measurable even when the main ring wraps. */
 spin_lock_irqsave(&qt_lock, flags);
 end = qt_critical_seq;
 spin_unlock_irqrestore(&qt_lock, flags);
 begin = end >= QT_CRITICAL_CAPACITY ? end - QT_CRITICAL_CAPACITY + 1 : 1;
 seq_printf(s, "{\"critical_header\":1,\"capacity\":%u,\"oldest\":%llu,\"newest\":%llu}\n",
  QT_CRITICAL_CAPACITY, begin, end);
 for (n = begin; n <= end; n++) {
  struct qt_critical_record c;
  spin_lock_irqsave(&qt_lock, flags);
  c = qt_critical[(n - 1) % QT_CRITICAL_CAPACITY];
  spin_unlock_irqrestore(&qt_lock, flags);
  if (c.position == n) {
   r = c.record;
   seq_printf(s, "{\"critical_version\":1,\"position\":%llu,\"seq\":%llu,\"ns\":%llu,\"generation\":%u,\"event\":%u,\"id\":%u,\"result\":%d,\"a\":%u,\"b\":%u,\"c\":%u,\"d\":%u}\n",
    n, r.seq, r.ns, r.generation, r.event, r.id, r.result, r.a, r.b, r.c, r.d);
  } else seq_printf(s, "{\"critical_gap\":%llu}\n", n);
 }
 for (event = 1; event < QT_EVENT_COUNT; event++) {
		spin_lock_irqsave(&qt_lock, flags);
		r = qt_first[event];
		spin_unlock_irqrestore(&qt_lock, flags);
		if (r.seq) qt_print(s, &r, true);
		for (id = 0; id < QT_IDS; id++) {
			spin_lock_irqsave(&qt_lock, flags);
			count = qt_count[event][id]; errors = qt_errors[event][id];
			spin_unlock_irqrestore(&qt_lock, flags);
			if (count) seq_printf(s, "{\"trace_count\":1,\"event\":%u,\"id\":%u,\"count\":%llu,\"errors\":%llu}\n",
					     event, id, count, errors);
		}
		for (id = 0; id < 256; id++) {
			spin_lock_irqsave(&qt_lock, flags);
			count = qt_results[event][id];
			spin_unlock_irqrestore(&qt_lock, flags);
			if (count) seq_printf(s, "{\"trace_result\":1,\"event\":%u,\"errno\":%u,\"count\":%llu}\n", event, id, count);
		}
	}
	return 0;
}

int q1000k_trace_init(void)
{
	qt_entry = proc_create_single("q1000k-pon-events", 0400, NULL, qt_show);
	return qt_entry ? 0 : -ENOMEM;
}

void q1000k_trace_exit(void)
{
	proc_remove(qt_entry);
	qt_entry = NULL;
}
