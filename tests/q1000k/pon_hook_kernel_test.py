#!/usr/bin/env python3
"""Generate a UML-only test of the production ECNT registry with Linux RCU."""
from test_pon_hooks import BSP
from test_pon_hook_lifecycle import function

header = (BSP / 'include/ecnt_hook/ecnt_hook.h').read_text()
types = header[header.index('typedef enum {'):header.index(
    '/************************************************************************', header.index('struct ecnt_hook_ops {'))]
source = (BSP / 'core/ecnt_hook.c').read_text(errors='replace')
production = ''.join(function(source, name) for name in [
    'ecnt_iterate', '__ECNT_HOOK', 'ecnt_hook_is_registered',
    'set_ecnt_hookfn_execute_or_not', 'ecnt_register_hook',
    'ecnt_unregister_hook', 'ecnt_ops_unregister', 'ecnt_hook_init'])
print(r'''
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#ifndef CONFIG_UML
#error This test module is for a disposable UML guest only.
#endif
#define __IMEM
#define ECNT_MAX_SUBTYPE 8
#define ECNT_REGISTER_FAIL -1
#define ECNT_REGISTER_SUCCESS 0
''')
print(types)
print(r'''
static DEFINE_MUTEX(ecnt_hook_mutex);
static unsigned int hook_id;
static struct list_head ecnt_hooks[ECNT_NUM_MAINTYPE][ECNT_MAX_SUBTYPE];
struct ecnt_data { int unused; };
''')
print(production)
print(r'''
static atomic_t calls = ATOMIC_INIT(0);
static ecnt_ret_val callback(struct ecnt_data *data)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "ECNT callback outside RCU");
	atomic_inc(&calls);
	return ECNT_CONTINUE;
}
static int reader(void *unused)
{
	struct ecnt_data data = {0};
	while (!kthread_should_stop()) {
		__ECNT_HOOK(ECNT_QDMA_WAN, 0, &data);
		cond_resched();
	}
	return 0;
}
static struct ecnt_hook_ops node = {
	.maintype = ECNT_QDMA_WAN, .subtype = 0,
	.hookfn = callback, .is_execute = 1, .name = "uml-test",
};

#define CHECK(expr) do { if (!(expr)) { \
	pr_err("Q1000K_PON_HOOK_KERNEL_FAIL line=%d: %s\n", __LINE__, #expr); \
	ret = -EINVAL; goto out; } } while (0)
static int __init pon_hook_test_init(void)
{
	struct task_struct *task = NULL;
	struct ecnt_hook_ops zero = {0};
	int ret = 0, i;

	if (!strstr(init_utsname()->release, "-q1000k-pon-hook-test"))
		return -EPERM;
	ecnt_hook_init();
	ecnt_unregister_hook(NULL);
	ecnt_unregister_hook(&zero);
	CHECK(ecnt_register_hook(&zero) == ECNT_REGISTER_FAIL);
	CHECK(!ecnt_ops_unregister(U32_MAX, 0, 1));
	CHECK(!ecnt_ops_unregister(0, U32_MAX, 1));
	CHECK(ecnt_register_hook(&node) == ECNT_REGISTER_SUCCESS);
	CHECK(ecnt_register_hook(&node) == ECNT_REGISTER_FAIL);
	CHECK(set_ecnt_hookfn_execute_or_not(node.maintype, 0, node.hook_id, 0));
	CHECK(!ecnt_hook_is_registered(node.maintype, 0));
	CHECK(set_ecnt_hookfn_execute_or_not(node.maintype, 0, node.hook_id, 1));
	task = kthread_run(reader, NULL, "pon-hook-test");
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		task = NULL;
		goto out;
	}
	/* Ensure real dispatch runs, including on a uniprocessor UML kernel. */
	msleep(20);
	CHECK(atomic_read(&calls) > 0);
	for (i = 0; i < 100; i++) {
		unsigned int old_id = node.hook_id;
		if (i & 1)
			CHECK(ecnt_ops_unregister(node.maintype, 0, old_id) == 1);
		else
			ecnt_unregister_hook(&node);
		CHECK(!node.list.next && !node.list.prev);
		CHECK(!ecnt_hook_is_registered(node.maintype, 0));
		CHECK(!ecnt_ops_unregister(node.maintype, 0, old_id));
		ecnt_unregister_hook(&node);
		CHECK(ecnt_register_hook(&node) == ECNT_REGISTER_SUCCESS);
		CHECK(node.hook_id > old_id);
		cond_resched();
	}
out:
	if (task)
		kthread_stop(task);
	ecnt_unregister_hook(&node);
	if (!ret)
		pr_info("Q1000K_PON_HOOK_KERNEL_PASS calls=%d cycles=100\n", atomic_read(&calls));
	return ret;
}
static void __exit pon_hook_test_exit(void) {}
module_init(pon_hook_test_init);
module_exit(pon_hook_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K ECNT registration/RCU lifetime test");
''')
