/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (C) 2008-2010 Lawrence Livermore National Security, LLC.
 * Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 * Rewritten for Linux by Brian Behlendorf <behlendorf1@llnl.gov>.
 * LLNL-CODE-403049.
 *
 * ZFS volume emulation driver.
 *
 * Makes a DMU object look like a volume of arbitrary size, up to 2^64 bytes.
 * Volumes are accessed through the symbolic links named:
 *
 * /dev/<pool_name>/<dataset_name>
 *
 * Volumes are persistent through reboot and module load.  No user command
 * needs to be run before opening and using a device.
 *
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2016 Actifio, Inc. All rights reserved.
 * Copyright (c) 2012, 2019 by Delphix. All rights reserved.
 */

/*
 * Note on locking of zvol state structures.
 *
 * These structures are used to maintain internal state used to emulate block
 * devices on top of zvols. In particular, management of device minor number
 * operations - create, remove, rename, and set_snapdev - involves access to
 * these structures. The zvol_state_lock is primarily used to protect the
 * zvol_state_list. The zv->zv_state_lock is used to protect the contents
 * of the zvol_state_t structures, as well as to make sure that when the
 * time comes to remove the structure from the list, it is not in use, and
 * therefore, it can be taken off zvol_state_list and freed.
 *
 * The zv_suspend_lock was introduced to allow for suspending I/O to a zvol,
 * e.g. for the duration of receive and rollback operations. This lock can be
 * held for significant periods of time. Given that it is undesirable to hold
 * mutexes for long periods of time, the following lock ordering applies:
 * - take zvol_state_lock if necessary, to protect zvol_state_list
 * - take zv_suspend_lock if necessary, by the code path in question
 * - take zv_state_lock to protect zvol_state_t
 *
 * The minor operations are issued to spa->spa_zvol_taskq queues, that are
 * single-threaded (to preserve order of minor operations), and are executed
 * through the zvol_task_cb that dispatches the specific operations. Therefore,
 * these operations are serialized per pool. Consequently, we can be certain
 * that for a given zvol, there is only one operation at a time in progress.
 * That is why one can be sure that first, zvol_state_t for a given zvol is
 * allocated and placed on zvol_state_list, and then other minor operations
 * for this zvol are going to proceed in the order of issue.
 *
 * It is also worth keeping in mind that once add_disk() is called, the zvol is
 * announced to the world, and zvol_open()/zvol_release() can be called at any
 * time. Incidentally, add_disk() itself calls zvol_open()->zvol_first_open()
 * and zvol_release()->zvol_last_close() directly as well.
 */

#include <sys/dataset_kstats.h>
#include <sys/dbuf.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_dir.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/zil_impl.h>
#include <sys/dmu_tx.h>
#include <sys/zio.h>
#include <sys/zfs_rlock.h>
#include <sys/spa_impl.h>
#include <sys/zvol.h>
#include <sys/zvol_impl.h>

#include <linux/blkdev_compat.h>
#include <linux/task_io_accounting_ops.h>

unsigned int zvol_major = ZVOL_MAJOR;
unsigned int zvol_request_sync = 0;
unsigned int zvol_prefetch_bytes = (128 * 1024);
unsigned long zvol_max_discard_blocks = 16384;


static struct ida zvol_ida;

typedef struct zv_request {
	zvol_state_t	*zv;
	struct bio	*bio;
	locked_range_t	*lr;
} zv_request_t;

/*
 * Given a path, return TRUE if path is a ZVOL.
 */
boolean_t
zvol_is_zvol(const char *device)
{
	struct block_device *bdev;
	unsigned int major;

	bdev = vdev_lookup_bdev(device);
	if (IS_ERR(bdev))
		return (B_FALSE);

	major = MAJOR(bdev->bd_dev);
	bdput(bdev);

	if (major == zvol_major)
		return (B_TRUE);

	return (B_FALSE);
}

static void
uio_from_bio(uio_t *uio, struct bio *bio)
{
	uio->uio_bvec = &bio->bi_io_vec[BIO_BI_IDX(bio)];
	uio->uio_iovcnt = bio->bi_vcnt - BIO_BI_IDX(bio);
	uio->uio_loffset = BIO_BI_SECTOR(bio) << 9;
	uio->uio_segflg = UIO_BVEC;
	uio->uio_limit = MAXOFFSET_T;
	uio->uio_resid = BIO_BI_SIZE(bio);
	uio->uio_skip = BIO_BI_SKIP(bio);
}

