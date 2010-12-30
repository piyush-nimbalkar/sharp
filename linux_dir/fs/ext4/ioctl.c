/*
 * linux/fs/ext4/ioctl.c
 *
 * Copyright (C) 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 * Copyright (C) 2008-2010 CTERA Networks
 * Added snapshot support, Amir Goldstein <amir73il@users.sf.net>, 2008
 */

#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/capability.h>
#include <linux/time.h>
#include <linux/compat.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <asm/uaccess.h>
#include <linux/smp_lock.h>
#include "ext4_jbd2.h"
#include "ext4.h"
#include "snapshot.h"

static void print_inode_dealloc_info(struct inode *inode)
{
	if (!EXT4_I(inode)->i_reserved_data_blocks ||
	    !EXT4_I(inode)->i_reserved_meta_blocks)
		return;

	printk(KERN_DEBUG "ino %lu: %u %u\n", inode->i_ino,
	       EXT4_I(inode)->i_reserved_data_blocks,
	       EXT4_I(inode)->i_reserved_meta_blocks);
}

long ext4_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct ext4_inode_info *ei = EXT4_I(inode);
	unsigned int flags;

	ext4_debug("cmd = %u, arg = %lu\n", cmd, arg);

	switch (cmd) {
	case EXT4_IOC_GETFLAGS:
		ext4_get_inode_flags(ei);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		ext4_snapshot_get_flags(ei, filp);
#endif
		flags = ei->i_flags & EXT4_FL_USER_VISIBLE;
		return put_user(flags, (int __user *) arg);
	case EXT4_IOC_SETFLAGS: {
		handle_t *handle = NULL;
		int err, migrate = 0;
		struct ext4_iloc iloc;
		unsigned int oldflags;
		unsigned int jflag;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		unsigned int snapflags = 0;
#endif

		if (!is_owner_or_cap(inode))
			return -EACCES;

		if (get_user(flags, (int __user *) arg))
			return -EFAULT;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;

		flags = ext4_mask_flags(inode->i_mode, flags);

		err = -EPERM;
		mutex_lock(&inode->i_mutex);
		/* Is it quota file? Do not allow user to mess with it */
		if (IS_NOQUOTA(inode))
			goto flags_out;

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		/* update snapshot 'open' flag under i_mutex */
		ext4_snapshot_get_flags(ei, filp);
#endif
		oldflags = ei->i_flags;

		/* The JOURNAL_DATA flag is modifiable only by root */
		jflag = flags & EXT4_JOURNAL_DATA_FL;

		/*
		 * The IMMUTABLE and APPEND_ONLY flags can only be changed by
		 * the relevant capability.
		 *
		 * This test looks nicer. Thanks to Pauline Middelink
		 */
		if ((flags ^ oldflags) & (EXT4_APPEND_FL | EXT4_IMMUTABLE_FL)) {
			if (!capable(CAP_LINUX_IMMUTABLE))
				goto flags_out;
		}

		/*
		 * The JOURNAL_DATA flag can only be changed by
		 * the relevant capability.
		 */
		if ((jflag ^ oldflags) & (EXT4_JOURNAL_DATA_FL)) {
			if (!capable(CAP_SYS_RESOURCE))
				goto flags_out;
		}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		/*
		 * Snapshot file flags can only be changed by
		 * the relevant capability and under snapshot_mutex lock.
		 */
		snapflags = ((flags | oldflags) & EXT4_FL_SNAPSHOT_MASK);
		if (snapflags) {
			if (!capable(CAP_SYS_RESOURCE)) {
				/* indicate snapshot_mutex not taken */
				snapflags = 0;
				goto flags_out;
			}

			/*
			 * snapshot_mutex should be held throughout the trio
			 * snapshot_{set_flags,take,update}().  It must be taken
			 * before starting the transaction, otherwise
			 * journal_lock_updates() inside snapshot_take()
			 * can deadlock:
			 * A: journal_start()
			 * A: snapshot_mutex_lock()
			 * B: journal_start()
			 * B: snapshot_mutex_lock() (waiting for A)
			 * A: journal_stop()
			 * A: snapshot_take() ->
			 * A: journal_lock_updates() (waiting for B)
			 */
			mutex_lock(&EXT4_SB(inode->i_sb)->s_snapshot_mutex);
		}

#endif

		if (oldflags & EXT4_EXTENTS_FL) {
			/* We don't support clearning extent flags */
			if (!(flags & EXT4_EXTENTS_FL)) {
				err = -EOPNOTSUPP;
				goto flags_out;
			}
		} else if (flags & EXT4_EXTENTS_FL) {
			/* migrate the file */
			migrate = 1;
			flags &= ~EXT4_EXTENTS_FL;
		}

		if (flags & EXT4_EOFBLOCKS_FL) {
			/* we don't support adding EOFBLOCKS flag */
			if (!(oldflags & EXT4_EOFBLOCKS_FL)) {
				err = -EOPNOTSUPP;
				goto flags_out;
			}
		} else if (oldflags & EXT4_EOFBLOCKS_FL)
			ext4_truncate(inode);

		handle = ext4_journal_start(inode, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto flags_out;
		}
		if (IS_SYNC(inode))
			ext4_handle_sync(handle);
		err = ext4_reserve_inode_write(handle, inode, &iloc);
		if (err)
			goto flags_err;

		flags = flags & EXT4_FL_USER_MODIFIABLE;
		flags |= oldflags & ~EXT4_FL_USER_MODIFIABLE;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		err = ext4_snapshot_set_flags(handle, inode, flags);
		if (err)
			goto flags_err;
#else
		ei->i_flags = flags;
#endif

		ext4_set_inode_flags(inode);
		inode->i_ctime = ext4_current_time(inode);

		err = ext4_mark_iloc_dirty(handle, inode, &iloc);
flags_err:
		ext4_journal_stop(handle);
		if (err)
			goto flags_out;

		if ((jflag ^ oldflags) & (EXT4_JOURNAL_DATA_FL))
			err = ext4_change_inode_journal_flag(inode, jflag);
		if (err)
			goto flags_out;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		if (!(oldflags & EXT4_SNAPFILE_LIST_FL) &&
				(flags & EXT4_SNAPFILE_LIST_FL))
			/* setting list flag - take snapshot */
			err = ext4_snapshot_take(inode);
#endif
		if (migrate)
			err = ext4_ext_migrate(inode);
flags_out:
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		if (snapflags & EXT4_SNAPFILE_LIST_FL) {
			/* if clearing list flag, cleanup snapshot list */
			int ret, cleanup = !(flags & EXT4_SNAPFILE_LIST_FL);

			/* update/cleanup snapshots list even if take failed */
			ret = ext4_snapshot_update(inode->i_sb, cleanup, 0);
			if (!err)
				err = ret;
		}

		if (snapflags)
			mutex_unlock(&EXT4_SB(inode->i_sb)->s_snapshot_mutex);
#endif
		mutex_unlock(&inode->i_mutex);
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}
	case EXT4_IOC_GETVERSION:
	case EXT4_IOC_GETVERSION_OLD:
		return put_user(inode->i_generation, (int __user *) arg);
	case EXT4_IOC_SETVERSION:
	case EXT4_IOC_SETVERSION_OLD: {
		handle_t *handle;
		struct ext4_iloc iloc;
		__u32 generation;
		int err;

		if (!is_owner_or_cap(inode))
			return -EPERM;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;
		if (get_user(generation, (int __user *) arg)) {
			err = -EFAULT;
			goto setversion_out;
		}
		handle = ext4_journal_start(inode, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto setversion_out;
		}
		err = ext4_reserve_inode_write(handle, inode, &iloc);
		if (err == 0) {
			inode->i_ctime = ext4_current_time(inode);
			inode->i_generation = generation;
			err = ext4_mark_iloc_dirty(handle, inode, &iloc);
		}
		ext4_journal_stop(handle);
setversion_out:
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}
#ifdef CONFIG_JBD2_DEBUG
	case EXT4_IOC_WAIT_FOR_READONLY:
		/*
		 * This is racy - by the time we're woken up and running,
		 * the superblock could be released.  And the module could
		 * have been unloaded.  So sue me.
		 *
		 * Returns 1 if it slept, else zero.
		 */
		{
			struct super_block *sb = inode->i_sb;
			DECLARE_WAITQUEUE(wait, current);
			int ret = 0;

			set_current_state(TASK_INTERRUPTIBLE);
			add_wait_queue(&EXT4_SB(sb)->ro_wait_queue, &wait);
			if (timer_pending(&EXT4_SB(sb)->turn_ro_timer)) {
				schedule();
				ret = 1;
			}
			remove_wait_queue(&EXT4_SB(sb)->ro_wait_queue, &wait);
			return ret;
		}
#endif
	case EXT4_IOC_GROUP_EXTEND: {
		ext4_fsblk_t n_blocks_count;
		struct super_block *sb = inode->i_sb;
		int err, err2=0;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;

		if (get_user(n_blocks_count, (__u32 __user *)arg))
			return -EFAULT;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		/* avoid snapshot_take() in the middle of group_extend() */
		mutex_lock(&EXT4_SB(sb)->s_snapshot_mutex);
#endif
		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;

		err = ext4_group_extend(sb, EXT4_SB(sb)->s_es, n_blocks_count);
		if (EXT4_SB(sb)->s_journal) {
			jbd2_journal_lock_updates(EXT4_SB(sb)->s_journal);
			err2 = jbd2_journal_flush(EXT4_SB(sb)->s_journal);
			jbd2_journal_unlock_updates(EXT4_SB(sb)->s_journal);
		}
		if (err == 0)
			err = err2;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		mutex_unlock(&EXT4_SB(sb)->s_snapshot_mutex);
#endif
		mnt_drop_write(filp->f_path.mnt);

		return err;
	}

	case EXT4_IOC_MOVE_EXT: {
		struct move_extent me;
		struct file *donor_filp;
		int err;

		if (!(filp->f_mode & FMODE_READ) ||
		    !(filp->f_mode & FMODE_WRITE))
			return -EBADF;

		if (copy_from_user(&me,
			(struct move_extent __user *)arg, sizeof(me)))
			return -EFAULT;
		me.moved_len = 0;

		donor_filp = fget(me.donor_fd);
		if (!donor_filp)
			return -EBADF;

		if (!(donor_filp->f_mode & FMODE_WRITE)) {
			err = -EBADF;
			goto mext_out;
		}

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			goto mext_out;

		err = ext4_move_extents(filp, donor_filp, me.orig_start,
					me.donor_start, me.len, &me.moved_len);
		mnt_drop_write(filp->f_path.mnt);
		if (me.moved_len > 0)
			file_remove_suid(donor_filp);

		if (copy_to_user((struct move_extent __user *)arg,
				 &me, sizeof(me)))
			err = -EFAULT;
mext_out:
		fput(donor_filp);
		return err;
	}

	case EXT4_IOC_GROUP_ADD: {
		struct ext4_new_group_data input;
		struct super_block *sb = inode->i_sb;
		int err, err2=0;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;

		if (copy_from_user(&input, (struct ext4_new_group_input __user *)arg,
				sizeof(input)))
			return -EFAULT;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		/* avoid snapshot_take() in the middle of group_add() */
		mutex_lock(&EXT4_SB(sb)->s_snapshot_mutex);
#endif
		err = ext4_group_add(sb, &input);
		if (EXT4_SB(sb)->s_journal) {
			jbd2_journal_lock_updates(EXT4_SB(sb)->s_journal);
			err2 = jbd2_journal_flush(EXT4_SB(sb)->s_journal);
			jbd2_journal_unlock_updates(EXT4_SB(sb)->s_journal);
		}
		if (err == 0)
			err = err2;
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL
		mutex_unlock(&EXT4_SB(sb)->s_snapshot_mutex);
#endif
		mnt_drop_write(filp->f_path.mnt);

		return err;
	}

	case EXT4_IOC_MIGRATE:
	{
		int err;
		if (!is_owner_or_cap(inode))
			return -EACCES;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;
		/*
		 * inode_mutex prevent write and truncate on the file.
		 * Read still goes through. We take i_data_sem in
		 * ext4_ext_swap_inode_data before we switch the
		 * inode format to prevent read.
		 */
		mutex_lock(&(inode->i_mutex));
		err = ext4_ext_migrate(inode);
		mutex_unlock(&(inode->i_mutex));
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}

	case EXT4_IOC_ALLOC_DA_BLKS:
	{
		int err;
		if (!is_owner_or_cap(inode))
			return -EACCES;

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			return err;
		err = ext4_alloc_da_blocks(inode);
		mnt_drop_write(filp->f_path.mnt);
		return err;
	}

	case EXT4_IOC_DEBUG_DELALLOC:
	{
#ifndef MODULE
		extern spinlock_t inode_lock;
#endif
		struct super_block *sb = inode->i_sb;
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
		struct inode *inode;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		printk(KERN_DEBUG "EXT4-fs debug delalloc of %s\n", sb->s_id);
		printk(KERN_DEBUG "EXT4-fs: dirty blocks %lld free blocks %lld\n",
		       percpu_counter_sum(&sbi->s_dirtyblocks_counter),
		       percpu_counter_sum(&sbi->s_freeblocks_counter));
#ifdef MODULE
		/* Yuck; but the inode_lock spinlock is not exported */
		lock_kernel();
#else
		spin_lock(&inode_lock);
#endif
		if (!list_empty(&sb->s_bdi->wb.b_dirty)) {
			printk(KERN_DEBUG "s_bdi->wb.b_dirty list:\n");
			list_for_each_entry(inode, &sb->s_bdi->wb.b_dirty,
					    i_list) {
				print_inode_dealloc_info(inode);
			}
		}
		if (!list_empty(&sb->s_bdi->wb.b_io)) {
			printk(KERN_DEBUG "s_bdi->wb.b_io list:\n");
			list_for_each_entry(inode, &sb->s_bdi->wb.b_io,
					    i_list) {
				print_inode_dealloc_info(inode);
			}
		}
		if (!list_empty(&sb->s_bdi->wb.b_more_io)) {
			printk(KERN_DEBUG "s_bdi->wb.b_more_io list:\n");
			list_for_each_entry(inode, &sb->s_bdi->wb.b_more_io,
					    i_list) {
				print_inode_dealloc_info(inode);
			}
		}
#ifdef MODULE
		lock_kernel();
#else
		spin_unlock(&inode_lock);
#endif
		printk(KERN_DEBUG "ext4 debug delalloc done\n");
		return 0;
	}


	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
long ext4_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	/* These are just misnamed, they actually get/put from/to user an int */
	switch (cmd) {
	case EXT4_IOC32_GETFLAGS:
		cmd = EXT4_IOC_GETFLAGS;
		break;
	case EXT4_IOC32_SETFLAGS:
		cmd = EXT4_IOC_SETFLAGS;
		break;
	case EXT4_IOC32_GETVERSION:
		cmd = EXT4_IOC_GETVERSION;
		break;
	case EXT4_IOC32_SETVERSION:
		cmd = EXT4_IOC_SETVERSION;
		break;
	case EXT4_IOC32_GROUP_EXTEND:
		cmd = EXT4_IOC_GROUP_EXTEND;
		break;
	case EXT4_IOC32_GETVERSION_OLD:
		cmd = EXT4_IOC_GETVERSION_OLD;
		break;
	case EXT4_IOC32_SETVERSION_OLD:
		cmd = EXT4_IOC_SETVERSION_OLD;
		break;
#ifdef CONFIG_JBD2_DEBUG
	case EXT4_IOC32_WAIT_FOR_READONLY:
		cmd = EXT4_IOC_WAIT_FOR_READONLY;
		break;
#endif
	case EXT4_IOC32_GETRSVSZ:
		cmd = EXT4_IOC_GETRSVSZ;
		break;
	case EXT4_IOC32_SETRSVSZ:
		cmd = EXT4_IOC_SETRSVSZ;
		break;
	case EXT4_IOC32_GROUP_ADD: {
		struct compat_ext4_new_group_input __user *uinput;
		struct ext4_new_group_input input;
		mm_segment_t old_fs;
		int err;

		uinput = compat_ptr(arg);
		err = get_user(input.group, &uinput->group);
		err |= get_user(input.block_bitmap, &uinput->block_bitmap);
		err |= get_user(input.inode_bitmap, &uinput->inode_bitmap);
		err |= get_user(input.inode_table, &uinput->inode_table);
		err |= get_user(input.blocks_count, &uinput->blocks_count);
		err |= get_user(input.reserved_blocks,
				&uinput->reserved_blocks);
		if (err)
			return -EFAULT;
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		err = ext4_ioctl(file, EXT4_IOC_GROUP_ADD,
				 (unsigned long) &input);
		set_fs(old_fs);
		return err;
	}
	case EXT4_IOC_MOVE_EXT:
	case EXT4_IOC_DEBUG_DELALLOC:
		break;
	default:
		return -ENOIOCTLCMD;
	}
	return ext4_ioctl(file, cmd, (unsigned long) compat_ptr(arg));
}
#endif
