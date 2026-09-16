// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <ecnt_hook/ecnt_hook.h>
#include <q1000k_trace.h>

static int __init airoha_ecnt_hook_init(void)
{
	ecnt_hook_init();
	return q1000k_trace_init();
}

static void __exit airoha_ecnt_hook_exit(void)
{
	q1000k_trace_exit();
	/* Consumers pin this module through exported symbols and unregister
	 * their hooks, including the RCU grace period, before dropping that
	 * reference. The framework owns only static list heads and a mutex.
	 */
}

module_init(airoha_ecnt_hook_init);
module_exit(airoha_ecnt_hook_exit);

MODULE_DESCRIPTION("Airoha Econet hook framework");
MODULE_LICENSE("GPL");
