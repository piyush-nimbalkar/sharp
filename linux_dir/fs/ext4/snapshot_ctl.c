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

#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK_BITMAP
/*
 * ext4_snapshot_reset_bitmap_cache():
 *
 * Resets the COW/exclude bitmap cache for all block groups.
 *
 * Called from init_bitmap_cache() with @init=1 under sb_lock during mount time.
 * Called from snapshot_take() with @init=0 under journal_lock_updates().
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_reset_bitmap_cache(struct super_block *sb, int init)
{
	struct ext4_group_info *gi = EXT4_SB(sb)->s_snapshot_group_info;
	int i;

	for (i = 0; i < EXT4_SB(sb)->s_groups_count; i++, gi++) {
		gi->bg_cow_bitmap = 0;
		if (init)
			gi->bg_exclude_bitmap = 0;
	}
	return 0;
}
#else
#define ext4_snapshot_reset_bitmap_cache(sb, init) 0
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
/*
 * Snapshot control functions
 *
 * Snapshot files are controlled by changing snapshot flags with chattr and
 * moving the snapshot file through the stages of its life cycle:
 *
 * 1. Creating a snapshot file
 * The snapfile flag is changed for directories only (chattr +x), so
 * snapshot files must be created inside a snapshots directory.
 * They inherit the flag at birth and they die with it.
 * This helps to avoid various race conditions when changing
 * regular files to snapshots and back.
 * Snapshot files are assigned with read-only address space operations, so
 * they are not writable for users.
 *
 * 2. Taking a snapshot
 * An empty snapshot file becomes the active snapshot after it is added to the
 * head on the snapshots list by setting its snapshot list flag (chattr -X +S).
 * snapshot_create() verifies that the file is empty and pre-allocates some
 * blocks during the ioctl transaction.  snapshot_take() locks journal updates
 * and copies some file system block to the pre-allocated blocks and then adds
 * the snapshot file to the on-disk list and sets it as the active snapshot.
 *
 * 3. Mounting a snapshot
 * A snapshot on the list can be enabled for user read access by setting the
 * enabled flag (chattr -X +n) and disabled by clearing the enabled flag.
 * An enabled snapshot can be mounted via a loop device and mounted as a
 * read-only ext2 filesystem.
 *
 * 4. Deleting a snapshot
 * A non-mounted and disabled snapshot may be marked for removal from the
 * snapshots list by requesting to clear its snapshot list flag (chattr -X -S).
 * The process of removing a snapshot from the list varies according to the
 * dependencies between the snapshot and older snapshots on the list:
 * - if all older snapshots are deleted, the snapshot is removed from the list.
 * - if some older snapshots are enabled, snapshot_shrink() is called to free
 *   unused blocks, but the snapshot remains on the list.
 * - if all older snapshots are disabled, snapshot_merge() is called to move
 *   used blocks to an older snapshot and the snapshot is removed from the list.
 *
 * 5. Unlinking a snapshot file
 * When a snapshot file is no longer (or never was) on the snapshots list, it
 * may be unlinked.  Snapshots on the list are protected from user unlink and
 * truncate operations.
 *
 * 6. Discarding all snapshots
 * An irregular way to abruptly end the lives of all snapshots on the list is by
 * detaching the snapshot list head using the command: tune2fs -O ^has_snapshot.
 * This action is applicable on an un-mounted ext4 filesystem.  After mounting
 * the filesystem, the discarded snapshot files will not be loaded, they will
 * not have the snapshot list flag and therefore, may be unlinked.
 */
static int ext4_snapshot_enable(struct inode *inode);
static int ext4_snapshot_disable(struct inode *inode);
static int ext4_snapshot_create(struct inode *inode);
static int ext4_snapshot_delete(struct inode *inode);

/*
 * ext4_snapshot_get_flags() check snapshot state
 * Called from ext4_ioctl() under i_mutex
 */
void ext4_snapshot_get_flags(struct ext4_inode_info *ei, struct file *filp)
{
	int open_count = atomic_read(&filp->f_path.dentry->d_count);
	/*
	 * 1 count for ioctl (lsattr)
	 * greater count means the snapshot is open by user (mounted?)
	 */
	if ((ei->i_flags & EXT4_SNAPFILE_LIST_FL) && open_count > 1)
		ei->i_flags |= EXT4_SNAPFILE_OPEN_FL;
	else
		ei->i_flags &= ~EXT4_SNAPFILE_OPEN_FL;
}