static void
zvol_write(void *arg)
{
	int error = 0;

	zv_request_t *zvr = arg;
	struct bio *bio = zvr->bio;
	uio_t uio = { { 0 }, 0 };
	uio_from_bio(&uio, bio);

	zvol_state_t *zv = zvr->zv;
	ASSERT(zv && zv->zv_open_count > 0);
	ASSERT(zv->zv_zilog != NULL);

	ssize_t start_resid = uio.uio_resid;
	unsigned long start_jif = jiffies;
	blk_generic_start_io_acct(zv->zv_queue, WRITE, bio_sectors(bio),
	    &zv->zv_disk->part0);

	boolean_t sync =
	    bio_is_fua(bio) || zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS;

	uint64_t volsize = zv->zv_volsize;
	while (uio.uio_resid > 0 && uio.uio_loffset < volsize) {
		uint64_t bytes = MIN(uio.uio_resid, DMU_MAX_ACCESS >> 1);
		uint64_t off = uio.uio_loffset;
		dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);

		if (bytes > volsize - off)	/* don't write past the end */
			bytes = volsize - off;

		dmu_tx_hold_write(tx, ZVOL_OBJ, off, bytes);

		/* This will only fail for ENOSPC */
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			break;
		}
		error = dmu_write_uio_dnode(zv->zv_dn, &uio, bytes, tx);
		if (error == 0) {
			zvol_log_write(zv, tx, off, bytes, sync);
		}
		dmu_tx_commit(tx);

		if (error)
			break;
	}
	rangelock_exit(zvr->lr);

	int64_t nwritten = start_resid - uio.uio_resid;
	dataset_kstats_update_write_kstats(&zv->zv_kstat, nwritten);
	task_io_account_write(nwritten);

	if (sync)
		zil_commit(zv->zv_zilog, ZVOL_OBJ);

	rw_exit(&zv->zv_suspend_lock);
	blk_generic_end_io_acct(zv->zv_queue, WRITE, &zv->zv_disk->part0,
	    start_jif);
	BIO_END_IO(bio, -error);
	kmem_free(zvr, sizeof (zv_request_t));
}

static void
zvol_discard(void *arg)
{
	zv_request_t *zvr = arg;
	struct bio *bio = zvr->bio;
	zvol_state_t *zv = zvr->zv;
	uint64_t start = BIO_BI_SECTOR(bio) << 9;
	uint64_t size = BIO_BI_SIZE(bio);
	uint64_t end = start + size;
	boolean_t sync;
	int error = 0;
	dmu_tx_t *tx;
	unsigned long start_jif;

	ASSERT(zv && zv->zv_open_count > 0);
	ASSERT(zv->zv_zilog != NULL);

	start_jif = jiffies;
	blk_generic_start_io_acct(zv->zv_queue, WRITE, bio_sectors(bio),
	    &zv->zv_disk->part0);

	sync = bio_is_fua(bio) || zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS;

	if (end > zv->zv_volsize) {
		error = SET_ERROR(EIO);
		goto unlock;
	}

	/*
	 * Align the request to volume block boundaries when a secure erase is
	 * not required.  This will prevent dnode_free_range() from zeroing out
	 * the unaligned parts which is slow (read-modify-write) and useless
	 * since we are not freeing any space by doing so.
	 */
	if (!bio_is_secure_erase(bio)) {
		start = P2ROUNDUP(start, zv->zv_volblocksize);
		end = P2ALIGN(end, zv->zv_volblocksize);
		size = end - start;
	}

	if (start >= end)
		goto unlock;

	tx = dmu_tx_create(zv->zv_objset);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
	} else {
		zvol_log_truncate(zv, tx, start, size, B_TRUE);
		dmu_tx_commit(tx);
		error = dmu_free_long_range(zv->zv_objset,
		    ZVOL_OBJ, start, size);
	}
unlock:
	rangelock_exit(zvr->lr);

	if (error == 0 && sync)
		zil_commit(zv->zv_zilog, ZVOL_OBJ);

	rw_exit(&zv->zv_suspend_lock);
	blk_generic_end_io_acct(zv->zv_queue, WRITE, &zv->zv_disk->part0,
	    start_jif);
	BIO_END_IO(bio, -error);
	kmem_free(zvr, sizeof (zv_request_t));
}

