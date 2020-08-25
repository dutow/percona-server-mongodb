/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __backup_all(WT_SESSION_IMPL *);
static int __backup_list_append(WT_SESSION_IMPL *, WT_CURSOR_BACKUP *, const char *);
static int __backup_list_uri_append(WT_SESSION_IMPL *, const char *, bool *);
static int __backup_start(WT_SESSION_IMPL *, WT_CURSOR_BACKUP *, bool, const char *[]);
static int __backup_stop(WT_SESSION_IMPL *, WT_CURSOR_BACKUP *);
static int __backup_uri(WT_SESSION_IMPL *, const char *[], bool, bool *, bool *);

/*
 * __curbackup_next --
 *     WT_CURSOR->next method for the backup cursor type.
 */
static int
__curbackup_next(WT_CURSOR *cursor)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cb = (WT_CURSOR_BACKUP *)cursor;
    CURSOR_API_CALL(cursor, session, next, NULL);

    if (cb->list == NULL || cb->list[cb->next] == NULL) {
        F_CLR(cursor, WT_CURSTD_KEY_SET);
        WT_ERR(WT_NOTFOUND);
    }

    cb->iface.key.data = cb->list[cb->next];
    cb->iface.key.size = strlen(cb->list[cb->next]) + 1;
    ++cb->next;

    F_SET(cursor, WT_CURSTD_KEY_INT);

err:
    API_END_RET(session, ret);
}

/*
 * __curbackup_reset --
 *     WT_CURSOR->reset method for the backup cursor type.
 */
static int
__curbackup_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cb = (WT_CURSOR_BACKUP *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);

    cb->next = 0;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __backup_free --
 *     Free list resources for a backup cursor.
 */
static void
__backup_free(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
{
    int i;

    if (cb->list != NULL) {
        for (i = 0; cb->list[i] != NULL; ++i)
            __wt_free(session, cb->list[i]);
        __wt_free(session, cb->list);
    }
}

/*
 * __curbackup_close --
 *     WT_CURSOR->close method for the backup cursor type.
 */
static int
__curbackup_close(WT_CURSOR *cursor)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cb = (WT_CURSOR_BACKUP *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    /*
     * When starting a hot backup, we serialize hot backup cursors and set the connection's
     * hot-backup flag. Once that's done, we set the cursor's backup-locker flag, implying the
     * cursor owns all necessary cleanup (including removing temporary files), regardless of error
     * or success. The cursor's backup-locker flag is never cleared (it's just discarded when the
     * cursor is closed), because that cursor will never not be responsible for cleanup.
     */
    if (F_ISSET(cb, WT_CURBACKUP_DUP)) {
        __backup_free(session, cb);
        /* Make sure the original backup cursor is still open. */
        WT_ASSERT(session, F_ISSET(session, WT_SESSION_BACKUP_CURSOR));
        F_CLR(session, WT_SESSION_BACKUP_DUP);
        F_CLR(cb, WT_CURBACKUP_DUP);
    } else if (F_ISSET(cb, WT_CURBACKUP_LOCKER))
        WT_TRET(__backup_stop(session, cb));

    __wt_cursor_close(cursor);
    session->bkp_cursor = NULL;

    API_END_RET(session, ret);
}

/*
 * __wt_curbackup_open --
 *     WT_SESSION->open_cursor method for the backup cursor type.
 */
int
__wt_curbackup_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value_notsup,                   /* get-value */
      __wt_cursor_set_key_notsup,                     /* set-key */
      __wt_cursor_set_value_notsup,                   /* set-value */
      __wt_cursor_compare_notsup,                     /* compare */
      __wt_cursor_equals_notsup,                      /* equals */
      __curbackup_next,                               /* next */
      __wt_cursor_notsup,                             /* prev */
      __curbackup_reset,                              /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __wt_cursor_notsup,                             /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_reconfigure_notsup,                 /* reconfigure */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __curbackup_close);                             /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;

    WT_STATIC_ASSERT(offsetof(WT_CURSOR_BACKUP, iface) == 0);

    WT_RET(__wt_calloc_one(session, &cb));
    cursor = (WT_CURSOR *)cb;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = "S";  /* Return the file names as the key. */
    cursor->value_format = ""; /* No value. */

    session->bkp_cursor = cb;

    /*
     * Start the backup and fill in the cursor's list. Acquire the schema lock, we need a consistent
     * view when creating a copy.
     */
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __backup_start(session, cb, other != NULL, cfg)));
    WT_ERR(ret);

    WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

    if (0) {
err:
        WT_TRET(__curbackup_close(cursor));
        *cursorp = NULL;
    }

    return (ret);
}