/*
 * ext4_snapshot_set_flags() monitors snapshot state changes
 * Called from ext4_ioctl() under i_mutex and snapshot_mutex
 */
int ext4_snapshot_set_flags(handle_t *handle, struct inode *inode,
			     unsigned int flags)
{
	unsigned int oldflags = EXT4_I(inode)->i_flags;
	int err = 0;

	if (S_ISDIR(inode->i_mode)) {
		/* only the snapfile flag may be set for directories */
		EXT4_I(inode)->i_flags &= ~EXT4_SNAPFILE_FL;
		EXT4_I(inode)->i_flags |= flags & EXT4_SNAPFILE_FL;
		goto non_snapshot;
	}

	if (!ext4_snapshot_file(inode)) {
		if ((flags ^ oldflags) & EXT4_FL_SNAPSHOT_MASK) {
			/* snapflags can only be changed for snapfiles */
			snapshot_debug(1, "changing snapflags for non snapfile"
					" (ino=%lu) is not allowed\n",
					inode->i_ino);
			return -EINVAL;
		}
		goto non_snapshot;
	}



	if ((flags ^ oldflags) & EXT4_SNAPFILE_ENABLED_FL) {
		/* enabled/disabled the snapshot during transaction */
		if (flags & EXT4_SNAPFILE_ENABLED_FL)
			err = ext4_snapshot_enable(inode);
		else
			err = ext4_snapshot_disable(inode);
	}
	if (err)
		goto out;

	if ((flags ^ oldflags) & EXT4_SNAPFILE_LIST_FL) {
		/* add/delete to snapshots list during transaction */
		if (flags & EXT4_SNAPFILE_LIST_FL)
			err = ext4_snapshot_create(inode);
		else
			err = ext4_snapshot_delete(inode);
	}
	if (err)
		goto out;

	/* set snapshot user flags */
	EXT4_I(inode)->i_flags &= ~EXT4_FL_SNAPSHOT_USER_MASK;
	EXT4_I(inode)->i_flags |= flags & EXT4_FL_SNAPSHOT_USER_MASK;
non_snapshot:
	/* set only non-snapshot flags here */
	flags &= ~EXT4_FL_SNAPSHOT_MASK;
	flags |= (EXT4_I(inode)->i_flags & EXT4_FL_SNAPSHOT_MASK);
	EXT4_I(inode)->i_flags = flags;

out:
	/*
	 * retake reserve inode write from ext4_ioctl() and mark inode
	 * dirty
	 */
	if (!err)
		err = ext4_mark_inode_dirty(handle, inode);
	return err;
}

/*
 * If we have fewer than nblocks credits,
 * extend transaction by a minimum of EXT4_MAX_TRANS_DATA.
 * If that fails, restart the transaction &
 * regain write access for the inode block.
 */
static int __extend_or_restart_transaction(const char *where,
		handle_t *handle, struct inode *inode, int nblocks)
{
	int err;

	if (EXT4_SNAPSHOT_HAS_TRANS_BLOCKS(handle, nblocks))
		return 0;

	if (nblocks < EXT4_MAX_TRANS_DATA)
		nblocks = EXT4_MAX_TRANS_DATA;

	err = __ext4_journal_extend(where,
			(ext4_handle_t *)handle, nblocks);
	if (err < 0)
		return err;
	if (err) {
		if (inode) {
			/* lazy way to do mark_iloc_dirty() */
			err = ext4_mark_inode_dirty(handle, inode);
			if (err)
				return err;
		}
		err = __ext4_journal_restart(where,
				(ext4_handle_t *)handle, nblocks);
		if (err)
			return err;
		if (inode)
			/* lazy way to do reserve_inode_write() */
			err = ext4_mark_inode_dirty(handle, inode);
	}

	return err;
}

#define extend_or_restart_transaction(handle, nblocks)			\
	__extend_or_restart_transaction(__func__, (handle), NULL, (nblocks))
#define extend_or_restart_transaction_inode(handle, inode, nblocks)	\
	__extend_or_restart_transaction(__func__, (handle), (inode), (nblocks))

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
/*
 * helper function for snapshot_create().
 * places pre-allocated [d,t]ind blocks in position
 * after they have been allocated as direct blocks.
 */
