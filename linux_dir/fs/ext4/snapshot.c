/*
 * linux/fs/ext4/snapshot.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshots core functions.
 */

#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK_MOVE
#include <linux/quotaops.h>
#endif
#include "snapshot.h"
#include "ext4.h"

#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK
#define snapshot_debug_hl(n, f, a...) snapshot_debug_l(n, handle ? \
					IS_COWING(handle) : 0, f, ## a)

/*
 * ext4_snapshot_map_blocks() - helper function for
 * ext4_snapshot_test_and_cow().  Test if blocks are mapped in snapshot file.
 * If @block is not mapped and if @cmd is non zero, try to allocate @maxblocks.
 * Also used by ext4_snapshot_create() to pre-allocate snapshot blocks.
 *
 * Return values:
 * > 0 - no. of mapped blocks in snapshot file
 * = 0 - @block is not mapped in snapshot file
 * < 0 - error
 */
int ext4_snapshot_map_blocks(handle_t *handle, struct inode *inode,
			      ext4_snapblk_t block, unsigned long maxblocks,
			      ext4_fsblk_t *mapped, int cmd)
{
	int err;
	struct ext4_map_blocks map;
	map.m_len = maxblocks;
	map.m_pblk = SNAPSHOT_IBLOCK(block);
#ifdef WARNING_NOT_IMPLEMENTED
	map.m_flag = ?;
#endif
	err = ext4_map_blocks(handle, inode, &map, cmd);
	/*
	 * ext4_get_blocks_handle() returns number of blocks
	 * mapped. 0 in case of a HOLE.
	 */
	if (mapped && err > 0)
		*mapped = map.m_pblk;

	snapshot_debug_hl(4, "snapshot (%u) map_blocks "
			"[%lld/%lld] = [%lld/%lld] "
			"cmd=%d, maxblocks=%lu, mapped=%d\n",
			inode->i_generation,
			SNAPSHOT_BLOCK_TUPLE(block),
			SNAPSHOT_BLOCK_TUPLE(map.m_pblk),
			cmd, maxblocks, err);
	return err;
}
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE_READ
/*
 * ext4_snapshot_get_inode_access() - called from ext4_get_blocks_handle()
 * on snapshot file access.
 * return value <0 indicates access not granted
 * return value 0 indicates normal inode access
 * return value 1 indicates snapshot inode read through access
 * in which case 'prev_snapshot' is pointed to the previous snapshot
 * on the list or set to NULL to indicate read through to block device.
 */
int ext4_snapshot_get_inode_access(handle_t *handle, struct inode *inode,
		ext4_fsblk_t iblock, int count, int cmd,
		struct inode **prev_snapshot)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	unsigned int flags = ei->i_flags;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK
#ifdef CONFIG_EXT4_FS_DEBUG
	ext4_fsblk_t block = SNAPSHOT_BLOCK(iblock);
	unsigned long block_group = (iblock < SNAPSHOT_BLOCK_OFFSET ? -1 :
			SNAPSHOT_BLOCK_GROUP(block));
	ext4_grpblk_t blk = (iblock < SNAPSHOT_BLOCK_OFFSET ? iblock :
			SNAPSHOT_BLOCK_GROUP_OFFSET(block));
	snapshot_debug_hl(4, "snapshot (%u) get_blocks [%d/%lu] count=%d "
			  "cmd=%d\n", inode->i_generation, blk, block_group,
			  count, cmd);
#endif

	if (SNAPMAP_ISSPECIAL(cmd)) {
		/*
		 * COWing or moving blocks to active snapshot
		 */
		BUG_ON(!handle || !IS_COWING(handle));
		BUG_ON(!(flags & EXT4_SNAPFILE_ACTIVE_FL));
		BUG_ON(iblock < SNAPSHOT_BLOCK_OFFSET);
		return 0;
	} else if (cmd)
		BUG_ON(handle && IS_COWING(handle));
