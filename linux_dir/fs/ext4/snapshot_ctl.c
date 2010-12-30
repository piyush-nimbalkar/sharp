/*
 * linux/fs/ext4/snapshot_ctl.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots control functions.
 */

#include "snapshot.h"

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/*
 * General snapshot locking semantics:
 *
 * The snapshot_mutex:
 * -------------------
 * The majority of the code in the snapshot_{ctl,debug}.c files is called from
 * very few entry points in the code:
 * 1. {init,exit}_ext4_fs() - calls {init,exit}_ext4_snapshot() under BGL.
 * 2. ext4_{fill,put}_super() - calls ext4_snapshot_{load,destroy}() under
 *    VFS sb_lock, while f/s is not accessible to users.
 * 3. ext4_ioctl() - only place that takes snapshot_mutex (after i_mutex)
 *    and only entry point to snapshot control functions below.
 *
 * From the rules above it follows that all fields accessed inside
 * snapshot_{ctl,debug}.c are protected by one of the following:
 * - snapshot_mutex during snapshot control operations.
 * - VFS sb_lock during f/s mount/umount time.
 * - Big kernel lock during module init time.
 * Needless to say, either of the above is sufficient.
 * So if a field is accessed only inside snapshot_*.c it should be safe.
 *
 * The transaction handle:
 * -----------------------
 * Snapshot COW code (in snapshot.c) is called from block access hooks during a
 * transaction (with a transaction handle). This guaranties safe read access to
 * s_active_snapshot, without taking snapshot_mutex, because the latter is only
 * changed under journal_lock_updates() (while no transaction handles exist).
 *
 * The transaction handle is a per task struct, so there is no need to protect
 * fields on that struct (i.e. h_cowing, h_cow_*).
 */

/*
 * ext4_snapshot_set_active - set the current active snapshot
 * First, if current active snapshot exists, it is deactivated.
 * Then, if @inode is not NULL, the active snapshot is set to @inode.
 *
 * Called from ext4_snapshot_take() and ext4_snapshot_update() under
 * journal_lock_updates() and snapshot_mutex.
 * Called from ext4_snapshot_{load,destroy}() under sb_lock.
 *
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_set_active(struct super_block *sb,
		struct inode *inode)
{
	struct inode *old = EXT4_SB(sb)->s_active_snapshot;

	if (old == inode)
		return 0;

	/* add new active snapshot reference */
	if (inode && !igrab(inode))
		return -EIO;

	/* point of no return - replace old with new snapshot */
	if (old) {
		EXT4_I(old)->i_flags &= ~EXT4_SNAPFILE_ACTIVE_FL;
		snapshot_debug(1, "snapshot (%u) deactivated\n",
			       old->i_generation);
		/* remove old active snapshot reference */
		iput(old);
	}
	if (inode) {
		EXT4_I(inode)->i_flags |=
			EXT4_SNAPFILE_ACTIVE_FL|EXT4_SNAPFILE_LIST_FL;
		snapshot_debug(1, "snapshot (%u) activated\n",
			       inode->i_generation);
	}
	EXT4_SB(sb)->s_active_snapshot = inode;

	return 0;
}
#endif

#define ext4_snapshot_reset_bitmap_cache(sb, init) 0


/*
 * Snapshot constructor/destructor
 */

/* with no exclude inode, exclude bitmap is reset to 0 */
#define ext4_snapshot_init_bitmap_cache(sb, create)	\
		ext4_snapshot_reset_bitmap_cache(sb, 1)

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/*
 * ext4_snapshot_load - load the on-disk snapshot list to memory.
 * Start with last (or active) snapshot and continue to older snapshots.
 * If snapshot load fails before active snapshot, force read-only mount.
 * If snapshot load fails after active snapshot, allow read-write mount.
 * Called from ext4_fill_super() under sb_lock during mount time.
 *
 * Return values:
 * = 0 - on-disk snapshot list is empty or active snapshot loaded
 * < 0 - error loading active snapshot
 */
int ext4_snapshot_load(struct super_block *sb, struct ext4_super_block *es,
		int read_only)
{
	__u32 active_ino = le32_to_cpu(es->s_snapshot_inum);
	__u32 load_ino = le32_to_cpu(es->s_snapshot_list);
	int err, num = 0, snapshot_id = 0;
	int has_active = 0;


#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE_OLD
	/* Migrate super block on-disk format */
	if (EXT4_HAS_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT_OLD) &&
			!EXT4_HAS_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
		u64 snapshot_r_blocks;

		/* Copy snapshot fields to new positions */
		es->s_snapshot_inum = es->s_snapshot_inum_old;
		active_ino = le32_to_cpu(es->s_snapshot_inum);
		es->s_snapshot_id = es->s_snapshot_id_old;
		snapshot_r_blocks = le32_to_cpu(es->s_snapshot_r_blocks_old);
		es->s_snapshot_r_blocks_count = cpu_to_le64(snapshot_r_blocks);
		es->s_snapshot_list = es->s_snapshot_list_old;
		/* Clear old snapshot fields */
		es->s_snapshot_inum_old = 0;
		es->s_snapshot_id_old = 0;
		es->s_snapshot_r_blocks_old = 0;
		es->s_snapshot_list_old = 0;
		/* Copy snapshot flags to new positions */
		EXT4_SET_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
		if (EXT4_HAS_COMPAT_FEATURE(sb,
				EXT4_FEATURE_COMPAT_EXCLUDE_INODE_OLD))
			EXT4_SET_COMPAT_FEATURE(sb,
					EXT4_FEATURE_COMPAT_EXCLUDE_INODE);
		if (EXT4_HAS_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_FIX_SNAPSHOT_OLD))
			EXT4_SET_FLAGS(sb, EXT4_FLAGS_FIX_SNAPSHOT);
		if (EXT4_HAS_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_FIX_EXCLUDE_OLD))
			EXT4_SET_FLAGS(sb, EXT4_FLAGS_FIX_EXCLUDE);
		/* Clear old snapshot flags */
		EXT4_CLEAR_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT_OLD|
				EXT4_FEATURE_RO_COMPAT_IS_SNAPSHOT_OLD|
				EXT4_FEATURE_RO_COMPAT_FIX_SNAPSHOT_OLD|
				EXT4_FEATURE_RO_COMPAT_FIX_EXCLUDE_OLD);
		/* Clear deprecated big journal flag */
		EXT4_CLEAR_COMPAT_FEATURE(sb,
				EXT4_FEATURE_COMPAT_BIG_JOURNAL_OLD);
		EXT4_CLEAR_FLAGS(sb, EXT4_FLAGS_BIG_JOURNAL);
		/* Keep old exclude inode flag b/c inode was not moved yet */
	}

