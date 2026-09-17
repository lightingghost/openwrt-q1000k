/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q1000K_EN7573_MPD_H
#define Q1000K_EN7573_MPD_H

/* Raw observations, never calibrated optical power. Rows are before monitor
 * selection, selected after the OEM 50+5 ms waits, and after restoration.
 * Values: 33c LE32, f0 LE16, 66/64/6a BE16, 3c4/3c8/3e0/488 LE32.
 * valid[] is a per-field bitmap; zero in an invalid slot is not a sample.
 */
#define EN7573_MPD_FIELDS 9
struct en7573_mpd {
	u32 saved[3], selected[3]; /* 130, 208, 120; selected_valid is a bitmap */
	u32 values[3][EN7573_MPD_FIELDS], valid[3], selected_valid;
	int sample_error[3], restore_error;
	bool active, restored;
};
#endif