/*
 * __backup_log_append --
 *     Append log files needed for backup.
 */
static int
__backup_log_append(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, bool active)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    u_int i, logcount;
    char **logfiles;

    conn = S2C(session);
    logfiles = NULL;
    logcount = 0;
    ret = 0;

    if (conn->log) {
        WT_ERR(__wt_log_get_backup_files(session, &logfiles, &logcount, &cb->maxid, active));
        for (i = 0; i < logcount; i++)
            WT_ERR(__backup_list_append(session, cb, logfiles[i]));
    }
err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    return (ret);
}

/*
 * __backup_start --
 *     Start a backup.
 */
static int
__backup_start(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, bool is_dup, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FSTREAM *srcfs;
    const char *dest;
    bool exist, log_only, target_list;

    conn = S2C(session);
    srcfs = NULL;
    dest = NULL;

    cb->next = 0;
    cb->list = NULL;
    cb->list_next = 0;

    WT_RET(__wt_inmem_unsupported_op(session, "backup cursor"));

    /*
     * Single thread hot backups: we're holding the schema lock, so we know we'll serialize with
     * other attempts to start a hot backup.
     */
    if (conn->hot_backup_start != 0 && !is_dup)
        WT_RET_MSG(session, EINVAL, "there is already a backup cursor open");

    if (F_ISSET(session, WT_SESSION_BACKUP_DUP) && is_dup)
        WT_RET_MSG(session, EINVAL, "there is already a duplicate backup cursor open");

    if (!is_dup) {
        /*
         * The hot backup copy is done outside of WiredTiger, which
         * means file blocks can't be freed and re-allocated until the
         * backup completes. The checkpoint code checks the backup flag,
         * and if a backup cursor is open checkpoints aren't discarded.
         * We release the lock as soon as we've set the flag, we don't
         * want to block checkpoints, we just want to make sure no
         * checkpoints are deleted.  The checkpoint code holds the lock
         * until it's finished the checkpoint, otherwise we could start
         * a hot backup that would race with an already-started
         * checkpoint.
         *
         * We are holding the checkpoint and schema locks so schema
         * operations will not see the backup file list until it is
         * complete and valid.
         */
        __wt_writelock(session, &conn->hot_backup_lock);
        conn->hot_backup_start = conn->ckpt_most_recent;
        conn->hot_backup_list = NULL;
        __wt_writeunlock(session, &conn->hot_backup_lock);

        /* We're the lock holder, we own cleanup. */
        F_SET(cb, WT_CURBACKUP_LOCKER);

        /*
         * Create a temporary backup file. This must be opened before generating the list of targets
         * in backup_uri. This file will later be renamed to the correct name depending on whether
         * or not we're doing an incremental backup. We need a temp file so that if we fail or crash
         * while filling it, the existence of a partial file doesn't confuse restarting in the
         * source database.
         */
        WT_ERR(__wt_fopen(session, WT_BACKUP_TMP, WT_FS_OPEN_CREATE, WT_STREAM_WRITE, &cb->bfs));
    }

    /*
     * If targets were specified, add them to the list. Otherwise it is a full backup, add all
     * database objects and log files to the list.
     */
    target_list = false;
    WT_ERR(__backup_uri(session, cfg, is_dup, &target_list, &log_only));
    /*
     * For a duplicate cursor, all the work is done in backup_uri. The only usage accepted is
     * "target=("log:")" so error if not log only.
     */
    if (is_dup) {
        if (!log_only)
            WT_ERR_MSG(session, EINVAL, "duplicate backup cursor must be for logs only.");
        F_SET(cb, WT_CURBACKUP_DUP);
        F_SET(session, WT_SESSION_BACKUP_DUP);
        goto done;
    }
    if (!target_list) {
        /*
         * It's important to first gather the log files to be copied
         * (which internally starts a new log file), followed by
         * choosing a checkpoint to reference in the WiredTiger.backup
         * file.
         *
         * Applications may have logic that takes a checkpoint, followed
         * by performing a write that should only appear in the new
         * checkpoint. This ordering prevents choosing the prior
         * checkpoint, but including the write in the log files
         * returned.
         *
         * It is also possible, and considered legal, to choose the new
         * checkpoint, but not include the log file that contains the
         * log entry for taking the new checkpoint.
         */
        WT_ERR(__backup_log_append(session, cb, true));
        WT_ERR(__backup_all(session));
    }

    /* Add the hot backup and standard WiredTiger files to the list. */
    if (log_only) {
        /*
         * If this is not a duplicate cursor, using the log target is an
         * incremental backup. If this is a duplicate cursor then using
         * the log target on an existing backup cursor means this cursor
         * returns the current list of log files. That list was set up
         * when parsing the URI so we don't have anything to do here.
         *
         * We also open an incremental backup source file so that we can
         * detect a crash with an incremental backup existing in the
         * source directory versus an improper destination.
         */
        dest = WT_INCREMENTAL_BACKUP;
        WT_ERR(__wt_fopen(session, WT_INCREMENTAL_SRC, WT_FS_OPEN_CREATE, WT_STREAM_WRITE, &srcfs));
        WT_ERR(__backup_list_append(session, cb, WT_INCREMENTAL_BACKUP));
    } else {
        dest = WT_METADATA_BACKUP;
        WT_ERR(__backup_list_append(session, cb, WT_METADATA_BACKUP));
        WT_ERR(__wt_fs_exist(session, WT_BASECONFIG, &exist));
        if (exist)
            WT_ERR(__backup_list_append(session, cb, WT_BASECONFIG));
        WT_ERR(__wt_fs_exist(session, WT_USERCONFIG, &exist));
        if (exist)
            WT_ERR(__backup_list_append(session, cb, WT_USERCONFIG));
        WT_ERR(__backup_list_append(session, cb, WT_WIREDTIGER));
    }

err:
    /* Close the hot backup file. */
    if (srcfs != NULL)
        WT_TRET(__wt_fclose(session, &srcfs));
    /*
     * Sync and rename the temp file into place.
     */
    if (ret == 0)
        ret = __wt_sync_and_rename(session, &cb->bfs, WT_BACKUP_TMP, dest);
    if (ret == 0) {
        __wt_writelock(session, &conn->hot_backup_lock);
        conn->hot_backup_list = cb->list;
        __wt_writeunlock(session, &conn->hot_backup_lock);
        F_SET(session, WT_SESSION_BACKUP_CURSOR);
    }
    /*
     * If the file hasn't been closed, do it now.
     */
    if (cb->bfs != NULL)
        WT_TRET(__wt_fclose(session, &cb->bfs));

done:
    return (ret);
}

