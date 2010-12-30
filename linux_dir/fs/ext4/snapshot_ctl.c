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


#define ext4_snapshot_reset_bitmap_cache(sb, init) 0


/*
 * Snapshot constructor/destructor
 */

/* with no exclude inode, exclude bitmap is reset to 0 */
#define ext4_snapshot_init_bitmap_cache(sb, create)	\
		ext4_snapshot_reset_bitmap_cache(sb, 1)

int ext4_snapshot_load(struct super_block *sb, struct ext4_super_block *es,
		int read_only)
{
	return 0;
}

void ext4_snapshot_destroy(struct super_block *sb)
{
}
