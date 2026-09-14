/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_SERVICES_H_
#define _Q1000K_SERVICES_H_
#include "common/q1000k_identity.h"
#ifdef Q1000K_PON_IDENTITY
#include <net/xpon/omci.h>
#include <linux/skbuff.h>

void q1000k_services_init(void);
/* Module teardown, after core callbacks and packet producers have stopped. */
void q1000k_services_destroy(void);
/* Called with protocol ownership after registration has retired all records. */
void q1000k_services_reset(void);
void q1000k_services_enable(bool enabled);
int q1000k_services_topology(struct omci_device *, struct omci_ani_topology *);
int q1000k_services_tcont(struct omci_device *, u16 entity, u16 alloc, bool valid);
int q1000k_services_gem(struct omci_device *, u16 entity, u16 gem, u16 tcont,
		       u8 direction, bool valid, u8 key_ring);
int q1000k_services_gem_config(struct omci_device *, u16 entity,
		const struct omci_gem_port_config *, bool valid);
/* Protocol owner; no MMIO. */
int q1000k_services_gem_key_ring(u16 entity, u8 *key_ring);
int q1000k_services_uni(struct omci_device *, u16 entity, bool enabled);
int q1000k_services_queue(struct omci_device *, u16 entity,
			 const struct omci_priority_queue_config *);
int q1000k_services_scheduler(struct omci_device *, u16 entity,
			 const struct omci_traffic_scheduler_config *);
int q1000k_services_replace(struct omci_device *, const struct omci_service_config *, size_t count);
/* UNI-side Ethernet frames undergo the provisioned VLAN pipeline. The
 * caller holds RCU across classification, binding lookup and native enqueue.
 * tx consumes skb only on success. rx leaves ownership with the caller.
 */
int q1000k_services_tx(struct sk_buff *skb);
int q1000k_services_rx(struct sk_buff *skb, u16 gem);
#endif
#endif
