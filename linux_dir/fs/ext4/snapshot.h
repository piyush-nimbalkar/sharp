/*
 * linux/fs/ext4/snapshot.h
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshot extensions.
 */

#ifndef _LINUX_EXT4_SNAPSHOT_H
#define _LINUX_EXT4_SNAPSHOT_H

#include <linux/version.h>
#include <linux/delay.h>
#include "ext4_jbd2.h"
#include "snapshot_debug.h"


#define EXT4_SNAPSHOT_VERSION "ext4 snapshot v1.0.13-rc3 (1-Nov-2010)"

/*
 * use signed 64bit for snapshot image addresses
 * negative addresses are used to reference snapshot meta blocks
 */
#define ext4_snapblk_t long long

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34))
/* one snapshot patch fits all kernel versions */
#define dquot_file_open generic_file_open
#define dquot_alloc_block vfs_dq_alloc_block
#define dquot_free_block vfs_dq_free_block
#endif

/*
 * We assert that snapshot must use a file system with block size == page
 * size (4K) and that the first file system block is block 0.
 * Snapshot inode direct blocks are reserved for snapshot meta blocks.
 * Snapshot inode single indirect blocks are not used.
 * Snapshot image starts at the first double indirect block.
 * This way, a snapshot image block group can be mapped with 1 double
 * indirect block + 32 indirect blocks.
 */
#define SNAPSHOT_BLOCK_SIZE		PAGE_SIZE
#define SNAPSHOT_BLOCK_SIZE_BITS	PAGE_SHIFT
#define	SNAPSHOT_ADDR_PER_BLOCK		(SNAPSHOT_BLOCK_SIZE / sizeof(__u32))
#define SNAPSHOT_ADDR_PER_BLOCK_BITS	(SNAPSHOT_BLOCK_SIZE_BITS - 2)
#define SNAPSHOT_DIR_BLOCKS		EXT4_NDIR_BLOCKS
#define SNAPSHOT_IND_BLOCKS		SNAPSHOT_ADDR_PER_BLOCK

#define SNAPSHOT_BLOCKS_PER_GROUP_BITS	15
#define SNAPSHOT_BLOCKS_PER_GROUP					\
	(1<<SNAPSHOT_BLOCKS_PER_GROUP_BITS) /* 32K */
#define SNAPSHOT_BLOCK_GROUP(block)		\
	((block)>>SNAPSHOT_BLOCKS_PER_GROUP_BITS)
#define SNAPSHOT_BLOCK_GROUP_OFFSET(block)	\
	((block)&(SNAPSHOT_BLOCKS_PER_GROUP-1))
#define SNAPSHOT_BLOCK_TUPLE(block)		\
	(ext4_fsblk_t)SNAPSHOT_BLOCK_GROUP_OFFSET(block), \
	(ext4_fsblk_t)SNAPSHOT_BLOCK_GROUP(block)
#define SNAPSHOT_IND_PER_BLOCK_GROUP_BITS				\
	(SNAPSHOT_BLOCKS_PER_GROUP_BITS-SNAPSHOT_ADDR_PER_BLOCK_BITS)
#define SNAPSHOT_IND_PER_BLOCK_GROUP			\
	(1<<SNAPSHOT_IND_PER_BLOCK_GROUP_BITS) /* 32 */
#define SNAPSHOT_DIND_BLOCK_GROUPS_BITS					\
	(SNAPSHOT_ADDR_PER_BLOCK_BITS-SNAPSHOT_IND_PER_BLOCK_GROUP_BITS)
#define SNAPSHOT_DIND_BLOCK_GROUPS			\
	(1<<SNAPSHOT_DIND_BLOCK_GROUPS_BITS) /* 32 */

#define SNAPSHOT_BLOCK_OFFSET				\
	(SNAPSHOT_DIR_BLOCKS+SNAPSHOT_IND_BLOCKS)