static void
zvol_read(void *arg)
{
	int error = 0;

	zv_request_t *zvr = arg;
	struct bio *bio = zvr->bio;
	uio_t uio = { { 0 }, 0 };
	uio_from_bio(&uio, bio);

	zvol_state_t *zv = zvr->zv;
	ASSERT(zv && zv->zv_open_count > 0);

	ssize_t start_resid = uio.uio_resid;
	unsigned long start_jif = jiffies;
	blk_generic_start_io_acct(zv->zv_queue, READ, bio_sectors(bio),
	    &zv->zv_disk->part0);

	uint64_t volsize = zv->zv_volsize;
	while (uio.uio_resid > 0 && uio.uio_loffset < volsize) {
		uint64_t bytes = MIN(uio.uio_resid, DMU_MAX_ACCESS >> 1);

		/* don't read past the end */
		if (bytes > volsize - uio.uio_loffset)
			bytes = volsize - uio.uio_loffset;

		error = dmu_read_uio_dnode(zv->zv_dn, &uio, bytes);
		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
	}
	rangelock_exit(zvr->lr);

	int64_t nread = start_resid - uio.uio_resid;
	dataset_kstats_update_read_kstats(&zv->zv_kstat, nread);
	task_io_account_read(nread);

	rw_exit(&zv->zv_suspend_lock);
	blk_generic_end_io_acct(zv->zv_queue, READ, &zv->zv_disk->part0,
	    start_jif);
	BIO_END_IO(bio, -error);
	kmem_free(zvr, sizeof (zv_request_t));
}

static MAKE_REQUEST_FN_RET
zvol_request(struct request_queue *q, struct bio *bio)
{
	zvol_state_t *zv = q->queuedata;
	fstrans_cookie_t cookie = spl_fstrans_mark();
	uint64_t offset = BIO_BI_SECTOR(bio) << 9;
	uint64_t size = BIO_BI_SIZE(bio);
	int rw = bio_data_dir(bio);
	zv_request_t *zvr;

	if (bio_has_data(bio) && offset + size > zv->zv_volsize) {
		printk(KERN_INFO
		    "%s: bad access: offset=%llu, size=%lu\n",
		    zv->zv_disk->disk_name,
		    (long long unsigned)offset,
		    (long unsigned)size);

		BIO_END_IO(bio, -SET_ERROR(EIO));
		goto out;
	}

	if (rw == WRITE) {
		boolean_t need_sync = B_FALSE;

		if (unlikely(zv->zv_flags & ZVOL_RDONLY)) {
			BIO_END_IO(bio, -SET_ERROR(EROFS));
			goto out;
		}

		/*
		 * To be released in the I/O function. See the comment on
		 * rangelock_enter() below.
		 */
		rw_enter(&zv->zv_suspend_lock, RW_READER);

		/*
		 * Open a ZIL if this is the first time we have written to this
		 * zvol. We protect zv->zv_zilog with zv_suspend_lock rather
		 * than zv_state_lock so that we don't need to acquire an
		 * additional lock in this path.
		 */
		if (zv->zv_zilog == NULL) {
			rw_exit(&zv->zv_suspend_lock);
			rw_enter(&zv->zv_suspend_lock, RW_WRITER);
			if (zv->zv_zilog == NULL) {
				zv->zv_zilog = zil_open(zv->zv_objset,
				    zvol_get_data);
				zv->zv_flags |= ZVOL_WRITTEN_TO;
			}
			rw_downgrade(&zv->zv_suspend_lock);
		}

		/* bio marked as FLUSH need to flush before write */
		if (bio_is_flush(bio))
			zil_commit(zv->zv_zilog, ZVOL_OBJ);

		/* Some requests are just for flush and nothing else. */
		if (size == 0) {
			rw_exit(&zv->zv_suspend_lock);
			BIO_END_IO(bio, 0);
			goto out;
		}

		zvr = kmem_alloc(sizeof (zv_request_t), KM_SLEEP);
		zvr->zv = zv;
		zvr->bio = bio;

		/*
		 * To be released in the I/O function. Since the I/O functions
		 * are asynchronous, we take it here synchronously to make
		 * sure overlapped I/Os are properly ordered.
		 */
		zvr->lr = rangelock_enter(&zv->zv_rangelock, offset, size,
		    RL_WRITER);
		/*
		 * Sync writes and discards execute zil_commit() which may need
		 * to take a RL_READER lock on the whole block being modified
		 * via its zillog->zl_get_data(): to avoid circular dependency
		 * issues with taskq threads execute these requests
		 * synchronously here in zvol_request().
		 */
		need_sync = bio_is_fua(bio) ||
		    zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS;
		if (bio_is_discard(bio) || bio_is_secure_erase(bio)) {
			if (zvol_request_sync || need_sync ||
			    taskq_dispatch(zvol_taskq, zvol_discard, zvr,
			    TQ_SLEEP) == TASKQID_INVALID)
				zvol_discard(zvr);
		} else {
			if (zvol_request_sync || need_sync ||
			    taskq_dispatch(zvol_taskq, zvol_write, zvr,
			    TQ_SLEEP) == TASKQID_INVALID)
				zvol_write(zvr);
		}
	} else {
		/*
		 * The SCST driver, and possibly others, may issue READ I/Os
		 * with a length of zero bytes.  These empty I/Os contain no
		 * data and require no additional handling.
		 */
		if (size == 0) {
			BIO_END_IO(bio, 0);
			goto out;
		}

		zvr = kmem_alloc(sizeof (zv_request_t), KM_SLEEP);
		zvr->zv = zv;
		zvr->bio = bio;

		rw_enter(&zv->zv_suspend_lock, RW_READER);

		zvr->lr = rangelock_enter(&zv->zv_rangelock, offset, size,
		    RL_READER);
		if (zvol_request_sync || taskq_dispatch(zvol_taskq,
		    zvol_read, zvr, TQ_SLEEP) == TASKQID_INVALID)
			zvol_read(zvr);
	}

out:
	spl_fstrans_unmark(cookie);
#ifdef HAVE_MAKE_REQUEST_FN_RET_INT
	return (0);
#elif defined(HAVE_MAKE_REQUEST_FN_RET_QC)
	return (BLK_QC_T_NONE);
#endif
}

