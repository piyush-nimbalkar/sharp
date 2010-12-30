/*
 * linux/fs/ext4/snapshot_debug.c
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshot debugging.
 */


#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include "snapshot.h"

/*
 * debugfs tunables
 */

const char *snapshot_indent = SNAPSHOT_INDENT_STR + SNAPSHOT_INDENT_MAX;

/*
 * Tunable delay values per snapshot operation for testing of
 * COW race conditions and master snapshot_mutex lock
 */
static const char *snapshot_test_names[SNAPSHOT_TESTS_NUM] = {
	/* delay completion of snapshot create|take */
	"test-take-delay-msec",
	/* delay completion of snapshot shrink|cleanup */
	"test-delete-delay-msec",
	/* delay completion of COW operation */
	"test-cow-delay-msec",
	/* delay submission of tracked read */
	"test-read-delay-msec",
	/* delay completion of COW bitmap operation */
	"test-bitmap-delay-msec",
};

#define SNAPSHOT_TEST_NAMES (sizeof(snapshot_test_names) / \
			     sizeof(snapshot_test_names[0]))

u16 snapshot_enable_test[SNAPSHOT_TESTS_NUM] __read_mostly = {0};
u8 snapshot_enable_debug __read_mostly = 1;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CACHE
u8 cow_cache_offset __read_mostly = 0;
#endif

static struct dentry *ext4_debugfs_dir;
static struct dentry *snapshot_debug;
static struct dentry *snapshot_version;
static struct dentry *snapshot_test[SNAPSHOT_TESTS_NUM];
#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CACHE
static struct dentry *cow_cache;
#endif

static char snapshot_version_str[] = EXT4_SNAPSHOT_VERSION;
static struct debugfs_blob_wrapper snapshot_version_blob = {
	.data = snapshot_version_str,
	.size = sizeof(snapshot_version_str)
};


/*
 * ext4_snapshot_create_debugfs_entry - register ext4 debug hooks
 * Void function doesn't return error if debug hooks are not registered.
 */
void ext4_snapshot_create_debugfs_entry(void)
{
	int i;

	ext4_debugfs_dir = debugfs_create_dir("ext4", NULL);
	if (!ext4_debugfs_dir)
		return;
	snapshot_debug = debugfs_create_u8("snapshot-debug", S_IRUGO|S_IWUSR,
					   ext4_debugfs_dir,
					   &snapshot_enable_debug);
	snapshot_version = debugfs_create_blob("snapshot-version", S_IRUGO,
					       ext4_debugfs_dir,
					       &snapshot_version_blob);
	for (i = 0; i < SNAPSHOT_TESTS_NUM && i < SNAPSHOT_TEST_NAMES; i++)
		snapshot_test[i] = debugfs_create_u16(snapshot_test_names[i],
					      S_IRUGO|S_IWUSR,
					      ext4_debugfs_dir,
					      &snapshot_enable_test[i]);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CACHE
	cow_cache = debugfs_create_u8("cow-cache", S_IRUGO,
					   ext4_debugfs_dir,
					   &cow_cache_offset);
#endif
}

/*
 * ext4_snapshot_remove_debugfs_entry - unregister ext4 debug hooks
 * checks if the hooks have been registered before unregistering them.
 */
void ext4_snapshot_remove_debugfs_entry(void)
{
	int i;

	if (!ext4_debugfs_dir)
		return;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CACHE
	if (cow_cache)
		debugfs_remove(cow_cache);
#endif
	for (i = 0; i < SNAPSHOT_TESTS_NUM && i < SNAPSHOT_TEST_NAMES; i++)
		if (snapshot_test[i])
			debugfs_remove(snapshot_test[i]);
	if (snapshot_version)
		debugfs_remove(snapshot_version);
	if (snapshot_debug)
		debugfs_remove(snapshot_debug);
	debugfs_remove(ext4_debugfs_dir);
}


