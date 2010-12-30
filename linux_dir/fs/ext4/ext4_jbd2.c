/*
 * Interface between ext4 and JBD
 *
 * Copyright (C) 2008-2010 CTERA Networks
 * Added snapshot support, Amir Goldstein <amir73il@users.sf.net>, 2008
 */

#include "ext4_jbd2.h"
#include "snapshot.h"

#include <trace/events/ext4.h>

int __ext4_journal_get_undo_access(const char *where, unsigned int line,
				   handle_t *handle, struct buffer_head *bh)
{
	int err = 0;

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_get_undo_access(handle, bh);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_JBD
	if (!err)
		err = ext4_snapshot_get_undo_access(handle, bh);
#endif
		if (err)
			ext4_journal_abort_handle(where, line, __func__, bh,
						  handle, err);
	}
	return err;
}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_JBD
int __ext4_journal_get_write_access_inode(const char *where, unsigned int line,
					 handle_t *handle, struct inode *inode,
					 struct buffer_head *bh)
#else

int __ext4_journal_get_write_access(const char *where, unsigned int line,
				    handle_t *handle, struct buffer_head *bh)
#endif
{
	int err = 0;

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_get_write_access(handle, bh);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_JBD
	if (!err)
		err = ext4_snapshot_get_write_access(handle, inode, bh);
#endif
		if (err)
			ext4_journal_abort_handle(where, line, __func__, bh,
						  handle, err);
	}
	return err;
}

/*
 * The ext4 forget function must perform a revoke if we are freeing data
 * which has been journaled.  Metadata (eg. indirect blocks) must be
 * revoked in all cases.
 *
 * "bh" may be NULL: a metadata block may have been freed from memory
 * but there may still be a record of it in the journal, and that record
 * still needs to be revoked.
 *
 * If the handle isn't valid we're not journaling, but we still need to
 * call into ext4_journal_revoke() to put the buffer head.
 */
int __ext4_forget(const char *where, unsigned int line, handle_t *handle,
		  int is_metadata, struct inode *inode,
		  struct buffer_head *bh, ext4_fsblk_t blocknr)
{
	int err;

	might_sleep();

	trace_ext4_forget(inode, is_metadata, blocknr);
	BUFFER_TRACE(bh, "enter");

	jbd_debug(4, "forgetting bh %p: is_metadata = %d, mode %o, "
		  "data mode %x\n",
		  bh, is_metadata, inode->i_mode,
		  test_opt(inode->i_sb, DATA_FLAGS));

	/* In the no journal case, we can just do a bforget and return */
	if (!ext4_handle_valid(handle)) {
		bforget(bh);
		return 0;
	}

	/* Never use the revoke function if we are doing full data
	 * journaling: there is no need to, and a V1 superblock won't
	 * support it.  Otherwise, only skip the revoke on un-journaled
	 * data blocks. */

	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA ||
	    (!is_metadata && !ext4_should_journal_data(inode))) {
		if (bh) {
			BUFFER_TRACE(bh, "call jbd2_journal_forget");
			err = jbd2_journal_forget(handle, bh);
			if (err)
				ext4_journal_abort_handle(where, line, __func__,
							  bh, handle, err);
			return err;
		}
		return 0;
	}

	/*
	 * data!=journal && (is_metadata || should_journal_data(inode))
	 */
	BUFFER_TRACE(bh, "call jbd2_journal_revoke");
	err = jbd2_journal_revoke(handle, blocknr, bh);
	if (err) {
		ext4_journal_abort_handle(where, line, __func__,
					  bh, handle, err);
		__ext4_abort(inode->i_sb, where, line,
			   "error %d when attempting revoke", err);
	}
	BUFFER_TRACE(bh, "exit");
	return err;
}

int __ext4_journal_get_create_access(const char *where, unsigned int line,
				handle_t *handle, struct buffer_head *bh)
{
	int err = 0;

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_get_create_access(handle, bh);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_JBD
	if (!err)
		err = ext4_snapshot_get_create_access(handle, bh);
#endif
		if (err)
			ext4_journal_abort_handle(where, line, __func__,
						  bh, handle, err);
	}
	return err;
}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_RELEASE
int __ext4_journal_release_buffer(const char *where, handle_t *handle,
				struct buffer_head *bh)
{
	int err = 0;

	if (IS_COWING(handle))
		goto out;

	/*
	 * Trying to cancel a previous call to get_write_access(), which may
	 * have resulted in a single COW operation.  We don't need to add
	 * user credits, but if COW credits are too low we will try to
	 * extend the transaction to compensate for the buffer credits used
	 * by the extra COW operation.
	 */
	err = ext4_journal_extend(handle, 0);
	if (err > 0) {
		/* well, we can't say we didn't try - now lets hope
		 * we have enough buffer credits to spare */
		snapshot_debug(1, "%s: warning: couldn't extend transaction "
				"from %s (credits=%d/%d)\n", __func__,
				where, handle->h_buffer_credits,
				((ext4_handle_t *)handle)->h_user_credits);
		err = 0;
	}
	ext4_journal_trace(SNAP_WARN, where, handle, -1);
out:
	jbd2_journal_release_buffer(handle, bh);
	return err;
}

#endif


int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct buffer_head *bh)
{
	int err = 0;

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_dirty_metadata(handle, bh);
		if (err)
			ext4_journal_abort_handle(where, line, __func__,
						  bh, handle, err);
#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CREDITS
	if (err)
		return err;
	if (!IS_COWING(handle)) {
		struct journal_head *jh = bh2jh(bh);
		jbd_lock_bh_state(bh);
		if (jh->b_modified == 1) {
			/*
			 * buffer_credits was decremented when buffer was
			 * modified for the first time in the current
			 * transaction, which may have been during a COW
			 * operation.  We decrement user_credits and mark
			 * b_modified = 2, on the first time that the buffer
			 * is modified not during a COW operation (!h_cowing).
			 */
			jh->b_modified = 2;
			((ext4_handle_t *)handle)->h_user_credits--;
		}
		jbd_unlock_bh_state(bh);
	}
#endif
	} else {
		if (inode)
			mark_buffer_dirty_inode(bh, inode);
		else
			mark_buffer_dirty(bh);
		if (inode && inode_needs_sync(inode)) {
			sync_dirty_buffer(bh);
			if (buffer_req(bh) && !buffer_uptodate(bh)) {
				struct ext4_super_block *es;

				es = EXT4_SB(inode->i_sb)->s_es;
				es->s_last_error_block =
					cpu_to_le64(bh->b_blocknr);
				ext4_error_inode(inode, where, line,
						 bh->b_blocknr,
					"IO error syncing itable block");
				err = -EIO;
			}
		}
	}
	return err;
}

int __ext4_handle_dirty_super(const char *where, unsigned int line,
			      handle_t *handle, struct super_block *sb)
{
	struct buffer_head *bh = EXT4_SB(sb)->s_sbh;
	int err = 0;

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_dirty_metadata(handle, bh);
		if (err)
			ext4_journal_abort_handle(where, line, __func__,
						  bh, handle, err);
	} else
		sb->s_dirt = 1;
	return err;
}