static int
zvol_open(struct block_device *bdev, fmode_t flag)
{
	zvol_state_t *zv;
	int error = 0;
	boolean_t drop_suspend = B_TRUE;

	rw_enter(&zvol_state_lock, RW_READER);
	/*
	 * Obtain a copy of private_data under the zvol_state_lock to make
	 * sure that either the result of zvol free code path setting
	 * bdev->bd_disk->private_data to NULL is observed, or zvol_free()
	 * is not called on this zv because of the positive zv_open_count.
	 */
	zv = bdev->bd_disk->private_data;
	if (zv == NULL) {
		rw_exit(&zvol_state_lock);
		return (SET_ERROR(-ENXIO));
	}

	mutex_enter(&zv->zv_state_lock);
	/*
	 * make sure zvol is not suspended during first open
	 * (hold zv_suspend_lock) and respect proper lock acquisition
	 * ordering - zv_suspend_lock before zv_state_lock
	 */
	if (zv->zv_open_count == 0) {
		if (!rw_tryenter(&zv->zv_suspend_lock, RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, RW_READER);
			mutex_enter(&zv->zv_state_lock);
			/* check to see if zv_suspend_lock is needed */
			if (zv->zv_open_count != 0) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	} else {
		drop_suspend = B_FALSE;
	}
	rw_exit(&zvol_state_lock);

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(zv->zv_open_count != 0 || RW_READ_HELD(&zv->zv_suspend_lock));

	if (zv->zv_open_count == 0) {
		error = zvol_first_open(zv, !(flag & FMODE_WRITE));
		if (error)
			goto out_mutex;
	}

	if ((flag & FMODE_WRITE) && (zv->zv_flags & ZVOL_RDONLY)) {
		error = -EROFS;
		goto out_open_count;
	}

	zv->zv_open_count++;

	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);

	check_disk_change(bdev);

	return (0);

out_open_count:
	if (zv->zv_open_count == 0)
		zvol_last_close(zv);

out_mutex:
	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	if (error == -ERESTARTSYS)
		schedule();

	return (SET_ERROR(error));
}

#ifdef HAVE_BLOCK_DEVICE_OPERATIONS_RELEASE_VOID
static void
#else
static int
#endif
zvol_release(struct gendisk *disk, fmode_t mode)
{
	zvol_state_t *zv;
	boolean_t drop_suspend = B_TRUE;

	rw_enter(&zvol_state_lock, RW_READER);
	zv = disk->private_data;

	mutex_enter(&zv->zv_state_lock);
	ASSERT(zv->zv_open_count > 0);
	/*
	 * make sure zvol is not suspended during last close
	 * (hold zv_suspend_lock) and respect proper lock acquisition
	 * ordering - zv_suspend_lock before zv_state_lock
	 */
	if (zv->zv_open_count == 1) {
		if (!rw_tryenter(&zv->zv_suspend_lock, RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, RW_READER);
			mutex_enter(&zv->zv_state_lock);
			/* check to see if zv_suspend_lock is needed */
			if (zv->zv_open_count != 1) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	} else {
		drop_suspend = B_FALSE;
	}
	rw_exit(&zvol_state_lock);

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(zv->zv_open_count != 1 || RW_READ_HELD(&zv->zv_suspend_lock));

	zv->zv_open_count--;
	if (zv->zv_open_count == 0)
		zvol_last_close(zv);

	mutex_exit(&zv->zv_state_lock);

	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);

#ifndef HAVE_BLOCK_DEVICE_OPERATIONS_RELEASE_VOID
	return (0);
#endif
}

