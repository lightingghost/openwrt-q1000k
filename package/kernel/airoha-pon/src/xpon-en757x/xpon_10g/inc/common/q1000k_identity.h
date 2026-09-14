/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PON_IDENTITY_H_
#define _Q1000K_PON_IDENTITY_H_
#if defined(TCSUPPORT_CPU_EN7581) && !defined(TCSUPPORT_CPU_AN7583)
#define Q1000K_PON_IDENTITY
int q1000k_pon_identity_init(void);
int q1000k_pon_get_serial(unsigned char *serial, int len);
int q1000k_pon_get_registration(unsigned char *registration, int len);
int get_ethaddr(unsigned char *addr, int len);
char get_onutype(void);
#endif
#endif