/*
 * __backup_stop --
 *     Stop a backup.
 */
static int
__backup_stop(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Release all btree names held by the backup. */
    WT_ASSERT(session, !F_ISSET(cb, WT_CURBACKUP_DUP));
    /* If it's not a dup backup cursor, make sure one isn't open. */
    WT_ASSERT(session, !F_ISSET(session, WT_SESSION_BACKUP_DUP));
    __wt_writelock(session, &conn->hot_backup_lock);
    conn->hot_backup_list = NULL;
    __wt_writeunlock(session, &conn->hot_backup_lock);
    __backup_free(session, cb);

    /* Remove any backup specific file. */
    WT_TRET(__wt_backup_file_remove(session));

    /* Checkpoint deletion and next hot backup can proceed. */
    __wt_writelock(session, &conn->hot_backup_lock);
    conn->hot_backup_start = 0;
    __wt_writeunlock(session, &conn->hot_backup_lock);
    F_CLR(session, WT_SESSION_BACKUP_CURSOR);

    return (ret);
}

/*
 * __backup_all --
 *     Backup all objects in the database.
 */
static int
__backup_all(WT_SESSION_IMPL *session)
{
    /* Build a list of the file objects that need to be copied. */
    return (__wt_meta_apply_all(session, NULL, __backup_list_uri_append, NULL));
}

/*
 * __backup_uri --
 *     Backup a list of objects.
 */
static int
__backup_uri(WT_SESSION_IMPL *session, const char *cfg[], bool is_dup, bool *foundp, bool *log_only)
{
    WT_CONFIG targetconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    const char *uri;
    bool target_list;

    *foundp = *log_only = false;

    /*
     * If we find a non-empty target configuration string, we have a job, otherwise it's not our
     * problem.
     */
    WT_RET(__wt_config_gets(session, cfg, "target", &cval));
    __wt_config_subinit(session, &targetconf, &cval);
    for (target_list = false; (ret = __wt_config_next(&targetconf, &k, &v)) == 0;
         target_list = true) {
        /* If it is our first time through, allocate. */
        if (!target_list) {
            *foundp = true;
            WT_ERR(__wt_scr_alloc(session, 512, &tmp));
        }

        WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)k.len, k.str));
        uri = tmp->data;
        if (v.len != 0)
            WT_ERR_MSG(session, EINVAL, "%s: invalid backup target: URIs may need quoting", uri);

        /*
         * Handle log targets. We do not need to go through the schema worker, just call the
         * function to append them. Set log_only only if it is our only URI target.
         */
        if (WT_PREFIX_MATCH(uri, "log:")) {
            /*
             * Log archive cannot mix with incremental backup, don't let that happen. If we're a
             * duplicate cursor archiving is already temporarily suspended.
             */
            if (!is_dup && FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ARCHIVE))
                WT_ERR_MSG(session, EINVAL,
                  "incremental backup not possible when "
                  "automatic log archival configured");
            *log_only = !target_list;
            WT_ERR(__backup_log_append(session, session->bkp_cursor, false));
        } else {
            *log_only = false;

            /*
             * If backing up individual tables, we have to include indexes, which may involve
             * opening those indexes. Acquire the table lock in write mode for that case.
             */
            WT_WITH_TABLE_WRITE_LOCK(session,
              ret = __wt_schema_worker(session, uri, NULL, __backup_list_uri_append, cfg, 0));
            WT_ERR(ret);
        }
    }
    WT_ERR_NOTFOUND_OK(ret);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_backup_file_remove --
 *     Remove the incremental and meta-data backup files.
 */