static inline int ext4_snapshot_shift_blocks(struct ext4_inode_info *ei,
		int from, int to, int count)
{
	int i, err = -EIO;

	/* move from direct blocks range */
	BUG_ON(from < 0 || from + count > EXT4_NDIR_BLOCKS);
	/* to indirect blocks range */
	BUG_ON(to < EXT4_NDIR_BLOCKS || to + count > EXT4_SNAPSHOT_N_BLOCKS);

	/*
	 * truncate_mutex is held whenever allocating or freeing inode
	 * blocks.
	 */
	down_write(&ei->i_data_sem);

	/*
	 * verify that 'from' blocks are allocated
	 * and that 'to' blocks are not allocated.
	 */
	for (i = 0; i < count; i++)
		if (!ei->i_data[from+i] ||
				ei->i_data[to+i])
			goto out;

	/*
	 * shift 'count' blocks from position 'from' to 'to'
	 */
	for (i = 0; i < count; i++) {
		ei->i_data[to+i] = ei->i_data[from+i];
		ei->i_data[from+i] = 0;
	}
	err = 0;
out:
	up_write(&ei->i_data_sem);
	return err;
}
#endif

/*
 * ext4_snapshot_create() initializes a snapshot file
 * and adds it to the list of snapshots
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_create(struct inode *inode)
{
	handle_t *handle;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	int i, err, ret;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
	int count, nind;
	const long double_blocks = (1 << (2 * SNAPSHOT_ADDR_PER_BLOCK_BITS));
	struct buffer_head *bh = NULL;
	struct ext4_group_desc *desc;
	unsigned long ino;
	struct ext4_iloc iloc;
	ext4_fsblk_t bmap_blk = 0, imap_blk = 0, inode_blk = 0;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	ext4_fsblk_t prev_inode_blk = 0;
#endif
#endif
	ext4_fsblk_t snapshot_blocks = ext4_blocks_count(sbi->s_es);
	if (active_snapshot) {
		snapshot_debug(1, "failed to add snapshot because active "
			       "snapshot (%u) has to be deleted first\n",
			       active_snapshot->i_generation);
		return -EINVAL;
	}

	/* prevent take of unlinked snapshot file */
	if (!inode->i_nlink) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it has 0 nlink count\n",
				inode->i_ino);
		return -EINVAL;
	}

	/* prevent recycling of old snapshot files */
	if ((ei->i_flags & EXT4_FL_SNAPSHOT_MASK) != EXT4_SNAPFILE_FL) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it has snapshot flags (0x%x)\n",
				inode->i_ino,
				inode->i_flags & EXT4_FL_SNAPSHOT_MASK);
		return -EINVAL;
	}

	/* verify that no inode blocks are allocated */
	for (i = 0; i < EXT4_N_BLOCKS; i++) {
		if (ei->i_data[i])
			break;
	}
	/* Don't need i_size_read because we hold i_mutex */
	if (i != EXT4_N_BLOCKS ||
		inode->i_size > 0 || ei->i_disksize > 0) {
		snapshot_debug(1, "failed to create snapshot file (ino=%lu) "
				"because it is not empty (i_data[%d]=%u, "
				"i_size=%lld, i_disksize=%lld)\n",
				inode->i_ino, i, ei->i_data[i],
				inode->i_size, ei->i_disksize);
		return -EINVAL;
	}

	/*
	 * Take a reference to the small transaction that started in
	 * ext4_ioctl() We will extend or restart this transaction as we go
	 * along.  journal_start(n > 1) would not have increase the buffer
	 * credits.
	 */
	handle = ext4_journal_start(inode, 1);

	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	/* record the new snapshot ID in the snapshot inode generation field */
	inode->i_generation = le32_to_cpu(sbi->s_es->s_snapshot_id) + 1;
	if (inode->i_generation == 0)
		/* 0 is not a valid snapshot id */
		inode->i_generation = 1;

	/* record the file system size in the snapshot inode disksize field */
	SNAPSHOT_SET_BLOCKS(inode, snapshot_blocks);
	SNAPSHOT_SET_DISABLED(inode);

	if (!EXT4_HAS_RO_COMPAT_FEATURE(sb,
		EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT))
		/* set the 'has_snapshot' feature */
		EXT4_SET_RO_COMPAT_FEATURE(sb,
			EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT);

	lock_super(sb);
	err = ext4_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_snapshot_list = cpu_to_le32(inode->i_ino);
	if (!err)
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	unlock_super(sb);
	if (err)
		goto out_handle;

	err = ext4_mark_inode_dirty(handle, inode);
	if (err)
		goto out_handle;

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
	/* small filesystems can be mapped with just 1 double indirect block */
	nind = 1;
	if (snapshot_blocks > double_blocks)
		/* add up to 4 triple indirect blocks to map 2^32 blocks */
		nind += ((snapshot_blocks - double_blocks) >>
			(3 * SNAPSHOT_ADDR_PER_BLOCK_BITS)) + 1;
	if (nind > EXT4_SNAPSHOT_NTIND_BLOCKS + 1) {
		snapshot_debug(1, "need too many [d,t]ind blocks (%d) "
				"for snapshot (%u)\n",
				nind, inode->i_generation);
		err = -EFBIG;
		goto out_handle;
	}

	err = extend_or_restart_transaction_inode(handle, inode,
			nind * EXT4_DATA_TRANS_BLOCKS(sb));
	if (err)
		goto out_handle;

	/* pre-allocate and zero out [d,t]ind blocks */
	for (i = 0; i < nind; i++) {
		brelse(bh);
		bh = ext4_getblk(handle, inode, i, SNAPMAP_WRITE, &err);
		if (!bh || err)
			break;
		/* zero out indirect block and journal as dirty metadata */
		err = ext4_journal_get_write_access(handle, bh);
		if (err)
			break;
		lock_buffer(bh);
		memset(bh->b_data, 0, bh->b_size);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		err = ext4_handle_dirty_metadata(handle, NULL, bh);
		if (err)
			break;
	}
	brelse(bh);
	if (!bh || err) {
		snapshot_debug(1, "failed to initiate [d,t]ind block (%d) "
				"for snapshot (%u)\n",
				i, inode->i_generation);
		goto out_handle;
	}
	/* place pre-allocated [d,t]ind blocks in position */
	err = ext4_snapshot_shift_blocks(ei, 0, EXT4_DIND_BLOCK, nind);
	if (err) {
		snapshot_debug(1, "failed to move pre-allocated [d,t]ind blocks"
				" for snapshot (%u)\n",
				inode->i_generation);
		goto out_handle;
	}

	/* allocate super block and group descriptors for snapshot */
	count = sbi->s_gdb_count + 1;
	err = count;
	for (i = 0; err > 0 && i < count; i += err) {
		err = extend_or_restart_transaction_inode(handle, inode,
				EXT4_DATA_TRANS_BLOCKS(sb));
		if (err)
			goto out_handle;
		err = ext4_snapshot_map_blocks(handle, inode, i, count - i,
						NULL, SNAPMAP_WRITE);
	}
	if (err <= 0) {
		snapshot_debug(1, "failed to allocate super block and %d "
			       "group descriptor blocks for snapshot (%u)\n",
			       count - 1, inode->i_generation);
		if (err)
			err = -EIO;
		goto out_handle;
	}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	/* start with root inode and continue with snapshot list */
	ino = EXT4_ROOT_INO;
