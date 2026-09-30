// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * The JSON status document, rendered to userspace, to a seq_file and to the
 * kernel log. See jitterentropy_status.h.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "jitterentropy.h"
#include "jitterentropy_status.h"
#include "jitterentropy_uapi.h"

/*
 * Serialize the document of @ec into @buf, which holds JENT_STATUS_MAX_LEN
 * bytes. Holds @lock so a concurrent read path can neither reallocate the
 * collector on health-test recovery nor mutate the state being serialized.
 */
static int jent_status_render(struct mutex *lock, struct rand_data **ec,
			      char *buf)
{
	int ret;

	if (mutex_lock_interruptible(lock))
		return -ERESTARTSYS;
	ret = jent_status(*ec, buf, JENT_STATUS_MAX_LEN);
	mutex_unlock(lock);

	return ret ? -EIO : 0;
}

long jent_status_to_user(struct mutex *lock, struct rand_data **ec,
			 void __user *arg)
{
	struct jent_status_ioctl status;
	char *buf;
	size_t slen;
	long ret;

	if (copy_from_user(&status, arg, sizeof(status)))
		return -EFAULT;

	buf = kvzalloc(JENT_STATUS_MAX_LEN, GFP_KERNEL_ACCOUNT);
	if (!buf)
		return -ENOMEM;

	ret = jent_status_render(lock, ec, buf);
	if (ret)
		goto out;

	/* Number of bytes to copy out, including the terminating NUL. */
	slen = strlen(buf) + 1;

	if (status.length < slen) {
		/* Buffer too small: report the required size to userspace. */
		status.length = slen;
		if (copy_to_user(arg, &status, sizeof(status)))
			ret = -EFAULT;
		else
			ret = -EOVERFLOW;
		goto out;
	}

	if (copy_to_user(u64_to_user_ptr(status.buf), buf, slen)) {
		ret = -EFAULT;
		goto out;
	}

	status.length = slen;
	if (copy_to_user(arg, &status, sizeof(status))) {
		ret = -EFAULT;
		goto out;
	}

	ret = 0;

out:
	kvfree(buf);
	return ret;
}

int jent_status_seq_show(struct seq_file *m, struct mutex *lock,
			 struct rand_data **ec)
{
	char *buf;
	int ret;

	buf = kvzalloc(JENT_STATUS_MAX_LEN, GFP_KERNEL_ACCOUNT);
	if (!buf)
		return -ENOMEM;

	ret = jent_status_render(lock, ec, buf);
	if (!ret)
		seq_puts(m, buf);

	kvfree(buf);
	return ret;
}

int jent_status_to_log(struct mutex *lock, struct rand_data **ec)
{
	char *line, *p;
	char *buf;
	int ret;

	buf = kvzalloc(JENT_STATUS_MAX_LEN, GFP_KERNEL_ACCOUNT);
	if (!buf)
		return -ENOMEM;

	/*
	 * Uninterruptible, unlike the two renderers above: this one has no
	 * caller waiting on a verdict that a signal should cut short, and a
	 * NULL @lock means the caller is already holding it.
	 */
	if (lock)
		mutex_lock(lock);
	ret = jent_status(*ec, buf, JENT_STATUS_MAX_LEN);
	if (lock)
		mutex_unlock(lock);

	if (ret)
		goto out;

	/*
	 * printk truncates records at about 1 kB; emit the multi-line JSON
	 * status line by line so it arrives intact. Any rate limiting covers
	 * the status as a whole, not a line, so an emitted status is never cut
	 * short - hence it sits at the call site rather than here.
	 */
	p = buf;
	while ((line = strsep(&p, "\n")) != NULL)
		if (*line)
			pr_notice("%s\n", line);

out:
	kvfree(buf);
	return ret;
}