static int
zvol_ioctl(struct block_device *bdev, fmode_t mode,
    unsigned int cmd, unsigned long arg)
{
	zvol_state_t *zv = bdev->bd_disk->private_data;
	int error = 0;

	ASSERT3U(zv->zv_open_count, >, 0);

	switch (cmd) {
	case BLKFLSBUF:
		fsync_bdev(bdev);
		invalidate_bdev(bdev);
		rw_enter(&zv->zv_suspend_lock, RW_READER);

		if (!(zv->zv_flags & ZVOL_RDONLY))
			txg_wait_synced(dmu_objset_pool(zv->zv_objset), 0);

		rw_exit(&zv->zv_suspend_lock);
		break;

	case BLKZNAME:
		mutex_enter(&zv->zv_state_lock);
		error = copy_to_user((void *)arg, zv->zv_name, MAXNAMELEN);
		mutex_exit(&zv->zv_state_lock);
		break;

	default:
		error = -ENOTTY;
		break;
	}

	return (SET_ERROR(error));
}

#ifdef CONFIG_COMPAT
static int
zvol_compat_ioctl(struct block_device *bdev, fmode_t mode,
    unsigned cmd, unsigned long arg)
{
	return (zvol_ioctl(bdev, mode, cmd, arg));
}
#else
#define	zvol_compat_ioctl	NULL
#endif

/*
 * Linux 2.6.38 preferred interface.
 */
#ifdef HAVE_BLOCK_DEVICE_OPERATIONS_CHECK_EVENTS
static unsigned int
zvol_check_events(struct gendisk *disk, unsigned int clearing)
{
	unsigned int mask = 0;

	rw_enter(&zvol_state_lock, RW_READER);

	zvol_state_t *zv = disk->private_data;
	if (zv != NULL) {
		mutex_enter(&zv->zv_state_lock);
		mask = zv->zv_changed ? DISK_EVENT_MEDIA_CHANGE : 0;
		zv->zv_changed = 0;
		mutex_exit(&zv->zv_state_lock);
	}

	rw_exit(&zvol_state_lock);

	return (mask);
}
#else
static int zvol_media_changed(struct gendisk *disk)
{
	int changed = 0;

	rw_enter(&zvol_state_lock, RW_READER);

	zvol_state_t *zv = disk->private_data;
	if (zv != NULL) {
		mutex_enter(&zv->zv_state_lock);
		changed = zv->zv_changed;
		zv->zv_changed = 0;
		mutex_exit(&zv->zv_state_lock);
	}

	rw_exit(&zvol_state_lock);

	return (changed);
}
#endif

static int zvol_revalidate_disk(struct gendisk *disk)
{
	rw_enter(&zvol_state_lock, RW_READER);

	zvol_state_t *zv = disk->private_data;
	if (zv != NULL) {
		mutex_enter(&zv->zv_state_lock);
		set_capacity(zv->zv_disk, zv->zv_volsize >> SECTOR_BITS);
		mutex_exit(&zv->zv_state_lock);
	}

	rw_exit(&zvol_state_lock);

	return (0);
}

int
zvol_os_update_volsize(zvol_state_t *zv, uint64_t volsize)
{

	revalidate_disk(zv->zv_disk);
	return (0);
}

void
zvol_os_clear_private(zvol_state_t *zv)
{
	/*
	 * Cleared while holding zvol_state_lock as a writer
	 * which will prevent zvol_open() from opening it.
	 */
	zv->zv_disk->private_data = NULL;
}

/*
 * Provide a simple virtual geometry for legacy compatibility.  For devices
 * smaller than 1 MiB a small head and sector count is used to allow very
 * tiny devices.  For devices over 1 Mib a standard head and sector count
 * is used to keep the cylinders count reasonable.
 */
static int
zvol_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	zvol_state_t *zv = bdev->bd_disk->private_data;
	sector_t sectors;

	ASSERT3U(zv->zv_open_count, >, 0);

	sectors = get_capacity(zv->zv_disk);

	if (sectors > 2048) {
		geo->heads = 16;
		geo->sectors = 63;
	} else {
		geo->heads = 2;
		geo->sectors = 4;
	}

	geo->start = 0;
	geo->cylinders = sectors / (geo->heads * geo->sectors);

	return (0);
}

static struct kobject *
zvol_probe(dev_t dev, int *part, void *arg)
{
	zvol_state_t *zv;
	struct kobject *kobj;

	zv = zvol_find_by_dev(dev);
	kobj = zv ? get_disk_and_module(zv->zv_disk) : NULL;
	ASSERT(zv == NULL || MUTEX_HELD(&zv->zv_state_lock));
	if (zv)
		mutex_exit(&zv->zv_state_lock);

	return (kobj);
}

