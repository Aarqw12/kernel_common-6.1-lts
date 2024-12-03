// SPDX-License-Identifier: GPL-2.0
/*
 * SMRA (Smart Readahead)
 *
 */

#define pr_fmt(fmt) "smra_core: " fmt

#include <linux/file.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>

#include <trace/hooks/mm.h>

#include "smra_core.h"
#include "smra_sysfs.h"

static DEFINE_RWLOCK(smra_rwlock);
static bool smra_enable = false;
static LIST_HEAD(smra_targets_list);

/* Protected by target->buf_lock */
static struct smra_info_buffer *smra_buffer_setup(ssize_t size)
{
	struct smra_info_buffer *buf;

	buf = kmalloc(sizeof(struct smra_info_buffer), GFP_KERNEL);
	if (!buf) {
		pr_err("Failed to allocate smra_info_buffer\n");
		return NULL;
	}

	buf->fault_info = kmalloc_array(size, sizeof(struct smra_fault_info),
				  GFP_KERNEL);
	if (!buf->fault_info) {
		kfree(buf);
		pr_err("Failed to allocate info array for smra_info_buffer\n");
		return NULL;
	}

	buf->cur = 0;
	buf->size = size;
	return buf;
}

/* Protected by target->buf_lock */
static void smra_buffer_free(struct smra_info_buffer *buf)
{
	kfree(buf->fault_info);
	kfree(buf);
}

/* Setup target pids and their buffers for recording */
int smra_setup(pid_t target_pids[], int nr_targets, int buffer_size)
{
	int i;
	struct smra_target *target;

	for (i = 0; i < nr_targets; i++) {
		target = kmalloc(sizeof(struct smra_target), GFP_KERNEL);
		if (!target)
			goto cleanup;
		target->buf = smra_buffer_setup(buffer_size);
		if (!target->buf) {
			kfree(target);
			goto cleanup;
		}
		target->pid = target_pids[i];
		spin_lock_init(&target->buf_lock);
		write_lock(&smra_rwlock);
		list_add_tail(&target->list, &smra_targets_list);
		write_unlock(&smra_rwlock);
	}

	return 0;

cleanup:
	write_lock(&smra_rwlock);
	list_for_each_entry(target, &smra_targets_list, list) {
		list_del(&target->list);
		smra_buffer_free(target->buf);
		kfree(target);
	}
	write_unlock(&smra_rwlock);
	return -ENOMEM;
}

void smra_start(void)
{
	write_lock(&smra_rwlock);
	smra_enable = true;
	write_unlock(&smra_rwlock);
}

void smra_stop(void)
{
	write_lock(&smra_rwlock);
	smra_enable = false;
	write_unlock(&smra_rwlock);
}

void smra_reset(void)
{
	int i;
	struct smra_target *target, *next;

	write_lock(&smra_rwlock);
	list_for_each_entry_safe(target, next, &smra_targets_list, list) {
		for (i = 0; i < target->buf->cur; i++)
			fput(target->buf->fault_info[i].file);
		smra_buffer_free(target->buf);
		list_del(&target->list);
		kfree(target);
	}
	write_unlock(&smra_rwlock);
}

/*
 * Helper to find target corresponds to @pid. Protected by
 * read_lock(&smra_rwlock)
 */
static struct smra_target *find_target(pid_t pid)
{
	struct smra_target *target;

	list_for_each_entry(target, &smra_targets_list, list) {
		if (target->pid == pid)
			return target;
	}

	return NULL;
}

static void rvh_do_read_fault(void *data, struct file *file, pgoff_t pgoff,
			      unsigned long *fault_around_bytes)
{
	int cur;
	pid_t tgid;
	struct smra_target *target;

	read_lock(&smra_rwlock);
	/*
	 * "Special" VMA mappings can enter the do_read_fault() path
	 * with file being NULL. Ex. vdso and uprobe.
	 */
	if (!smra_enable || !file)
		goto out;

	tgid = task_tgid_nr(current);
	target = find_target(tgid);
	if (!target)
		goto out;

	spin_lock(&target->buf_lock);
	if (target->buf->cur >= target->buf->size) {
		spin_unlock(&target->buf_lock);
		goto out;
	}
	/*
	 * Should fput() when users reset or restart recording.
	 * TODO: Investigate how we can remove this get_file() in the
	 * page fault path.
	 */
	get_file(file);
	cur = target->buf->cur;
	target->buf->fault_info[cur].file = file;
	target->buf->fault_info[cur].offset = pgoff;
	target->buf->fault_info[cur].time = ktime_get();
	target->buf->cur++;
	spin_unlock(&target->buf_lock);

out:
	read_unlock(&smra_rwlock);
	return;
}

static int smra_vh_init(void)
{
	int ret;

	ret = register_trace_android_rvh_do_read_fault(rvh_do_read_fault, NULL);
	if (ret)
		return ret;

	return 0;
}

int __init smra_init(void)
{
	int err;

	err = smra_vh_init();
	if (err) {
		pr_err("Failed to initialize vendor hooks, error %d\n", err);
		return err;
	}

	smra_sysfs_init();

	return 0;
}

module_init(smra_init);

MODULE_LICENSE("GPL");
