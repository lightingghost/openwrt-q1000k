/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_GWAN_H_
#define _Q1000K_GWAN_H_

#include <linux/types.h>

struct q1000k_gwan_binding {
	u16 gem;
	u16 alloc_id;
	u16 ani;
	u16 index;
	u8 channel;
	bool multicast;
};

/* One coherent data GEM/T-CONT snapshot; outputs are unchanged on failure.
 * Active bindings cannot be reassigned or reused before physical retirement.
 * Physical replacement closes binding acquisition, drains existing producers
 * and hardware, then publishes the complete replacement before reactivation.
 * No callbacks, MMIO or packet submission occur while the state lock is held.
 */
int q1000k_gwan_binding(u16 gem, bool tx, struct q1000k_gwan_binding *binding);
void q1000k_gwan_account(u16 gem, bool tx, unsigned int bytes);

#define Q1000K_GWAN_GEMS 256
#define Q1000K_GWAN_CHANNELS 32
#define Q1000K_GWAN_UNASSIGNED 0xffff
#define Q1000K_GWAN_UNKNOWN_CHANNEL 33
struct q1000k_gwan_entry {
	u16 gem, alloc_id, ani;
	u8 channel;
	bool valid, multicast, encrypted;
};
struct q1000k_gwan_table {
	u16 alloc_id[Q1000K_GWAN_CHANNELS];
	struct q1000k_gwan_entry gem[Q1000K_GWAN_GEMS];
};
/* Snapshot/apply retain the existing ONU/OMCC identity. Apply is process-only,
 * outside RTNL/RCU, and compares the complete expected table before changing
 * hardware. Errors before physical retirement preserve records; EUCLEAN means
 * retirement/replacement failed and the port is contained. Both arrays remain
 * owned by the caller and must stay unchanged until apply returns.
 */
int q1000k_gwan_snapshot(struct q1000k_gwan_table *table);
int q1000k_gwan_apply(const struct q1000k_gwan_table *expected,
		      const struct q1000k_gwan_table *desired);
/* Apply a complete record candidate and its QoS in the same drain. Even an
 * identical record set executes install, before any packet producer resumes.
 */
int q1000k_gwan_apply_install(const struct q1000k_gwan_table *expected,
			    const struct q1000k_gwan_table *desired,
			    int (*install)(void *), void *arg);
int q1000k_gwan_delete_gem(u16 gem, bool all);
int q1000k_gwan_delete_tcont(u16 alloc_id, bool all);
int q1000k_gwan_add_tcont(u16 alloc_id);
/* Authentication owner: after closing/draining OMCI core and retry admission,
 * refresh an unchanged data namespace and run install while hardware is fully
 * drained. The callback must only install MAC state; no core session calls.
 */
int q1000k_gwan_refresh(int (*install)(void *arg), void *arg);
/* Ranging resynchronization also verifies MAC readiness before TX resumes. */
int q1000k_gwan_refresh_checked(int (*install)(void *), int (*ready)(void *), void *arg);

/* Registration owner, after core/session and OMCI retry barriers: discard all
 * old data/OMCC records and install ONU/keys through the drained callback.
 * onu_id is 0..1022, or UNASSIGNED to remove the namespace. All queues stay
 * closed after success; only the registration owner may open OMCC queue zero.
 */
int q1000k_gwan_register(u16 onu_id, int (*install)(void *arg), void *arg);

/* Cold startup/recovery: reset the exclusive MAC only after the complete
 * drain, verify the entire GEM address space is empty, discard ONU/OMCC/data
 * records and run install. Caller restores ALL required MAC state and keys.
 * Success resumes receive only, with every queue and controller TX closed.
 * Core/session barriers are the same as for registration replacement.
 */
int q1000k_gwan_cold_reset(int (*install)(void *arg), void *arg);

/* Private record-owner bridge. The nonblocking transaction guard must remain
 * held across snapshot, physical replacement and publication. begin closes
 * data binding snapshots until end; no state lock spans hardware callbacks.
 */
int q1000k_gwan_table_begin(void);
void q1000k_gwan_table_end(void);
void q1000k_gwan_table_publish(const struct q1000k_gwan_table *table);
void q1000k_gwan_table_failed(int error);
#endif
