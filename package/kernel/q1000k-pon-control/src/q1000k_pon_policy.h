/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PON_POLICY_H
#define _Q1000K_PON_POLICY_H
/* Policy is selected once per driver lifetime, before any GPIO/I2C access.
 * An old immutable-inhibit DT can never be upgraded to TX by a parameter.
 */
static inline int q1000k_pon_tx_policy(bool hard_inhibit, bool validation,
				     bool allow_tx, bool *inhibit)
{
	if (allow_tx && (!validation || hard_inhibit))
		return -EACCES;
	*inhibit = hard_inhibit || (validation && !allow_tx);
	return 0;
}
#endif