#endif

	if (!(flags & EXT4_SNAPFILE_LIST_FL))
		return 0;

	if (cmd) {
		/* snapshot inode write access */
		snapshot_debug(1, "snapshot (%u) is read-only"
				" - write access denied!\n",
				inode->i_generation);
		return -EPERM;
	} else {
		/* snapshot inode read access */
		if (iblock < SNAPSHOT_BLOCK_OFFSET)
			/* snapshot reserved blocks */
			return 0;
		/*
		 * non NULL handle indicates this is test_and_cow()
		 * checking if snapshot block is mapped
		 */
		if (handle)
			return 0;
	}

	/*
	 * Snapshot image read through access: (!cmd && !handle)
	 * indicates this is ext4_snapshot_readpage()
	 * calling ext4_snapshot_get_block()
	 */
	*prev_snapshot = NULL;
	return ext4_snapshot_is_active(inode) ? 1 : 0;
}
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK_COW
/*
 * COW helper functions
 */

/*
 * copy buffer @bh to (locked) snapshot buffer @sbh and mark it uptodate
 */
static inline void
__ext4_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh)
{
	memcpy(sbh->b_data, bh->b_data, SNAPSHOT_BLOCK_SIZE);
	set_buffer_uptodate(sbh);
}


/*
 * ext4_snapshot_complete_cow()
 * Unlock a newly COWed snapshot buffer and complete the COW operation.
 * Optionally, sync the buffer to disk or add it to the current transaction
 * as dirty data.
 */
static inline int
ext4_snapshot_complete_cow(handle_t *handle,
		struct buffer_head *sbh, struct buffer_head *bh, int sync)
{
	int err = 0;

	unlock_buffer(sbh);
	if (handle) {
#ifdef WARNING_NOT_IMPLEMENTED
		/*Patch snapshot_block_cow_patch*/
		err = ext4_journal_dirty_data(handle, sbh);
		if (err)
			goto out;
#endif
	}
	mark_buffer_dirty(sbh);
	if (sync)
		sync_dirty_buffer(sbh);

out:
	return err;
}

/*
 * ext4_snapshot_copy_buffer_cow()
 * helper function for ext4_snapshot_test_and_cow()
 * copy COWed buffer to new allocated (locked) snapshot buffer
 * add complete the COW operation
 */
static inline int
ext4_snapshot_copy_buffer_cow(handle_t *handle,
				   struct buffer_head *sbh,
				   struct buffer_head *bh)
{
	__ext4_snapshot_copy_buffer(sbh, bh);
	return ext4_snapshot_complete_cow(handle, sbh, bh, 0);
}

/*
 * ext4_snapshot_copy_buffer()
 * helper function for ext4_snapshot_take()
 * used for initializing pre-allocated snapshot blocks
 * copy buffer to snapshot buffer and sync to disk
 * 'mask' block bitmap with exclude bitmap before copying to snapshot.
 */
void ext4_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh, const char *mask)
{
	lock_buffer(sbh);
	__ext4_snapshot_copy_buffer(sbh, bh);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);
}




/*
 * COW functions
 */

#ifdef CONFIG_EXT4_FS_DEBUG
static void
__ext4_snapshot_trace_cow(const char *where, handle_t *handle,
		struct super_block *sb, struct inode *inode,
		struct buffer_head *bh, ext4_fsblk_t block, int cmd)
{
	unsigned long inode_group = 0;
	ext4_grpblk_t inode_offset = 0;

	if (inode) {
		inode_group = (inode->i_ino - 1) /
			EXT4_INODES_PER_GROUP(sb);
		inode_offset = (inode->i_ino - 1) %
			EXT4_INODES_PER_GROUP(sb);
	}
	snapshot_debug_hl(4, "%s(i:%d/%ld, b:%lld/%lld)"
			" h_ref=%d, cmd=%d\n",
			where, inode_offset, inode_group,
			SNAPSHOT_BLOCK_GROUP_OFFSET(block),
			SNAPSHOT_BLOCK_GROUP(block),
			handle->h_ref, cmd);
}

#define ext4_snapshot_trace_cow(where, handle, sb, inode, bh, block, cmd) \
	if (snapshot_enable_debug >= 4)					\
		__ext4_snapshot_trace_cow(where, handle, sb, inode,	\
				bh, block, cmd)
#else
#define ext4_snapshot_trace_cow(where, handle, sb, inode, bh, block, cmd)
#endif


