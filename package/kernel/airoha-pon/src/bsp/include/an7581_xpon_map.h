/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_XPON_MAP_H
#define _AN7581_XPON_MAP_H

/* OEM register windows, in legacy offset order: GPON, XG-PON, EPON.
 * Keep the actual lengths, not the vendor accessor's assumed 4 KiB windows.
 */
#define AN7581_XPON_BANKS 3
static const unsigned int an7581_xpon_sizes[AN7581_XPON_BANKS] = {
	0x3e8, 0xff8, 0x23c,
};

static inline int an7581_xpon_decode(unsigned int reg, unsigned int *offset)
{
	unsigned int bank, off;

	if ((reg & 3) || reg < 0x4000 || reg >= 0x7000)
		return -1;
	bank = (reg >> 12) - 4;
	off = reg & 0xfff;
	if (off > an7581_xpon_sizes[bank] - sizeof(unsigned int))
		return -1;
	*offset = off;
	return bank;
}

#endif