static struct block_device_operations zvol_ops = {
	.open			= zvol_open,
	.release		= zvol_release,
	.ioctl			= zvol_ioctl,
	.compat_ioctl		= zvol_compat_ioctl,
#ifdef HAVE_BLOCK_DEVICE_OPERATIONS_CHECK_EVENTS
	.check_events		= zvol_check_events,
#else
	.media_changed		= zvol_media_changed,
#endif
	.revalidate_disk	= zvol_revalidate_disk,
	.getgeo			= zvol_getgeo,
	.owner			= THIS_MODULE,
};

/*
 * Allocate memory for a new zvol_state_t and setup the required
 * request queue and generic disk structures for the block device.
 */
static zvol_state_t *
zvol_alloc(dev_t dev, const char *name)
{
	zvol_state_t *zv;
	uint64_t volmode;

	if (dsl_prop_get_integer(name, "volmode", &volmode, NULL) != 0)
		return (NULL);

	if (volmode == ZFS_VOLMODE_DEFAULT)
		volmode = zvol_volmode;

	if (volmode == ZFS_VOLMODE_NONE)
		return (NULL);

	zv = kmem_zalloc(sizeof (zvol_state_t), KM_SLEEP);

	list_link_init(&zv->zv_next);

	mutex_init(&zv->zv_state_lock, NULL, MUTEX_DEFAULT, NULL);

	zv->zv_queue = blk_alloc_queue(GFP_ATOMIC);
	if (zv->zv_queue == NULL)
		goto out_kmem;

	blk_queue_make_request(zv->zv_queue, zvol_request);
	blk_queue_set_write_cache(zv->zv_queue, B_TRUE, B_TRUE);

	/* Limit read-ahead to a single page to prevent over-prefetching. */
	blk_queue_set_read_ahead(zv->zv_queue, 1);

	/* Disable write merging in favor of the ZIO pipeline. */
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, zv->zv_queue);

	zv->zv_disk = alloc_disk(ZVOL_MINORS);
	if (zv->zv_disk == NULL)
		goto out_queue;

	zv->zv_queue->queuedata = zv;
	zv->zv_dev = dev;
	zv->zv_open_count = 0;
	strlcpy(zv->zv_name, name, MAXNAMELEN);

	zfs_rangelock_init(&zv->zv_rangelock, NULL, NULL);
	rw_init(&zv->zv_suspend_lock, NULL, RW_DEFAULT, NULL);

	zv->zv_disk->major = zvol_major;
#ifdef HAVE_BLOCK_DEVICE_OPERATIONS_CHECK_EVENTS
	zv->zv_disk->events = DISK_EVENT_MEDIA_CHANGE;
#endif

	if (volmode == ZFS_VOLMODE_DEV) {
		/*
		 * ZFS_VOLMODE_DEV disable partitioning on ZVOL devices: set
		 * gendisk->minors = 1 as noted in include/linux/genhd.h.
		 * Also disable extended partition numbers (GENHD_FL_EXT_DEVT)
		 * and suppresses partition scanning (GENHD_FL_NO_PART_SCAN)
		 * setting gendisk->flags accordingly.
		 */
		zv->zv_disk->minors = 1;
#if defined(GENHD_FL_EXT_DEVT)
		zv->zv_disk->flags &= ~GENHD_FL_EXT_DEVT;
#endif
#if defined(GENHD_FL_NO_PART_SCAN)
		zv->zv_disk->flags |= GENHD_FL_NO_PART_SCAN;
#endif
	}
	zv->zv_disk->first_minor = (dev & MINORMASK);
	zv->zv_disk->fops = &zvol_ops;
	zv->zv_disk->private_data = zv;
	zv->zv_disk->queue = zv->zv_queue;
	snprintf(zv->zv_disk->disk_name, DISK_NAME_LEN, "%s%d",
	    ZVOL_DEV_NAME, (dev & MINORMASK));

	return (zv);

out_queue:
	blk_cleanup_queue(zv->zv_queue);
out_kmem:
	kmem_free(zv, sizeof (zvol_state_t));

	return (NULL);
}

/*
 * Setup zv after we just own the zv->objset
 */
int
zvol_setup_zv(zvol_state_t *zv)
{
	uint64_t volsize;
	int error;
	uint64_t ro;
	objset_t *os = zv->zv_objset;

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(RW_LOCK_HELD(&zv->zv_suspend_lock));

	zv->zv_zilog = NULL;
	zv->zv_flags &= ~ZVOL_WRITTEN_TO;

	error = dsl_prop_get_integer(zv->zv_name, "readonly", &ro, NULL);
	if (error)
		return (SET_ERROR(error));

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize);
	if (error)
		return (SET_ERROR(error));

	error = dnode_hold(os, ZVOL_OBJ, FTAG, &zv->zv_dn);
	if (error)
		return (SET_ERROR(error));

	set_capacity(zv->zv_disk, volsize >> 9);
	zv->zv_volsize = volsize;

	if (ro || dmu_objset_is_snapshot(os) ||
	    !spa_writeable(dmu_objset_spa(os))) {
		set_disk_ro(zv->zv_disk, 1);
		zv->zv_flags |= ZVOL_RDONLY;
	} else {
		set_disk_ro(zv->zv_disk, 0);
		zv->zv_flags &= ~ZVOL_RDONLY;
	}
	return (0);
}