/*
 * Begin COW or move operation.
 * No locks needed here, because @handle is a per-task struct.
 */
static inline void ext4_snapshot_cow_begin(handle_t *handle)
{
	snapshot_debug_hl(4, "{\n");
	IS_COWING(handle) = 1;
}

/*
 * End COW or move operation.
 * No locks needed here, because @handle is a per-task struct.
 */
static inline void ext4_snapshot_cow_end(const char *where,
		handle_t *handle, ext4_fsblk_t block, int err)
{
	IS_COWING(handle) = 0;
	snapshot_debug_hl(4, "} = %d\n", err);
	snapshot_debug_hl(4, ".\n");
	if (err < 0)
		snapshot_debug(1, "%s(b:%lld/%lld) failed!"
				" h_ref=%d, err=%d\n", where,
				SNAPSHOT_BLOCK_GROUP_OFFSET(block),
				SNAPSHOT_BLOCK_GROUP(block),
				handle->h_ref, err);
}

/*
 * ext4_snapshot_test_and_cow - COW metadata block
 * @where:	name of caller function
 * @handle:	JBD handle
 * @inode:	owner of blocks (NULL for global metadata blocks)
 * @bh:		buffer head of metadata block
 * @cow:	if false, return -EIO if block needs to be COWed
 *
 * Return values:
 * = 0 - @block was COWed or doesn't need to be COWed
 * < 0 - error
 */
int ext4_snapshot_test_and_cow(const char *where, handle_t *handle,
		struct inode *inode, struct buffer_head *bh, int cow)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	struct buffer_head *sbh = NULL;
	ext4_fsblk_t block = bh->b_blocknr, blk = 0;
	int err = 0, clear = 0;

	if (!active_snapshot)
		/* no active snapshot - no need to COW */
		return 0;

	ext4_snapshot_trace_cow(where, handle, sb, inode, bh, block, cow);

	if (IS_COWING(handle)) {
		/* avoid recursion on active snapshot updates */
		WARN_ON(inode && inode != active_snapshot);
		snapshot_debug_hl(4, "active snapshot update - "
				  "skip block cow!\n");
		return 0;
	} else if (inode == active_snapshot) {
		/* active snapshot may only be modified during COW */
		snapshot_debug_hl(4, "active snapshot access denied!\n");
		return -EPERM;
	}


	/* BEGIN COWing */
	ext4_snapshot_cow_begin(handle);

	if (inode)
		clear = ext4_snapshot_excluded(inode);
	if (clear < 0) {
		/*
		 * excluded file block access - don't COW and
		 * mark block in exclude bitmap
		 */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
				"mark block (%lld) in exclude bitmap\n",
				inode->i_ino, block);
		cow = 0;
	}

	if (clear < 0)
		goto cowed;
	if (!err) {
		trace_cow_inc(handle, ok_bitmap);
		goto cowed;
	}

	/* block is in use by snapshot - check if it is mapped */
	err = ext4_snapshot_map_blocks(handle, active_snapshot, block, 1, &blk,
					SNAPMAP_READ);
	if (err < 0)
		goto out;
	if (err > 0) {
		sbh = sb_find_get_block(sb, blk);
		trace_cow_inc(handle, ok_mapped);
		err = 0;
		goto test_pending_cow;
	}

	/* block needs to be COWed */
	err = -EIO;
	if (!cow)
		/* don't COW - we were just checking */
		goto out;

	/* make sure we hold an uptodate source buffer */
	if (!bh || !buffer_mapped(bh))
		goto out;
	if (!buffer_uptodate(bh)) {
		snapshot_debug(1, "warning: non uptodate buffer (%lld)"
				" needs to be copied to active snapshot!\n",
				block);
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh))
			goto out;
	}

	/* try to allocate snapshot block to make a backup copy */
	sbh = ext4_getblk(handle, active_snapshot, SNAPSHOT_IBLOCK(block),
			   SNAPMAP_COW, &err);
	if (!sbh || err < 0)
		goto out;

	blk = sbh->b_blocknr;
	if (!err) {
		/*
		 * we didn't allocate this block -
		 * another COWing task must have allocated it
		 */
		trace_cow_inc(handle, ok_mapped);
		goto test_pending_cow;
	}

	/*
	 * we allocated this block -
	 * copy block data to snapshot and complete COW operation
	 */
	err = ext4_snapshot_copy_buffer_cow(handle, sbh, bh);
	if (err)
		goto out;
	snapshot_debug(3, "block [%lld/%lld] of snapshot (%u) "
			"mapped to block [%lld/%lld]\n",
			SNAPSHOT_BLOCK_TUPLE(block),
			active_snapshot->i_generation,
			SNAPSHOT_BLOCK_TUPLE(sbh->b_blocknr));

	trace_cow_inc(handle, copied);