alloc_inode_blocks:
#else
	ino = inode->i_ino;
#endif
	/*
	 * pre-allocate the following blocks in the new snapshot:
	 * - block and inode bitmap blocks of ino's block group
	 * - inode table block that contains ino
	 */
	err = extend_or_restart_transaction_inode(handle, inode,
			3 * EXT4_DATA_TRANS_BLOCKS(sb));
	if (err)
		goto out_handle;

	iloc.block_group = 0;
	inode_blk = ext4_get_inode_block(sb, ino, &iloc);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	if (!inode_blk || inode_blk == prev_inode_blk)
		goto next_snapshot;

	/* not same inode and bitmap blocks as prev snapshot */
	prev_inode_blk = inode_blk;
#endif
	bmap_blk = 0;
	imap_blk = 0;
	desc = ext4_get_group_desc(sb, iloc.block_group, NULL);
	if (!desc)
		goto next_snapshot;

	bmap_blk = ext4_block_bitmap(sb, desc);
	imap_blk = ext4_inode_bitmap(sb, desc);
	if (!bmap_blk || !imap_blk)
		goto next_snapshot;

	count = 1;
	if (imap_blk == bmap_blk + 1)
		count++;
	if (inode_blk == imap_blk + 1)
		count++;
	/* try to allocate all blocks at once */
	err = ext4_snapshot_map_blocks(handle, inode,
			bmap_blk, count,
			NULL, SNAPMAP_WRITE);
	count = err;
	/* allocate remaining blocks one by one */
	if (err > 0 && count < 2)
		err = ext4_snapshot_map_blocks(handle, inode,
				imap_blk, 1,
				NULL,
				SNAPMAP_WRITE);
	if (err > 0 && count < 3)
		err = ext4_snapshot_map_blocks(handle, inode,
				inode_blk, 1,
				NULL,
				SNAPMAP_WRITE);
