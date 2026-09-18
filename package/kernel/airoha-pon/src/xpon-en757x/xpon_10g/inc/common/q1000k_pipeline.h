/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PIPELINE_H_
#define _Q1000K_PIPELINE_H_

#include <linux/types.h>

enum q1000k_pipeline_stage {
	Q1000K_PIPELINE_UNDRAINED,
	Q1000K_PIPELINE_CPU_PAUSED,
	Q1000K_PIPELINE_INGRESS_STOPPED,
	Q1000K_PIPELINE_FE_RETIRED,
	Q1000K_PIPELINE_FIFO_EMPTY,
	Q1000K_PIPELINE_MAC_STOPPED,
	Q1000K_PIPELINE_RX_DRAINED,
	Q1000K_PIPELINE_PHY_STOPPED,
	Q1000K_PIPELINE_TABLES_CHANGING,
	Q1000K_PIPELINE_TABLES_CLEARED,
	Q1000K_PIPELINE_FCS_CLEARED,
	Q1000K_PIPELINE_EPOCH_READY,
	Q1000K_PIPELINE_PREPARED,
	Q1000K_PIPELINE_RX_ACTIVE,
	Q1000K_PIPELINE_PHY_ACTIVE,
	Q1000K_PIPELINE_MAC_ACTIVE,
};

struct q1000k_pipeline_status {
	enum q1000k_pipeline_stage stage;
	u32 retired;
	u32 channels;
	int error;
	int containment_error;
};

/* Process context, outside RTNL/RCU. Before entry the lifecycle owner must
 * close and drain all MAC protocol, timer, tasklet and control producers.
 * Packet attachment must remain alive. This is a complete port shutdown;
 * physical success alone does not remove service tables or restart a port.
 * Failure is permanent for this MAC instance and poisons the BSP provider.
 */
int q1000k_pipeline_shutdown(void);
void q1000k_pipeline_status(struct q1000k_pipeline_status *status);

/* After the caller drains all protocol/control producers, reconfigure first
 * completes physical shutdown. clear removes the old reusable table entries;
 * install programs and verifies replacement tables after native epoch reset.
 * Preserved ONU/OMCC/key state must never refer to a reused entry. reset_mac
 * requests the exclusive cold-start reset and requires install to restore all
 * needed MAC configuration. Callback failure leaves the pipeline poisoned;
 * the service owner must report EUCLEAN if the old hardware state is lost.
 * Neither callback may re-enter lifecycle APIs or start producers/queues.
 */
struct q1000k_pipeline_ops {
	bool reset_mac;
	int (*clear)(void *arg);
	int (*install)(void *arg);
};
enum q1000k_table_phase { Q1000K_TABLE_CLEAR, Q1000K_TABLE_INSTALL, Q1000K_TABLE_ACTIVATE, Q1000K_TABLE_APPEND };
/* Internal table primitives: permission exists only in the owning callback. */
int q1000k_pipeline_table_context(enum q1000k_table_phase phase);
int q1000k_pipeline_reconfigure(const struct q1000k_pipeline_ops *ops,
			       void *arg, u32 channels);
/* Add previously empty entries only, with the protocol/table transaction
 * guard held. No old identity, key, binding or queue may change. New channels
 * remain closed. A failure after entry contains the entire port permanently.
 */
int q1000k_pipeline_append(int (*install)(void *), void *arg, u32 channels);
/* Restore receive DMA, PHY callbacks, MAC transfers and controller TX in order.
 * CPU queues remain closed for explicit provisioning after success.
 */
int q1000k_pipeline_activate(void);
/* Resume receive/control state while keeping the controller transmitter off. */
int q1000k_pipeline_activate_receive_only(void);
/* Check resumed MAC/PHY state before optical TX and CPU producers can start.
 * The synchronous callback runs with ACTIVATE ownership and must not reenter.
 */
int q1000k_pipeline_activate_checked(bool transmit, int (*ready)(void *), void *arg);

#endif
