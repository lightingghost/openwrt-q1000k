/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_XPON_H
#define _AN7581_XPON_H

#include <linux/types.h>

struct device;

/* Borrowed device: consumers must hold this provider's module dependency. */
struct device *get_xpon_dev(void);
int get_xpon_irq(int index);
u32 get_xpon_data(u32 reg);
void set_xpon_data(u32 reg, u32 value);

/* XG-PON MBI/MPI stop controls, not completion/status bits. */
#define AN7581_XPON_MBI_RX_STOP (1U << 0)
#define AN7581_XPON_MBI_TX_STOP (1U << 8)
#define AN7581_XPON_MPI_RX_STOP (1U << 16)
#define AN7581_XPON_MPI_TX_STOP (1U << 24)

/* Atomic-context compatible, bounded to 3 ms. Stop timeout or invalid MMIO
 * latches a fault; release is prohibited until the provider is reinitialized.
 * A stop acknowledgment alone does not establish a drained optical pipeline.
 */
int an7581_xpon_mac_stop(u32 controls, bool hold);
/* Cold initialization only: latch MPI RX stop and verify the control bit.
 * Does not wait for completion and never proves ingress or FIFO retirement.
 */
int an7581_xpon_mac_request_rx_stop(void);
int an7581_xpon_mac_wait_tx_empty(void);

/* Process context only. Caller must quiesce MAC callbacks and the physical
 * pipeline first. Reset is exclusive to PON; errors remain latched. */
int an7581_xpon_reset(void);
int an7581_xpon_status(void);
/* Latch a failed physical transaction; this does not itself stop hardware. */
void an7581_xpon_invalidate(void);

#endif