next_snapshot:
	if (!bmap_blk || !imap_blk || !inode_blk || err < 0) {
#ifdef CONFIG_EXT4_FS_DEBUG
		ext4_fsblk_t blk0 = iloc.block_group *
			EXT4_BLOCKS_PER_GROUP(sb);
		snapshot_debug(1, "failed to allocate block/inode bitmap "
				"or inode table block of inode (%lu) "
				"(%llu,%llu,%llu/%u) for snapshot (%u)\n",
				ino, bmap_blk - blk0,
				imap_blk - blk0, inode_blk - blk0,
				iloc.block_group, inode->i_generation);
#endif
		if (!err)
			err = -EIO;
		goto out_handle;
	}
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	if (ino == EXT4_ROOT_INO) {
		ino = inode->i_ino;
		goto alloc_inode_blocks;
	}
#endif
#endif

	snapshot_debug(1, "snapshot (%u) created\n", inode->i_generation);
	err = 0;
out_handle:
	ret = ext4_journal_stop(handle);
	if (!err)
		err = ret;
	return err;
}

/*
 * If we call ext4_getblk() with NULL handle we will get read through access
 * to snapshot inode.  We don't want read through access in snapshot_take(),
 * so we call ext4_getblk() with this dummy handle and since we are not
 * allocating snapshot block here the handle will not be used anyway.
 */
static handle_t dummy_handle;

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
/*
 * ext4_snapshot_copy_block() - copy block to new snapshot
 * @snapshot:	new snapshot to copy block to
 * @bh:		source buffer to be copied
 * @mask:	if not NULL, mask buffer data before copying to snapshot
 *		(used to mask block bitmap with exclude bitmap)
 * @name:	name of copied block to print
 * @idx:	index of copied block to print
 *
 * Called from ext4_snapshot_take() under journal_lock_updates()
 * Returns snapshot buffer on success, NULL on error
 */
static struct buffer_head *ext4_snapshot_copy_block(struct inode *snapshot,
		struct buffer_head *bh, const char *mask,
		const char *name, unsigned long idx)
{
	struct buffer_head *sbh = NULL;
	int err;

	if (!bh)
		return NULL;

	sbh = ext4_getblk(&dummy_handle, snapshot,
			SNAPSHOT_IBLOCK(bh->b_blocknr),
			SNAPMAP_READ, &err);

	if (err || !sbh || sbh->b_blocknr == bh->b_blocknr) {
		snapshot_debug(1, "failed to copy %s (%lu) "
				"block [%llu/%llu] to snapshot (%u)\n",
				name, idx,
				SNAPSHOT_BLOCK_TUPLE(bh->b_blocknr),
				snapshot->i_generation);
		brelse(sbh);
		return NULL;
	}

	ext4_snapshot_copy_buffer(sbh, bh, mask);

	snapshot_debug(4, "copied %s (%lu) block [%llu/%llu] "
			"to snapshot (%u)\n",
			name, idx,
			SNAPSHOT_BLOCK_TUPLE(bh->b_blocknr),
			snapshot->i_generation);
	return sbh;
}

/*
 * List of blocks which are copied to snapshot for every special inode.
 * Keep block bitmap first and inode table block last in the list.
 */
enum copy_inode_block {
	COPY_BLOCK_BITMAP,
	COPY_INODE_BITMAP,
	COPY_INODE_TABLE,
	COPY_INODE_BLOCKS_NUM
};

static char *copy_inode_block_name[COPY_INODE_BLOCKS_NUM] = {
	"block bitmap",
	"inode bitmap",
	"inode table"
};
#endif

/*
 * ext4_snapshot_take() makes a new snapshot file
 * into the active snapshot
 *
 * this function calls journal_lock_updates()
 * and should not be called during a journal transaction
 * Called from ext4_ioctl() under i_mutex and snapshot_mutex
 */
