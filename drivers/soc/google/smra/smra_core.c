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
#include "smra_procfs.h"
#include "smra_sysfs.h"

static DEFINE_RWLOCK(smra_rwlock);
static bool smra_enable = false;
static LIST_HEAD(smra_targets_list);

static struct kmem_cache *smra_metadata_cachep;

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

/*
 * Protected by target->buf_lock, This API is used to make a separate copy of
 * the original recording buffer so that we can work with the data at ease. (
 * E.g. no need for spin_lock, free to allocate memory with arbitrary flags)
 */
static void smra_buffer_copy(struct smra_info_buffer *dst,
			     struct smra_info_buffer *src)
{
	dst->cur = src->cur;
	dst->size = src->size;
	memcpy(dst->fault_info, src->fault_info,
	       src->size * sizeof(struct smra_fault_info));
}

/*
 * Helper to create new metadata from @info. The d_path() API is used to
 * transfer struct *file to the actual readable path.
 */
static struct smra_metadata *new_metadata_from_info(struct smra_fault_info *info)
{
	struct smra_metadata *metadata;
	char *path;

	metadata = kmem_cache_alloc(smra_metadata_cachep, GFP_KERNEL);
	if (!metadata)
		return ERR_PTR(-ENOMEM);

	/*
	 * d_path() will return error code if the filepath is too long.
	 * If the file is deleted, the path name will be prefixed with
	 * "(deleted)", which would be later filtered by the smra library.
	 */
	path = d_path(&info->file->f_path, metadata->buf, MAX_PATH_LEN);
	if (IS_ERR(path)) {
		kfree(metadata);
		return ERR_CAST(path);
	}

	metadata->offset = info->offset;
	metadata->time = info->time;
	metadata->path = path;
	return metadata;
}

/*
 * Post-process the trace to generate human-readable metadata.
 *
 * context: This function is used when recording is stop and all pending
 * page faults are finished recorded. When post-processing, @buf is passed with
 * a separate copy of the original buffers. Hence no need to hold the
 * target->buf_lock and we are allowed to allocate memory with sleepable flags.
 */
static int do_post_processing(struct smra_info_buffer *buf,
			      struct list_head *footprint)
{
	struct smra_metadata *metadata, *next;
	int i, err;

	if (buf->cur == 0) {
		pr_warn("Receive empty buffer, nothing to be processed\n");
		return 0;
	}

	if (buf->cur >= buf->size)
		pr_warn("Buffer is too small, please consider recording "
			"again with larger buffer\n");

	for (i = 0; i < buf->cur; i++) {
		metadata = new_metadata_from_info(&buf->fault_info[i]);
		if (IS_ERR(metadata)) {
			err = PTR_ERR(metadata);
			goto cleanup;
		}
		list_add_tail(&metadata->list, footprint);
	}

	return 0;

cleanup:
	list_for_each_entry_safe(metadata, next, footprint, list) {
		list_del(&metadata->list);
		kfree(metadata);
	}
	return err;
}

void smra_post_processing_cleanup(struct list_head footprints[], int nr_targets)
{
	struct smra_metadata *metadata, *next;
	int i;

	for (i = 0; i < nr_targets; i++) {
		list_for_each_entry_safe(metadata, next, &footprints[i], list) {
			list_del(&metadata->list);
			kfree(metadata);
		}
	}
}

int smra_post_processing(pid_t target_pids[], int nr_targets, int buffer_size,
			 struct list_head footprints[])
{
	struct smra_target *target;
	int i = 0, err;

	struct smra_info_buffer *buf = smra_buffer_setup(buffer_size);
	if (!buf)
		return -ENOMEM;

	read_lock(&smra_rwlock);
	list_for_each_entry(target, &smra_targets_list, list) {
		BUG_ON(i >= nr_targets);
		BUG_ON(target->pid != target_pids[i]);

		spin_lock(&target->buf_lock);
		smra_buffer_copy(buf, target->buf);
		spin_unlock(&target->buf_lock);
		read_unlock(&smra_rwlock);

		pr_info("Start post processing pid %d\n", target_pids[i]);

		err = do_post_processing(buf, &footprints[i]);
		if (err) {
			smra_buffer_free(buf);
			smra_post_processing_cleanup(footprints, i);
			return err;
		}
		i++;
		read_lock(&smra_rwlock);
	}
	read_unlock(&smra_rwlock);

	return 0;
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

	smra_metadata_cachep = kmem_cache_create("smra_metadata",
						 sizeof(struct smra_metadata),
						 0, 0, NULL);
	if (!smra_metadata_cachep) {
		pr_err("Failed to create metadata cache\n");
		return -ENOMEM;
	}

	smra_procfs_init();
	smra_sysfs_init();

	return 0;
}

module_init(smra_init);

MODULE_LICENSE("GPL");
