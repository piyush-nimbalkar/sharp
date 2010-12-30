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