int ext4_snapshot_take(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = NULL;
	struct buffer_head *sbh = NULL;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
	struct buffer_head *bhs[COPY_INODE_BLOCKS_NUM] = { NULL };
	const char *mask = NULL;
	struct inode *curr_inode;
	struct ext4_iloc iloc;
	struct ext4_group_desc *desc;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	ext4_fsblk_t prev_inode_blk = 0;
	struct ext4_inode *raw_inode;
#endif
	int i;
#endif
	int err = -EIO;

	if (!sbi->s_sbh)
		goto out_err;
	else if (sbi->s_sbh->b_blocknr != 0) {
		snapshot_debug(1, "warning: unexpected super block at block "
			"(%lld:%d)!\n", (long long)sbi->s_sbh->b_blocknr,
			(int)((char *)sbi->s_es - (char *)sbi->s_sbh->b_data));
	} else if (sbi->s_es->s_magic != cpu_to_le16(EXT4_SUPER_MAGIC)) {
		snapshot_debug(1, "warning: super block of snapshot (%u) is "
			       "broken!\n", inode->i_generation);
	} else
		sbh = ext4_getblk(&dummy_handle, inode, SNAPSHOT_IBLOCK(0),
				   SNAPMAP_READ, &err);

	if (!sbh || sbh->b_blocknr == 0) {
		snapshot_debug(1, "warning: super block of snapshot (%u) not "
			       "allocated\n", inode->i_generation);
		goto out_err;
	} else {
		snapshot_debug(4, "super block of snapshot (%u) mapped to "
			       "block (%lld)\n", inode->i_generation,
			       (long long)sbh->b_blocknr);
		es = (struct ext4_super_block *)(sbh->b_data +
						  ((char *)sbi->s_es -
						   sbi->s_sbh->b_data));
	}

	err = -EIO;

	/*
	 * flush journal to disk and clear the RECOVER flag
	 * before taking the snapshot
	 */
	sb->s_op->freeze_fs(sb);
	lock_super(sb);

#ifdef CONFIG_EXT4_FS_DEBUG
	if (snapshot_enable_test[SNAPTEST_TAKE]) {
		snapshot_debug(1, "taking snapshot (%u) ...\n",
				inode->i_generation);
		/* sleep 1 tunable delay unit */
		snapshot_test_delay(SNAPTEST_TAKE);
	}
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
	/*
	 * copy super block to snapshot and fix it
	 */
	lock_buffer(sbh);
	memcpy(sbh->b_data, sbi->s_sbh->b_data, sb->s_blocksize);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	/*
	 * Convert from Ext4 to Ext3 super block:
	 * Remove the HAS_SNAPSHOT flag and snapshot inode number.
	 * Set the IS_SNAPSHOT flag to signal fsck this is a snapshot image.
	 */
	es->s_feature_ro_compat &=
		~cpu_to_le32(EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
	es->s_snapshot_inum = 0;
	es->s_snapshot_list = 0;
	es->s_flags |= cpu_to_le32(EXT4_FLAGS_IS_SNAPSHOT);
#endif
	set_buffer_uptodate(sbh);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);

	/*
	 * copy group descriptors to snapshot
	 */
	for (i = 0; i < sbi->s_gdb_count; i++) {
		brelse(sbh);
		sbh = ext4_snapshot_copy_block(inode,
				sbi->s_group_desc[i], NULL,
				"GDT", i);
		if (!sbh)
			goto out_unlockfs;
	}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	/* start with root inode and continue with snapshot list */
	curr_inode = sb->s_root->d_inode;
copy_inode_blocks:
#else
	curr_inode = inode;
#endif
	/*
	 * copy the following blocks to the new snapshot:
	 * - block and inode bitmap blocks of curr_inode block group
	 * - inode table block that contains curr_inode
	 */
	iloc.block_group = 0;
	err = ext4_get_inode_loc(curr_inode, &iloc);
	desc = ext4_get_group_desc(sb, iloc.block_group, NULL);
	if (err || !desc) {
		snapshot_debug(1, "failed to read inode and bitmap blocks "
			       "of inode (%lu)\n", curr_inode->i_ino);
		err = err ? : -EIO;
		goto out_unlockfs;
	}
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
	if (iloc.bh->b_blocknr == prev_inode_blk)
		goto fix_inode_copy;
	prev_inode_blk = iloc.bh->b_blocknr;
#endif
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
	bhs[COPY_BLOCK_BITMAP] = sb_bread(sb,
		    ext4_block_bitmap(sb, desc));
	bhs[COPY_INODE_BITMAP] = sb_bread(sb,
			ext4_inode_bitmap(sb, desc));
	bhs[COPY_INODE_TABLE] = iloc.bh;
	err = -EIO;
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++) {
		brelse(sbh);
		sbh = ext4_snapshot_copy_block(inode, bhs[i], mask,
				copy_inode_block_name[i], curr_inode->i_ino);
		if (!sbh)
			goto out_unlockfs;
		mask = NULL;
	}
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_FIX
fix_inode_copy:
	/* get snapshot copy of raw inode */
	iloc.bh = sbh;
	raw_inode = ext4_raw_inode(&iloc);
	if (curr_inode->i_ino != EXT4_ROOT_INO) {
		/*
		 * Snapshot inode blocks are excluded from COW bitmap,
		 * so they appear to be not allocated in the snapshot's
		 * block bitmap.  If we want the snapshot image to pass
		 * fsck with no errors, we need to detach those blocks
		 * from the copy of the snapshot inode, so we fix the
		 * snapshot inodes to appear as empty regular files.
		 */
		raw_inode->i_size = 0;
		raw_inode->i_size_high = 0;
		raw_inode->i_blocks_lo = 0;
		raw_inode->i_blocks_high = 0;
		raw_inode->i_flags &= cpu_to_le32(~EXT4_FL_SNAPSHOT_MASK);
		memset(raw_inode->i_block, 0, sizeof(raw_inode->i_block));
	}
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);

	if (curr_inode->i_ino == EXT4_ROOT_INO) {
		curr_inode = inode;
		goto copy_inode_blocks;
	}