/*
 * Cleanup then free a zvol_state_t which was created by zvol_alloc().
 * At this time, the structure is not opened by anyone, is taken off
 * the zvol_state_list, and has its private data set to NULL.
 * The zvol_state_lock is dropped.
 */

void
zvol_free(void *arg)
{
	zvol_state_t *zv = arg;

	ASSERT(!RW_LOCK_HELD(&zv->zv_suspend_lock));
	ASSERT(!MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(zv->zv_open_count == 0);
	ASSERT(zv->zv_disk->private_data == NULL);

	rw_destroy(&zv->zv_suspend_lock);
	zfs_rangelock_fini(&zv->zv_rangelock);

	del_gendisk(zv->zv_disk);
	blk_cleanup_queue(zv->zv_queue);
	put_disk(zv->zv_disk);

	ida_simple_remove(&zvol_ida, MINOR(zv->zv_dev) >> ZVOL_MINOR_BITS);

	mutex_destroy(&zv->zv_state_lock);
	dataset_kstats_destroy(&zv->zv_kstat);

	kmem_free(zv, sizeof (zvol_state_t));
}

/*
 * Create a block device minor node and setup the linkage between it
 * and the specified volume.  Once this function returns the block
 * device is live and ready for use.
 */

int
zvol_create_minor_impl(const char *name)
{
	zvol_state_t *zv;
	objset_t *os;
	dmu_object_info_t *doi;
	uint64_t volsize;
	uint64_t len;
	unsigned minor = 0;
	int error = 0;
	int idx;
	uint64_t hash = zvol_name_hash(name);

	if (zvol_inhibit_dev)
		return (0);

	idx = ida_simple_get(&zvol_ida, 0, 0, kmem_flags_convert(KM_SLEEP));
	if (idx < 0)
		return (SET_ERROR(-idx));
	minor = idx << ZVOL_MINOR_BITS;

	zv = zvol_find_by_name_hash(name, hash, RW_NONE);
	if (zv) {
		ASSERT(MUTEX_HELD(&zv->zv_state_lock));
		mutex_exit(&zv->zv_state_lock);
		ida_simple_remove(&zvol_ida, idx);
		return (SET_ERROR(EEXIST));
	}

	doi = kmem_alloc(sizeof (dmu_object_info_t), KM_SLEEP);

	error = dmu_objset_own(name, DMU_OST_ZVOL, B_TRUE, B_TRUE, FTAG, &os);
	if (error)
		goto out_doi;

	error = dmu_object_info(os, ZVOL_OBJ, doi);
	if (error)
		goto out_dmu_objset_disown;

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize);
	if (error)
		goto out_dmu_objset_disown;

	zv = zvol_alloc(MKDEV(zvol_major, minor), name);
	if (zv == NULL) {
		error = SET_ERROR(EAGAIN);
		goto out_dmu_objset_disown;
	}
	zv->zv_hash = hash;

	if (dmu_objset_is_snapshot(os))
		zv->zv_flags |= ZVOL_RDONLY;

	zv->zv_volblocksize = doi->doi_data_block_size;
	zv->zv_volsize = volsize;
	zv->zv_objset = os;

	set_capacity(zv->zv_disk, zv->zv_volsize >> 9);

	blk_queue_max_hw_sectors(zv->zv_queue, (DMU_MAX_ACCESS / 4) >> 9);
	blk_queue_max_segments(zv->zv_queue, UINT16_MAX);
	blk_queue_max_segment_size(zv->zv_queue, UINT_MAX);
	blk_queue_physical_block_size(zv->zv_queue, zv->zv_volblocksize);
	blk_queue_io_opt(zv->zv_queue, zv->zv_volblocksize);
	blk_queue_max_discard_sectors(zv->zv_queue,
	    (zvol_max_discard_blocks * zv->zv_volblocksize) >> 9);
	blk_queue_discard_granularity(zv->zv_queue, zv->zv_volblocksize);
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, zv->zv_queue);
#ifdef QUEUE_FLAG_NONROT
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zv->zv_queue);
#endif
#ifdef QUEUE_FLAG_ADD_RANDOM
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, zv->zv_queue);
#endif
	/* This flag was introduced in kernel version 4.12. */
#ifdef QUEUE_FLAG_SCSI_PASSTHROUGH
	blk_queue_flag_set(QUEUE_FLAG_SCSI_PASSTHROUGH, zv->zv_queue);
