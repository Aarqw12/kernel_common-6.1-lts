/* SPDX-License-Identifier: GPL-2.0 */

#ifndef PIXEL_MM_HINT_H
#define PIXEL_MM_HINT_H

enum mm_hint_mode {
	MM_HINT_NONE,
	MM_HINT_APP_LAUNCH,
	MM_HINT_CAMERA_LAUNCH,
	MM_HINT_NUM
};

enum mm_hint_mode get_mm_hint_mode(void);
bool is_file_cache_enough(void);

#endif	/* PIXEL_MM_HINT_H */