#define SNAPSHOT_BLOCK(iblock)					\
	((ext4_snapblk_t)(iblock) - SNAPSHOT_BLOCK_OFFSET)
#define SNAPSHOT_IBLOCK(block)						\
	(ext4_fsblk_t)((block) + SNAPSHOT_BLOCK_OFFSET)

#define SNAPSHOT_BYTES_OFFSET					\
	(SNAPSHOT_BLOCK_OFFSET << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_ISIZE(size)			\
	((size) + SNAPSHOT_BYTES_OFFSET)

#define SNAPSHOT_SET_SIZE(inode, size)				\
	(EXT4_I(inode)->i_disksize = SNAPSHOT_ISIZE(size))
#define SNAPSHOT_SIZE(inode)					\
	(EXT4_I(inode)->i_disksize - SNAPSHOT_BYTES_OFFSET)
#define SNAPSHOT_SET_BLOCKS(inode, blocks)			\
	SNAPSHOT_SET_SIZE((inode),				\
			(loff_t)(blocks) << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_BLOCKS(inode)					\
	(ext4_fsblk_t)(SNAPSHOT_SIZE(inode) >> SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_ENABLED(inode)				\
	i_size_write((inode), SNAPSHOT_SIZE(inode))
#define SNAPSHOT_SET_DISABLED(inode)		\
	i_size_write((inode), 0)

#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_DATA
enum ext4_bh_state_bits {
	BH_Partial_Write = 31,	/* Buffer should be read before write */
};

BUFFER_FNS(Partial_Write, partial_write)
#endif


#define ext4_snapshot_cow(handle, inode, bh, cow) 0

#define ext4_snapshot_move(handle, inode, block, num, move) (num)

/*
 * Block access functions
 */

#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_JBD
/*
 * get_write_access() is called before writing to a metadata block
 * if @inode is not NULL, then this is an inode's indirect block
 * otherwise, this is a file system global metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_write_access(handle_t *handle,
		struct inode *inode, struct buffer_head *bh)
{
	return ext4_snapshot_cow(handle, inode, bh, 1);
}

/*
 * called from ext4_journal_get_undo_access(),
 * which is called for group bitmap block from:
 * 1. ext4_free_blocks_sb_inode() before deleting blocks
 * 2. ext4_new_blocks() before allocating blocks
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_undo_access(handle_t *handle,
		struct buffer_head *bh)
{
	return ext4_snapshot_cow(handle, NULL, bh, 1);
}

/*
 * get_create_access() is called after allocating a new metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_create_access(handle_t *handle,
		struct buffer_head *bh)
{
	/*
	 * This block shouldn't need to be COWed if get_delete_access() was
	 * called for all deleted blocks.  However, it may need to be COWed
	 * if fsck was run and if it had freed some blocks without moving them
	 * to snapshot.  In the latter case, -EIO will be returned.
	 */
	return ext4_snapshot_cow(handle, NULL, bh, 0);
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_DATA
/*
 * get_move_access() - move block to snapshot
 * @handle:	JBD handle
 * @inode:	owner of @block
 * @block:	address of @block
 * @move:	if false, only test if @block needs to be moved
 *
 * Called from ext4_get_blocks_handle() before overwriting a data block,
 * when buffer_move() is true.  Specifically, only data blocks of regular files,
 * whose data is not being journaled are moved on full page write.
 * Journaled data blocks are COWed on get_write_access().
 * Snapshots and excluded files blocks are never moved-on-write.
 * If @move is true, then truncate_mutex is held.
 *
 * Return values:
 * = 1 - @block was moved or may not be overwritten
 * = 0 - @block may be overwritten
 * < 0 - error
 */
static inline int ext4_snapshot_get_move_access(handle_t *handle,
		struct inode *inode, ext4_fsblk_t block, int move)
{
	return ext4_snapshot_move(handle, inode, block, 1, move);
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_DELETE
/*
 * get_delete_access() - move count blocks to snapshot
 * @handle:	JBD handle
 * @inode:	owner of blocks
 * @block:	address of start @block
 * @count:	no. of blocks to move
 *
 * Called from ext4_free_blocks_sb_inode() before deleting blocks with
 * truncate_mutex held
 *
 * Return values:
 * > 0 - no. of blocks that were moved to snapshot and may not be deleted
 * = 0 - @block may be deleted
 * < 0 - error
 */
static inline int ext4_snapshot_get_delete_access(handle_t *handle,
		struct inode *inode, ext4_fsblk_t block, int count)
{
	return ext4_snapshot_move(handle, inode, block, count, 1);
}

#endif

/*
 * Snapshot constructor/destructor
 */
extern int ext4_snapshot_load(struct super_block *sb,
		struct ext4_super_block *es, int read_only);
extern int ext4_snapshot_update(struct super_block *sb, int cleanup,
		int read_only);
extern void ext4_snapshot_destroy(struct super_block *sb);

static inline int init_ext4_snapshot(void)
{
	init_ext4_snapshot_debug();
	return 0;
}

static inline void exit_ext4_snapshot(void)
{
	exit_ext4_snapshot_debug();
}


/* balloc.c */
extern struct buffer_head *read_block_bitmap(struct super_block *sb,
					     unsigned int block_group);

/* namei.c */

/* inode.c */
extern ext4_fsblk_t ext4_get_inode_block(struct super_block *sb,
					   unsigned long ino,
					   struct ext4_iloc *iloc);

/* super.c */

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/* tests if @inode is a snapshot file */
static inline int ext4_snapshot_file(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		/* a snapshots directory */
		return 0;
	return EXT4_I(inode)->i_flags & EXT4_SNAPFILE_FL;
}

/* tests if @inode is on the on-disk snapshot list */
static inline int ext4_snapshot_list(struct inode *inode)
{
	return EXT4_I(inode)->i_flags & EXT4_SNAPFILE_LIST_FL;
}
#endif


#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/*
 * ext4_snapshot_excluded():
 * Checks if the file should be excluded from snapshot.
 *
 * Returns 0 for normal file.
 * Returns > 0 for 'excluded' file.
 * Returns < 0 for 'ignored' file (stonger than 'excluded').
 *
 * Excluded and ignored file blocks are not moved to snapshot.
 * Ignored file metadata blocks are not COWed to snapshot.
 * Excluded file metadata blocks are zeroed in the snapshot file.
 * XXX: Excluded files code is experimental,
 *      but ignored files code isn't.
 */
static inline int ext4_snapshot_excluded(struct inode *inode)
{
	/* directory blocks and global filesystem blocks cannot be 'excluded' */
	if (!inode || !S_ISREG(inode->i_mode))
		return 0;
	/* snapshot files are 'ignored' */
	if (ext4_snapshot_file(inode))
		return -1;
	return 0;
}
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_DATA
/*
 * check if @inode data blocks should be moved-on-write
 */
static inline int ext4_snapshot_should_move_data(struct inode *inode)
{
#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
	if (ext4_snapshot_excluded(inode))
		return 0;
#endif
	/* when a data block is journaled, it is already COWed as metadata */
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return 0;
	if (ext4_should_journal_data(inode))
		return 0;
	return 1;
}
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/*
 * tests if the file system has an active snapshot and returns its inode.
 * active snapshot is only changed under journal_lock_updates(),
 * so it is safe to use the returned inode during a transaction.
 */
static inline struct inode *ext4_snapshot_has_active(struct super_block *sb)
{
	return EXT4_SB(sb)->s_active_snapshot;
}

/*
 * tests if @inode is the current active snapshot.
 * active snapshot is only changed under journal_lock_updates(),
 * so the test result never changes during a transaction.
 */
static inline int ext4_snapshot_is_active(struct inode *inode)
{
	return (inode == EXT4_SB(inode->i_sb)->s_active_snapshot);
}
#endif



#endif	/* _LINUX_EXT4_SNAPSHOT_H */
