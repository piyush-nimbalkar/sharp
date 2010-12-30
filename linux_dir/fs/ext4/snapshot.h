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



#define ext4_snapshot_cow(handle, inode, bh, cow) 0

#define ext4_snapshot_move(handle, inode, block, num, move) (num)

/*
 * Block access functions
 */


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
	return 0;
}

static inline void exit_ext4_snapshot(void)
{
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








#endif	/* _LINUX_EXT4_SNAPSHOT_H */