#endif
	/* init COW bitmap and exclude bitmap cache */
	err = ext4_snapshot_init_bitmap_cache(sb, !read_only);
	if (err)
		return err;

	if (!load_ino && active_ino) {
		/* snapshots list is empty and active snapshot exists */
		if (!read_only)
			/* reset list head to active snapshot */
			es->s_snapshot_list = es->s_snapshot_inum;
		/* try to load active snapshot */
		load_ino = le32_to_cpu(es->s_snapshot_inum);
	}

	if (!EXT4_HAS_RO_COMPAT_FEATURE(sb,
				EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT)) {
		/*
		 * When mounting an ext3 formatted volume as ext4, the
		 * HAS_SNAPSHOT flag is set on first snapshot_take()
		 * and after that the volume can no longer be mounted
		 * as rw ext3 (only rw ext4 or ro ext3/ext2).
		 * If we find a non-zero last_snapshot or snapshot_inum
		 * and the HAS_SNAPSHOT flag is not set, we ignore them.
		 */
		if (load_ino)
			snapshot_debug(1, "warning: has_snapshot feature not "
					"set and last snapshot found (%u).\n",
					load_ino);
		return 0;
	}

	while (load_ino) {
		struct inode *inode;

		inode = ext4_orphan_get(sb, load_ino);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
		} else if (!ext4_snapshot_file(inode)) {
			iput(inode);
			err = -EIO;
		}

		if (err && num == 0 && load_ino != active_ino) {
			/* failed to load last non-active snapshot */
			if (!read_only)
				/* reset list head to active snapshot */
				es->s_snapshot_list = es->s_snapshot_inum;
			snapshot_debug(1, "warning: failed to load "
					"last snapshot (%u) - trying to load "
					"active snapshot (%u).\n",
					load_ino, active_ino);
			/* try to load active snapshot */
			load_ino = active_ino;
			err = 0;
			continue;
		}

		if (err)
			break;

		snapshot_id = inode->i_generation;
		snapshot_debug(1, "snapshot (%d) loaded\n",
			       snapshot_id);
		num++;

		if (!has_active && load_ino == active_ino) {
			/* active snapshot was loaded */
			err = ext4_snapshot_set_active(sb, inode);
			if (err)
				break;
			has_active = 1;
		}

		iput(inode);
		break;
	}

	if (err) {
		/* failed to load active snapshot */
		snapshot_debug(1, "warning: failed to load "
				"snapshot (ino=%u) - "
				"forcing read-only mount!\n",
				load_ino);
		/* force read-only mount */
		return read_only ? 0 : err;
	}

	if (num > 0) {
		err = ext4_snapshot_update(sb, 0, read_only);
		snapshot_debug(1, "%d snapshots loaded\n", num);
	}
	return err;
}

/*
 * ext4_snapshot_destroy() releases the in-memory snapshot list
 * Called from ext4_put_super() under sb_lock during umount time.
 * This function cannot fail.
 */
void ext4_snapshot_destroy(struct super_block *sb)
{
	/* deactivate in-memory active snapshot - cannot fail */
	(void) ext4_snapshot_set_active(sb, NULL);
}

/*
 * ext4_snapshot_update - iterate snapshot list and update snapshots status.
 * @sb: handle to file system super block.
 * @cleanup: if true, shrink/merge/cleanup all snapshots marked for deletion.
 * @read_only: if true, don't remove snapshot after failed take.
 *
 * Called from ext4_ioctl() under snapshot_mutex.
 * Called from snapshot_load() under sb_lock with @cleanup=0.
 * Returns 0 on success and <0 on error.
 */
int ext4_snapshot_update(struct super_block *sb, int cleanup, int read_only)
{
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	int err = 0;

	BUG_ON(read_only && cleanup);
	if (active_snapshot)
		EXT4_I(active_snapshot)->i_flags |=
			EXT4_SNAPFILE_ACTIVE_FL|EXT4_SNAPFILE_LIST_FL;


	return err;
}
#else
int ext4_snapshot_load(struct super_block *sb, struct ext4_super_block *es,
		int read_only)
{
	return 0;
}

void ext4_snapshot_destroy(struct super_block *sb)
{
}
#endif
