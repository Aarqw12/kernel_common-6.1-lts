/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SMRA_CORE_H__
#define __SMRA_CORE_H__

#include <linux/sched.h>
#include <linux/types.h>

#define MAX_PATH_LEN 256

/*
 * @smra_fault_info: information captured during do_read_fault()
 *
 * When smra_fault_info is pushed into buffer, get_file() is used to avoid
 * @file being released when we are still recording. It will be fput() later
 * when post-processing is done.
 */
struct smra_fault_info {
	struct file *file;
	pgoff_t offset;
	ktime_t time;
};

struct smra_info_buffer {
	struct smra_fault_info *fault_info;
	int cur;
	ssize_t size;
};

struct smra_target {
	pid_t pid;
	spinlock_t buf_lock;
	struct smra_info_buffer *buf;
	struct list_head list;
};

int smra_setup(pid_t target_pids[], int nr_targets, int buffer_size);
void smra_start(void);
void smra_stop(void);
void smra_reset(void);

#endif
