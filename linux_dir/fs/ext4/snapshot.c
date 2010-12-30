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