#endif
#endif

	/* reset COW bitmap cache */
	err = ext4_snapshot_reset_bitmap_cache(sb, 0);
	if (err)
		goto out_unlockfs;
	/* set as in-memory active snapshot */
	err = ext4_snapshot_set_active(sb, inode);
	if (err)
		goto out_unlockfs;

	/* set as on-disk active snapshot */
	sbi->s_es->s_snapshot_id =
		cpu_to_le32(le32_to_cpu(sbi->s_es->s_snapshot_id)+1);
	if (sbi->s_es->s_snapshot_id == 0)
		/* 0 is not a valid snapshot id */
		sbi->s_es->s_snapshot_id = cpu_to_le32(1);
	sbi->s_es->s_snapshot_inum = cpu_to_le32(inode->i_ino);

	err = 0;
out_unlockfs:
	unlock_super(sb);
	sb->s_op->unfreeze_fs(sb);

	if (err)
		goto out_err;

	snapshot_debug(1, "snapshot (%u) has been taken\n",
			inode->i_generation);

out_err:
	brelse(sbh);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL_INIT
	for (i = 0; i < COPY_INODE_BLOCKS_NUM; i++)
		brelse(bhs[i]);
#endif
	return err;
}


/*
 * ext4_snapshot_enable() enables snapshot mount
 * sets the in-use flag and the active snapshot
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_enable(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_enable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & EXT4_SNAPFILE_DELETED_FL) {
		snapshot_debug(1, "enable of deleted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to block device size to enable loop device mount
	 */
	SNAPSHOT_SET_ENABLED(inode);
	ei->i_flags |= EXT4_SNAPFILE_ENABLED_FL;

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) enabled\n", inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_disable() disables snapshot mount
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_disable(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_disable() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & EXT4_SNAPFILE_OPEN_FL) {
		snapshot_debug(1, "disable of mounted snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/*
	 * set i_size to zero to disable loop device mount
	 */
	SNAPSHOT_SET_DISABLED(inode);
	ei->i_flags &= ~EXT4_SNAPFILE_ENABLED_FL;

	/* invalidate page cache */
	truncate_inode_pages(&inode->i_data, SNAPSHOT_BYTES_OFFSET);

	/* Don't need i_size_read because we hold i_mutex */
	snapshot_debug(4, "setting snapshot (%u) i_size to (%lld)\n",
			inode->i_generation, inode->i_size);
	snapshot_debug(1, "snapshot (%u) disabled\n", inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_delete() marks snapshot for deletion
 * Called under i_mutex and snapshot_mutex
 */
static int ext4_snapshot_delete(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (!ext4_snapshot_list(inode)) {
		snapshot_debug(1, "ext4_snapshot_delete() called with "
			       "snapshot file (ino=%lu) not on list\n",
			       inode->i_ino);
		return -EINVAL;
	}

	if (ei->i_flags & EXT4_SNAPFILE_ENABLED_FL) {
		snapshot_debug(1, "delete of enabled snapshot (%u) "
				"is not permitted\n",
				inode->i_generation);
		return -EPERM;
	}

	/* mark deleted for later cleanup to finish the job */
	ei->i_flags |= EXT4_SNAPFILE_DELETED_FL;
	snapshot_debug(1, "snapshot (%u) marked for deletion\n",
			inode->i_generation);
	return 0;
}

/*
 * ext4_snapshot_remove - removes a snapshot from the list
 * @inode: snapshot inode
 *
 * Removed the snapshot inode from in-memory and on-disk snapshots list of
 * and truncates the snapshot inode.
 * Called from ext4_snapshot_update/cleanup/merge() under snapshot_mutex.
 * Returns 0 on success and <0 on error.
 */
static int ext4_snapshot_remove(struct inode *inode)
{
	handle_t *handle;
	struct ext4_sb_info *sbi;
	struct ext4_inode_info *ei = EXT4_I(inode);
	int err = 0, ret;

	/* elevate ref count until final cleanup */
	if (!igrab(inode))
		return -EIO;

	if (ei->i_flags & (EXT4_SNAPFILE_ENABLED_FL | EXT4_SNAPFILE_INUSE_FL
			   | EXT4_SNAPFILE_ACTIVE_FL)) {
		snapshot_debug(4, "deferred delete of %s snapshot (%u)\n",
				(ei->i_flags & EXT4_SNAPFILE_ACTIVE_FL) ?
				"active" :
				((ei->i_flags & EXT4_SNAPFILE_ENABLED_FL) ?
				"enabled" : "referenced"),
			       inode->i_generation);
		goto out_err;
	}

	/* start large truncate transaction that will be extended/restarted */
	handle = ext4_journal_start(inode, EXT4_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_err;
	}
	sbi = EXT4_SB(inode->i_sb);


	err = extend_or_restart_transaction_inode(handle, inode, 2);
	if (err)
		goto out_handle;

	lock_super(inode->i_sb);
	err = ext4_journal_get_write_access(handle, sbi->s_sbh);
	sbi->s_es->s_snapshot_list = 0;
	if (!err)
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	unlock_super(inode->i_sb);
	if (err)
		goto out_handle;
	/*
	 * At this point, this snapshot is empty and not on the snapshots list.
	 * As long as it was on the list it had to have the LIST flag to prevent
	 * truncate/unlink.  Now that it is removed from the list, the LIST flag
	 * and other snapshot status flags should be cleared.  It will still
	 * have the SNAPFILE and DELETED flag to indicate this is a deleted
	 * snapshot that should not be recycled.  There is no need to mark the
	 * inode dirty, because the 'dynamic' status flags are not persistent.
	 */
	ei->i_flags &= ~EXT4_FL_SNAPSHOT_DYN_MASK;

out_handle:
	ret = ext4_journal_stop(handle);
	if (!err)
		err = ret;
	if (err)
		goto out_err;

	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_DELETE);
	snapshot_debug(1, "snapshot (%u) deleted\n", inode->i_generation);

	err = 0;
out_err:
	/* drop final ref count - taken on entry to this function */
	iput(inode);
	if (err) {
		snapshot_debug(1, "failed to delete snapshot (%u)\n",
				inode->i_generation);
	}
	return err;
}



#endif

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
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
	struct inode *used_by = NULL; /* last non-deleted snapshot found */
	int deleted;
#endif
	int err = 0;

	BUG_ON(read_only && cleanup);
	if (active_snapshot)
		EXT4_I(active_snapshot)->i_flags |=
			EXT4_SNAPFILE_ACTIVE_FL|EXT4_SNAPFILE_LIST_FL;


#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
	if (!active_snapshot || !cleanup || used_by)
		return 0;

	/* if all snapshots are deleted - deactivate active snapshot */
	deleted = EXT4_I(active_snapshot)->i_flags & EXT4_SNAPFILE_DELETED_FL;
	if (deleted && igrab(active_snapshot)) {
		/* lock journal updates before deactivating snapshot */
		sb->s_op->freeze_fs(sb);
		lock_super(sb);
		/* deactivate in-memory active snapshot - cannot fail */
		(void) ext4_snapshot_set_active(sb, NULL);
		/* clear on-disk active snapshot */
		EXT4_SB(sb)->s_es->s_snapshot_inum = 0;
		unlock_super(sb);
		sb->s_op->unfreeze_fs(sb);
		/* remove unused deleted active snapshot */
		err = ext4_snapshot_remove(active_snapshot);
		/* drop the refcount to 0 */
		iput(active_snapshot);
	}
#endif
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
