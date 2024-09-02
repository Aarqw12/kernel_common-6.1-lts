/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../../include/pixel_mm_hint.h"

// atomic type for no tearing issue
static atomic_long_t mm_hint_enable = ATOMIC_INIT(0);
static atomic_long_t mm_hint_mode = ATOMIC_INIT(0);

static int mm_hint_enable_set(const char *val, const struct kernel_param *kp)
{
	bool is_active;

	if (kstrtobool(val, &is_active)) {
		pr_err("%s: mm_hint_enable parse error", __func__);
		return -EINVAL;
	}

	atomic_long_set(&mm_hint_enable, is_active);

	return 0;
}

static int mm_hint_enable_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit_at(buf, 0, "%lu\n", atomic_long_read(&mm_hint_enable));
}

enum mm_hint_mode get_mm_hint_mode(void)
{
	if (atomic_long_read(&mm_hint_enable))
		return atomic_long_read(&mm_hint_mode);
	else
		return MM_HINT_NONE;
}
EXPORT_SYMBOL_GPL(get_mm_hint_mode);

static int mm_hint_mode_set(const char *val, const struct kernel_param *kp)
{
	unsigned long value;

	if (kstrtoul(val, 10, &value)) {
		pr_err("%s: mm_hint_mode parse error", __func__);
		return -EINVAL;
	}

	if (value < MM_HINT_NUM)
		atomic_long_set(&mm_hint_mode, value);
	else
		return -EINVAL;

	return 0;
}

static int mm_hint_mode_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit_at(buf, 0, "%lu\n", atomic_long_read(&mm_hint_mode));
}

static const struct kernel_param_ops mm_hint_enable_ops = {
	.set = mm_hint_enable_set,
	.get = mm_hint_enable_get,
};

static const struct kernel_param_ops  mm_hint_mode_ops = {
	.set = mm_hint_mode_set,
	.get = mm_hint_mode_get,
};

module_param_cb(mm_hint_enable, &mm_hint_enable_ops, NULL, 0644);
module_param_cb(mm_hint_mode, &mm_hint_mode_ops, NULL, 0644);
