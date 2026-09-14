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
};

struct q1000k_pipeline_status {
	enum q1000k_pipeline_stage stage;
	u32 retired;
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

#endif
