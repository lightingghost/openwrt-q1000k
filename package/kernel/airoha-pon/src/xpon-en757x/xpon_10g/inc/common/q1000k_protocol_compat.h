/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PROTOCOL_COMPAT_H_
#define _Q1000K_PROTOCOL_COMPAT_H_
#include "common/q1000k_identity.h"
#ifdef Q1000K_PON_IDENTITY
#include "common/q1000k_protocol.h"
/* Only imported MAC sources include this shim. Kernel headers are included
 * first; native transport, crypto, and this executor use the real APIs.
 */
#define tasklet_init q1000k_protocol_task_init
#define tasklet_schedule q1000k_protocol_task_schedule
#define tasklet_hi_schedule q1000k_protocol_task_schedule
#define tasklet_kill q1000k_protocol_task_kill
#define timer_delete(t) q1000k_protocol_timer_delete(t, false, false)
#define timer_delete_sync(t) q1000k_protocol_timer_delete(t, true, false)
#define timer_shutdown_sync(t) q1000k_protocol_timer_delete(t, true, true)
#endif
#endif