int
__wt_backup_file_remove(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    /*
     * Note that order matters for removing the incremental files. We must remove the backup file
     * before removing the source file so that we always know we were a source directory while
     * there's any chance of an incremental backup file existing.
     */
    WT_TRET(__wt_remove_if_exists(session, WT_BACKUP_TMP, true));
    WT_TRET(__wt_remove_if_exists(session, WT_INCREMENTAL_BACKUP, true));
    WT_TRET(__wt_remove_if_exists(session, WT_INCREMENTAL_SRC, true));
    WT_TRET(__wt_remove_if_exists(session, WT_METADATA_BACKUP, true));
    return (ret);
}

/*
 * __backup_list_uri_append --
 *     Append a new file name to the list, allocate space as necessary. Called via the schema_worker
 *     function.
 */
static int
__backup_list_uri_append(WT_SESSION_IMPL *session, const char *name, bool *skip)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    char *value;

    cb = session->bkp_cursor;
    WT_UNUSED(skip);

    /*
     * While reading the metadata file, check there are no data sources that can't support hot
     * backup. This checks for a data source that's non-standard, which can't be backed up, but is
     * also sanity checking: if there's an entry backed by anything other than a file or lsm entry,
     * we're confused.
     */
    if (!WT_PREFIX_MATCH(name, "file:") && !WT_PREFIX_MATCH(name, "colgroup:") &&
      !WT_PREFIX_MATCH(name, "index:") && !WT_PREFIX_MATCH(name, "lsm:") &&
      !WT_PREFIX_MATCH(name, WT_SYSTEM_PREFIX) && !WT_PREFIX_MATCH(name, "table:"))
        WT_RET_MSG(session, ENOTSUP, "hot backup is not supported for objects of type %s", name);

    /* Ignore the lookaside table or system info. */
    if (strcmp(name, WT_LAS_URI) == 0)
        return (0);

    /* Add the metadata entry to the backup file. */
    WT_RET(__wt_metadata_search(session, name, &value));
    ret = __wt_fprintf(session, cb->bfs, "%s\n%s\n", name, value);
    __wt_free(session, value);
    WT_RET(ret);

    /*
     * We want to retain the system information in the backup metadata file above, but there is no
     * file object to copy so return now.
     */
    if (WT_PREFIX_MATCH(name, WT_SYSTEM_PREFIX))
        return (0);

    /* Add file type objects to the list of files to be copied. */
    if (WT_PREFIX_MATCH(name, "file:"))
        WT_RET(__backup_list_append(session, cb, name));

    return (0);
}

/*
 * __backup_list_append --
 *     Append a new file name to the list, allocate space as necessary.
 */
static int
__backup_list_append(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, const char *uri)
{
    char **p;
    const char *name;

    /* Leave a NULL at the end to mark the end of the list. */
    WT_RET(__wt_realloc_def(session, &cb->list_allocated, cb->list_next + 2, &cb->list));
    p = &cb->list[cb->list_next];
    p[0] = p[1] = NULL;

    name = uri;

    /*
     * If it's a file in the database we need to remove the prefix.
     */
    if (WT_PREFIX_MATCH(uri, "file:"))
        name += strlen("file:");

    /*
     * !!!
     * Assumes metadata file entries map one-to-one to physical files.
     * To support a block manager where that's not the case, we'd need
     * to call into the block manager and get a list of physical files
     * that map to this logical "file".  I'm not going to worry about
     * that for now, that block manager might not even support physical
     * copying of files by applications.
     */
    WT_RET(__wt_strdup(session, name, p));

    ++cb->list_next;
    return (0);
}