#endif

	if (spa_writeable(dmu_objset_spa(os))) {
		if (zil_replay_disable)
			zil_destroy(dmu_objset_zil(os), B_FALSE);
		else
			zil_replay(os, zv, zvol_replay_vector);
	}
	ASSERT3P(zv->zv_kstat.dk_kstats, ==, NULL);
	dataset_kstats_create(&zv->zv_kstat, zv->zv_objset);

	/*
	 * When udev detects the addition of the device it will immediately
	 * invoke blkid(8) to determine the type of content on the device.
	 * Prefetching the blocks commonly scanned by blkid(8) will speed
	 * up this process.
	 */
	len = MIN(MAX(zvol_prefetch_bytes, 0), SPA_MAXBLOCKSIZE);
	if (len > 0) {
		dmu_prefetch(os, ZVOL_OBJ, 0, 0, len, ZIO_PRIORITY_SYNC_READ);
		dmu_prefetch(os, ZVOL_OBJ, 0, volsize - len, len,
		    ZIO_PRIORITY_SYNC_READ);
	}

	zv->zv_objset = NULL;
out_dmu_objset_disown:
	dmu_objset_disown(os, B_TRUE, FTAG);
out_doi:
	kmem_free(doi, sizeof (dmu_object_info_t));

	if (error == 0) {
		rw_enter(&zvol_state_lock, RW_WRITER);
		zvol_insert(zv);
		rw_exit(&zvol_state_lock);
		add_disk(zv->zv_disk);
	} else {
		ida_simple_remove(&zvol_ida, idx);
	}

	return (SET_ERROR(error));
}

/*
 * Rename a block device minor mode for the specified volume.
 */
void
zvol_rename_minor(zvol_state_t *zv, const char *newname)
{
	int readonly = get_disk_ro(zv->zv_disk);

	ASSERT(RW_LOCK_HELD(&zvol_state_lock));
	ASSERT(MUTEX_HELD(&zv->zv_state_lock));

	strlcpy(zv->zv_name, newname, sizeof (zv->zv_name));

	/* move to new hashtable entry  */
	zv->zv_hash = zvol_name_hash(zv->zv_name);
	hlist_del(&zv->zv_hlink);
	hlist_add_head(&zv->zv_hlink, ZVOL_HT_HEAD(zv->zv_hash));

	/*
	 * The block device's read-only state is briefly changed causing
	 * a KOBJ_CHANGE uevent to be issued.  This ensures udev detects
	 * the name change and fixes the symlinks.  This does not change
	 * ZVOL_RDONLY in zv->zv_flags so the actual read-only state never
	 * changes.  This would normally be done using kobject_uevent() but
	 * that is a GPL-only symbol which is why we need this workaround.
	 */
	set_disk_ro(zv->zv_disk, !readonly);
	set_disk_ro(zv->zv_disk, readonly);
}

int
zvol_os_init(void)
{
	int error;

	error = register_blkdev(zvol_major, ZVOL_DRIVER);
	if (error) {
		printk(KERN_INFO "ZFS: register_blkdev() failed %d\n", error);
		return (error);
	}

	blk_register_region(MKDEV(zvol_major, 0), 1UL << MINORBITS,
	    THIS_MODULE, zvol_probe, NULL, NULL);

	ida_init(&zvol_ida);

	return (0);
}

void
zvol_os_fini(void)
{

	blk_unregister_region(MKDEV(zvol_major, 0), 1UL << MINORBITS);
	unregister_blkdev(zvol_major, ZVOL_DRIVER);

	ida_destroy(&zvol_ida);
}

/* BEGIN CSTYLED */
module_param(zvol_inhibit_dev, uint, 0644);
MODULE_PARM_DESC(zvol_inhibit_dev, "Do not create zvol device nodes");

module_param(zvol_major, uint, 0444);
MODULE_PARM_DESC(zvol_major, "Major number for zvol device");

module_param(zvol_threads, uint, 0444);
MODULE_PARM_DESC(zvol_threads, "Max number of threads to handle I/O requests");

module_param(zvol_request_sync, uint, 0644);
MODULE_PARM_DESC(zvol_request_sync, "Synchronously handle bio requests");

module_param(zvol_max_discard_blocks, ulong, 0444);
MODULE_PARM_DESC(zvol_max_discard_blocks, "Max number of blocks to discard");

module_param(zvol_prefetch_bytes, uint, 0644);
MODULE_PARM_DESC(zvol_prefetch_bytes, "Prefetch N bytes at zvol start+end");

module_param(zvol_volmode, uint, 0644);
MODULE_PARM_DESC(zvol_volmode, "Default volmode property value");
/* END CSTYLED */
