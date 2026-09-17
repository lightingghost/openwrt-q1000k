/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q1000K_EN7573_OUTPUT_H
#define Q1000K_EN7573_OUTPUT_H

/* Read order brackets the asynchronous mailbox with hardware TSSI and BEN.
 * A bus read is fresh; neither it nor this struct proves ADC conversion age.
 * All registers are read-only here. Scalar order is part of output_version=1.
 */
#define EN7573_OUTPUT_FIELDS 30
struct en7573_output_sample {
	u32 values[EN7573_OUTPUT_FIELDS], valid;
	int error, board_disabled;
};
struct en7573_output_hold {
	u32 saved[3]; /* 130, 208, 120; only measurement fields are restored */
	bool saved_valid;
};
#endif