test_pending_cow:

cowed:
out:
	brelse(sbh);
	/* END COWing */
	ext4_snapshot_cow_end(where, handle, block, err);
	return err;
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK_MOVE
/*
 * ext4_snapshot_test_and_move - move blocks to active snapshot
 * @where:	name of caller function
 * @handle:	JBD handle
 * @inode:	owner of blocks (NULL for global metadata blocks)
 * @block:	address of first block to move
 * @maxblocks:	max. blocks to move
 * @move:	if false, only test if @block needs to be moved
 *
 * Return values:
 * > 0 - no. of blocks that were (or needs to be) moved to snapshot
 * = 0 - @block doesn't need to be moved
 * < 0 - error
 */
int ext4_snapshot_test_and_move(const char *where, handle_t *handle,
	struct inode *inode, ext4_fsblk_t block, int maxblocks, int move)
{
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *active_snapshot = ext4_snapshot_has_active(sb);
	ext4_fsblk_t blk = 0;
	int err = 0, count = maxblocks;
	int excluded = 0;

	if (!active_snapshot)
		/* no active snapshot - no need to move */
		return 0;

	ext4_snapshot_trace_cow(where, handle, sb, inode, NULL, block, move);

	BUG_ON(IS_COWING(handle) || inode == active_snapshot);

	/* BEGIN moving */
	ext4_snapshot_cow_begin(handle);

	if (inode)
		excluded = ext4_snapshot_excluded(inode);
	if (excluded) {
		/* don't move excluded file block to snapshot */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot\n",
				inode->i_ino);
		move = 0;
	}

	if (excluded)
		goto out;
	if (!err) {
		/* block not in COW bitmap - no need to move */
		trace_cow_inc(handle, ok_bitmap);
		goto out;
	}

#ifdef CONFIG_EXT4_FS_DEBUG
	if (inode == NULL &&
		!(EXT4_I(active_snapshot)->i_flags & EXT4_UNRM_FL)) {
		/*
		 * This is ext4_group_extend() "freeing" the blocks that
		 * were added to the block group.  These block should not be
		 * moved to snapshot, unless the snapshot is marked with the
		 * UNRM flag for large snapshot creation test.
		 */
		trace_cow_inc(handle, ok_bitmap);
		err = 0;
		goto out;
	}
#endif

	/* count blocks are in use by snapshot - check if @block is mapped */
	err = ext4_snapshot_map_blocks(handle, active_snapshot, block, 1, &blk,
					SNAPMAP_READ);
	if (err < 0)
		goto out;
	if (err > 0) {
		/* block already mapped in snapshot - no need to move */
		trace_cow_inc(handle, ok_mapped);
		err = 0;
		goto out;
	}

	/* @count blocks need to be moved */
	err = count;
	if (!move)
		/* don't move - we were just checking */
		goto out;

	/* try to move @count blocks from inode to snapshot */
	err = ext4_snapshot_map_blocks(handle, active_snapshot, block,
			count, NULL, SNAPMAP_MOVE);
	if (err <= 0)
		goto out;
	count = err;
	/*
	 * User should no longer be charged for these blocks.
	 * Snapshot file owner was charged for these blocks
	 * when they were mapped to snapshot file.
	 */
	if (inode)
		dquot_free_block(inode, count);
	trace_cow_add(handle, moved, count);
out:
	/* END moving */
	ext4_snapshot_cow_end(where, handle, block, err);
	return err;
}

#endif
