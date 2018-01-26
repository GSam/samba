/*
   ldb database library using mdb back end

   Copyright (C) Jakub Hrozek 2014
   Copyright (C) Catalyst.Net Ltd 2017

     ** NOTE! The following LGPL license applies to the ldb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "lmdb.h"
#include "ldb_mdb.h"
#include "../ldb_tdb/ldb_tdb.h"
#include "include/dlinklist.h"

#define MDB_URL_PREFIX		"mdb://"
#define MDB_URL_PREFIX_SIZE	(sizeof(MDB_URL_PREFIX)-1)

#define MEGABYTE (1024*1024)

int ldb_mdb_err_map(int lmdb_err)
{
	switch (lmdb_err) {
	case MDB_SUCCESS:
		return LDB_SUCCESS;
	case MDB_INCOMPATIBLE:
	case MDB_CORRUPTED:
	case EIO:
		return LDB_ERR_OPERATIONS_ERROR;
	case MDB_INVALID:
		return LDB_ERR_UNAVAILABLE;
	case MDB_BAD_TXN:
	case MDB_BAD_VALSIZE:
#ifdef MDB_BAD_DBI
	case MDB_BAD_DBI:
#endif
	case MDB_PANIC:
	case EINVAL:
		return LDB_ERR_PROTOCOL_ERROR;
	case MDB_MAP_FULL:
	case MDB_DBS_FULL:
	case MDB_READERS_FULL:
	case MDB_TLS_FULL:
	case MDB_TXN_FULL:
	case EAGAIN:
		return LDB_ERR_BUSY;
	case MDB_KEYEXIST:
		return LDB_ERR_ENTRY_ALREADY_EXISTS;
	case MDB_NOTFOUND:
	case ENOENT:
		return LDB_ERR_NO_SUCH_OBJECT;
	case EACCES:
		return LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS;
	default:
		break;
	}
	return LDB_ERR_OTHER;
}

static MDB_txn *lmdb_trans_get_tx(struct lmdb_trans *ltx)
{
	if (ltx == NULL) {
		return NULL;
	}

	return ltx->tx;
}

static void trans_push(struct lmdb_private *lmdb, struct lmdb_trans *ltx)
{
	if (lmdb->txlist) {
		talloc_steal(lmdb->txlist, ltx);
	}

	DLIST_ADD(lmdb->txlist, ltx);
}

static void trans_finished(struct lmdb_private *lmdb, struct lmdb_trans *ltx)
{
	ltx->tx = NULL; /* Neutralize destructor */
	DLIST_REMOVE(lmdb->txlist, ltx);
	talloc_free(ltx);
}

/*
static int ldb_mdb_trans_destructor(struct lmdb_trans *ltx)
{
	if (ltx != NULL && ltx->tx != NULL) {
		mdb_txn_abort(ltx->tx);
		ltx->tx = NULL;
	}
	return 0;
}
*/

static struct lmdb_trans *lmdb_private_trans_head(struct lmdb_private *lmdb)
{
	struct lmdb_trans *ltx;

	ltx = lmdb->txlist;
	return ltx;
}

static MDB_txn *get_current_txn(struct lmdb_private *lmdb)
{
	MDB_txn *txn;
	if (lmdb->read_txn != NULL) {
		return lmdb->read_txn;
	}

	txn = lmdb_trans_get_tx(lmdb_private_trans_head(lmdb));
	if (txn == NULL) {
		/* TODO ret? */
		int ret;
		ret = mdb_txn_begin(lmdb->env, NULL, MDB_RDONLY, &txn);
		if (ret != 0) {
			ldb_asprintf_errstring(lmdb->ldb,
					       "%s failed: %s\n", __FUNCTION__,
					       mdb_strerror(ret));
		}
		lmdb->read_txn = txn;
	}
	return txn;
}

static int lmdb_store(struct ltdb_private *ltdb,
		      TDB_DATA key,
		      TDB_DATA data, int flags)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	MDB_val mdb_key;
	MDB_val mdb_data;
	int mdb_flags;
	MDB_txn *txn = lmdb_trans_get_tx(lmdb_private_trans_head(lmdb));
	MDB_dbi dbi = 0;

	lmdb->error = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (lmdb->error != 0) {
		return lmdb->error;
	}

	mdb_key.mv_size = key.dsize;
	mdb_key.mv_data = key.dptr;

	mdb_data.mv_size = data.dsize;
	mdb_data.mv_data = data.dptr;

	if (flags == TDB_INSERT) {
		mdb_flags = MDB_NOOVERWRITE;
	} else {
		/* TODO TDB_MODIFY does an exists check */
		mdb_flags = 0;
	}

        lmdb->error = mdb_put(txn, dbi, &mdb_key, &mdb_data, mdb_flags);

	/* TODO set error */
	if (lmdb->error != 0) {
		ldb_asprintf_errstring(lmdb->ldb,
				       "%s failed: %s\n", __FUNCTION__,
				       mdb_strerror(lmdb->error));
	}

	return lmdb->error;
}

static int lmdb_exists(struct ltdb_private *ltdb, TDB_DATA key)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	MDB_val mdb_key;
	MDB_val mdb_data;
	MDB_txn *txn = get_current_txn(lmdb);
	MDB_dbi dbi = 0;

	lmdb->error = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (lmdb->error != 0) {
		return lmdb->error;
	}

	mdb_key.mv_size = key.dsize;
	mdb_key.mv_data = key.dptr;

        lmdb->error = mdb_get(txn, dbi, &mdb_key, &mdb_data);
	if (ltdb->read_lock_count == 0 && lmdb->read_txn != NULL) {
		mdb_txn_commit(lmdb->read_txn);
		lmdb->read_txn = NULL;
	}
	if (lmdb->error != 0) {
		return 0;
	}
	return 1;
}

static int lmdb_delete(struct ltdb_private *ltdb, TDB_DATA key)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	MDB_val mdb_key;
	//MDB_val mdb_data;
	MDB_txn *txn = lmdb_trans_get_tx(lmdb_private_trans_head(lmdb));
	MDB_dbi dbi = 0;

	lmdb->error = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (lmdb->error != 0) {
		return lmdb->error;
	}

	mdb_key.mv_size = key.dsize;
	mdb_key.mv_data = key.dptr;

        lmdb->error = mdb_del(txn, dbi, &mdb_key, NULL);

	/* TODO store error */
	if (lmdb->error != 0) {
		ldb_asprintf_errstring(lmdb->ldb,
				       "%s failed: %s\n", __FUNCTION__,
				       mdb_strerror(lmdb->error));
	}
	return lmdb->error;
}

static TDB_DATA lmdb_fetch(struct ltdb_private *ltdb, TDB_DATA key)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	MDB_val mdb_key;
	MDB_val mdb_data;
	MDB_txn *txn = get_current_txn(lmdb);
	MDB_dbi dbi = 0;

	lmdb->error = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (lmdb->error != 0) {
		return tdb_null;
	}

	mdb_key.mv_size = key.dsize;
	mdb_key.mv_data = key.dptr;

        lmdb->error = mdb_get(txn, dbi, &mdb_key, &mdb_data);


	if (lmdb->error != 0) {
		ldb_asprintf_errstring(lmdb->ldb,
				       "%s failed: %s\n", __FUNCTION__,
				       mdb_strerror(lmdb->error));
		/* We created a read transaction, commit it */
		if (ltdb->read_lock_count == 0 && lmdb->read_txn != NULL) {
			mdb_txn_commit(lmdb->read_txn);
			lmdb->read_txn = NULL;
		}
		return tdb_null;
	} else {
		TDB_DATA result;
		result.dsize = mdb_data.mv_size;
		/* TODO must talloc_memdup? */
		result.dptr = talloc_memdup(ltdb, mdb_data.mv_data, mdb_data.mv_size);

		/* We created a read transaction, commit it */
		if (ltdb->read_lock_count == 0 && lmdb->read_txn != NULL) {
			mdb_txn_commit(lmdb->read_txn);
			lmdb->read_txn = NULL;
		}
		return result;
	}
}

static int lmdb_traverse_fn(struct ltdb_private *ltdb,
		            ldb_kv_traverse_fn fn,
			    void *ctx)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	MDB_val mdb_key;
	MDB_val mdb_data;
	/* TODO ? */
	MDB_txn *txn = get_current_txn(lmdb);
	MDB_dbi dbi = 0;
	MDB_cursor *cursor = NULL;
	int ret;

	lmdb->error = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (lmdb->error != 0) {
		return lmdb->error;
	}

	ret = mdb_cursor_open(txn, dbi, &cursor);
	if (ret != 0) {
		ldb_asprintf_errstring(ltdb->lmdb_private->ldb,
				       "mdb_cursor_open failed: %s\n",
				       mdb_strerror(ret));
		ret = ldb_mdb_err_map(ret);
		goto done;
	}

	while ((ret = mdb_cursor_get(cursor, &mdb_key,
				     &mdb_data, MDB_NEXT)) == 0) {

		struct ldb_val key = {
			.length = mdb_key.mv_size,
			.data = mdb_key.mv_data,
		};
		struct ldb_val data = {
			.length = mdb_data.mv_size,
			.data = mdb_data.mv_data,
		};

		ret = fn(ltdb, &key, &data, ctx);
		if (ret != 0) {
			goto done;
		}
	}
	if (ret != MDB_NOTFOUND) {
		ret = ldb_mdb_err_map(ret);
		if (ret != 0) {
			ldb_asprintf_errstring(lmdb->ldb,
					       "%s failed: %s\n", __FUNCTION__,
					       mdb_strerror(ret));
		}
		/* TODO store error */
		goto done;
	}
done:
	if (cursor != NULL) {
		mdb_cursor_close(cursor);
	}

	if (ltdb->read_lock_count == 0 && lmdb->read_txn != NULL) {
		mdb_txn_commit(lmdb->read_txn);
		lmdb->read_txn = NULL;
	}

	return LDB_SUCCESS;
}

static int lmdb_update_in_iterate(struct ltdb_private *ltdb,
				  TDB_DATA key,
				  TDB_DATA key2,
				  TDB_DATA data,
				  void *state)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	struct TDB_DATA copy;

	/*
	 * Need to take a copy of the data as the delete operation alters the
	 * data, as it is in private lmdb memory.
	 */
	copy.dsize = data.dsize;
	copy.dptr = talloc_memdup(ltdb, data.dptr, data.dsize);
	if (copy.dptr == NULL) {
		ldb_debug(lmdb->ldb, LDB_DEBUG_ERROR,
			  "Unable to allocate memory for copy of data %*.*s "
			  "for rekey as %*.*s",
			  (int)key.dsize, (int)key.dsize,
			  (const char *)key.dptr,
			  (int)key2.dsize, (int)key2.dsize,
			  (const char *)key.dptr);
		// TODO store the error
		goto done;
	}

	lmdb->error = lmdb_delete(ltdb, key);
	if (lmdb->error != 0) {
		ldb_debug(lmdb->ldb, LDB_DEBUG_ERROR,
			  "Failed to delete %*.*s "
			  "for rekey as %*.*s: %s",
			  (int)key.dsize, (int)key.dsize,
			  (const char *)key.dptr,
			  (int)key2.dsize, (int)key2.dsize,
			  (const char *)key.dptr,
			  mdb_strerror(lmdb->error));
		// TODO store the error
		goto done;
	}
	lmdb->error = lmdb_store(ltdb, key2, copy, 0);
	if (lmdb->error != 0) {
		ldb_debug(lmdb->ldb, LDB_DEBUG_ERROR,
			  "Failed to rekey %*.*s as %*.*s: %s",
			  (int)key.dsize, (int)key.dsize,
			  (const char *)key.dptr,
			  (int)key2.dsize, (int)key2.dsize,
			  (const char *)key.dptr,
			  mdb_strerror(lmdb->error));
		// TODO Store the error
		goto done;
	}

done:
	if (copy.dptr != NULL) {
		TALLOC_FREE(copy.dptr);
		copy.dsize = 0;
	}

	/*
	 * Explicity invalidate the data, as the delete has done this
	 */
	data.dsize = 0;
	data.dptr = NULL;
	if (lmdb->error != 0) {
		return -1;
	} else {
		return 0;
	}
}
/* Handles only a single record */
static int lmdb_parse_record(struct ltdb_private *ltdb, TDB_DATA key,
			     int (*parser)(TDB_DATA key, TDB_DATA data,
			     void *private_data),
			     void *ctx)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	MDB_val mdb_key;
	MDB_val mdb_data;
	MDB_txn *txn = get_current_txn(lmdb);
	MDB_dbi dbi;
	TDB_DATA data;

	lmdb->error = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (lmdb->error != 0) {
		return lmdb->error;
	}

	mdb_key.mv_size = key.dsize;
	mdb_key.mv_data = key.dptr;

	/* TODO memdup ? */
        lmdb->error = mdb_get(txn, dbi, &mdb_key, &mdb_data);
	data.dptr = mdb_data.mv_data;
	data.dsize = mdb_data.mv_size;

	/* TODO closing a handle should not even be necessary */
	mdb_dbi_close(lmdb->env, dbi);

	/* We created a read transaction, commit it */
	if (ltdb->read_lock_count == 0 && lmdb->read_txn != NULL) {
		mdb_txn_commit(lmdb->read_txn);
		lmdb->read_txn = NULL;
	}

	if (lmdb->error != 0) {
		if (lmdb->error != 0) {
			ldb_asprintf_errstring(lmdb->ldb,
					       "%s failed: %s\n", __FUNCTION__,
					       mdb_strerror(lmdb->error));
		}
		return ldb_mdb_err_map(lmdb->error);
	}

	return parser(key, data, ctx);
}


static int lmdb_lock_read(struct ldb_module *module)
{
	void *data = ldb_module_get_private(module);
	struct ltdb_private *ltdb = talloc_get_type(data, struct ltdb_private);
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	int ret = 0;

	if (ltdb->in_transaction == 0 &&
	    ltdb->read_lock_count == 0) {
		ret = mdb_txn_begin(lmdb->env, NULL, MDB_RDONLY, &lmdb->read_txn);
	}
	if (ret == 0) {
		ltdb->read_lock_count++;
	} else {
		ldb_asprintf_errstring(lmdb->ldb,
				       "mdb_txn_begin failed: %s\n",
				       mdb_strerror(ret));
		return ldb_mdb_err_map(ret);
	}

	lmdb->error = ret;
	return ret;
}

static int lmdb_unlock_read(struct ldb_module *module)
{
	void *data = ldb_module_get_private(module);
	struct ltdb_private *ltdb = talloc_get_type(data, struct ltdb_private);
	if (ltdb->in_transaction == 0 && ltdb->read_lock_count == 1) {
		struct lmdb_private *lmdb = ltdb->lmdb_private;
		mdb_txn_commit(lmdb->read_txn);
		lmdb->read_txn = NULL;
		ltdb->read_lock_count--;
		return 0;
	}
	ltdb->read_lock_count--;
	return 0;
}

static int lmdb_transaction_start(struct ltdb_private *ltdb)
{
	struct lmdb_private *lmdb = ltdb->lmdb_private;
	struct lmdb_trans *ltx;
	struct lmdb_trans *ltx_head;
	MDB_txn *tx_parent;

	ltx = talloc_zero(lmdb, struct lmdb_trans);
	if (ltx == NULL) {
		return ldb_oom(lmdb->ldb);
	}

	// talloc_set_destructor(ltx, ldb_mdb_trans_destructor);
	/*ltx->db_op = talloc_zero(ltx, struct lmdb_db_op);
	if (ltx->db_op  == NULL) {
		talloc_free(ltx);
		return ldb_oom(lmdb->ldb);
	}*/

	//ltx->lmdb = lmdb;
	/*ltx->db_op->mdb_dbi = 0;
	ltx->db_op->ltx = ltx;*/

	ltx_head = lmdb_private_trans_head(lmdb);

	tx_parent = lmdb_trans_get_tx(ltx_head);

	lmdb->error = mdb_txn_begin(lmdb->env, tx_parent, 0, &ltx->tx);

	trans_push(lmdb, ltx);

	return lmdb->error;
}

static int lmdb_transaction_cancel(struct ltdb_private *ltdb)
{
	struct lmdb_trans *ltx;
	struct lmdb_private *lmdb = ltdb->lmdb_private;

	ltx = lmdb_private_trans_head(lmdb);
	if (ltx == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	mdb_txn_abort(ltx->tx);
	trans_finished(lmdb, ltx);
	return LDB_SUCCESS;
}

static int lmdb_transaction_prepare_commit(struct ltdb_private *ltdb)
{
	/* No need to prepare a commit */
	return LDB_SUCCESS;
}

static int lmdb_transaction_commit(struct ltdb_private *ltdb)
{
	struct lmdb_trans *ltx;
	struct lmdb_private *lmdb = ltdb->lmdb_private;

	ltx = lmdb_private_trans_head(lmdb);
	if (ltx == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	lmdb->error = mdb_txn_commit(ltx->tx);
	trans_finished(lmdb, ltx);

	return lmdb->error;
}

static int lmdb_error(struct ltdb_private *ltdb)
{
	return ldb_mdb_err_map(ltdb->lmdb_private->error);
}

static const char * lmdb_name(struct ltdb_private *ltdb)
{
	return "lmdb";
}

static bool lmdb_changed(struct ltdb_private *ltdb)
{
	return true;
}


static struct kv_db_ops lmdb_key_value_ops = {
	.store             = lmdb_store,
	.delete            = lmdb_delete,
	.exists            = lmdb_exists,
	.iterate_write     = lmdb_traverse_fn,
	.update_in_iterate = lmdb_update_in_iterate,
	.fetch             = lmdb_fetch,
	.fetch_and_parse   = lmdb_parse_record,
	.lock_read         = lmdb_lock_read,
	.unlock_read       = lmdb_unlock_read,
	.begin_write       = lmdb_transaction_start,
	.prepare_write     = lmdb_transaction_prepare_commit,
	.finish_write      = lmdb_transaction_commit,
	.abort_write       = lmdb_transaction_cancel,
	.error             = lmdb_error,
	.name              = lmdb_name,
	.has_changed       = lmdb_changed,
};

static const char *lmdb_get_path(const char *url)
{
	const char *path;

	/* parse the url */
	if (strchr(url, ':')) {
		if (strncmp(url, MDB_URL_PREFIX, MDB_URL_PREFIX_SIZE) != 0) {
			return NULL;
		}
		path = url + MDB_URL_PREFIX_SIZE;
	} else {
		path = url;
	}

	return path;
}

static int lmdb_pvt_destructor(struct lmdb_private *lmdb)
{
	struct lmdb_trans *ltx = NULL;

	/*
	 * Close the read transaction if it's open
	 */
	if (lmdb->read_txn != NULL) {
		mdb_txn_abort(lmdb->read_txn);
	}

	if (lmdb->env == NULL) {
		return 0;
	}

	/*
	 * Abort any currently active transactions
	 */
	ltx = lmdb_private_trans_head(lmdb);
	while (ltx != NULL) {
		mdb_txn_abort(ltx->tx);
		trans_finished(lmdb, ltx);
		ltx = lmdb_private_trans_head(lmdb);
	}

	mdb_env_close(lmdb->env);
	lmdb->env = NULL;

	return 0;
}

static int lmdb_pvt_open(TALLOC_CTX *mem_ctx,
				  struct ldb_context *ldb,
				  const char *path,
				  unsigned int flags,
				  struct lmdb_private *lmdb)
{
	int ret;
	unsigned int mdb_flags;

	if (flags & LDB_FLG_DONT_CREATE_DB) {
		struct stat st;
		if (stat(path, &st) != 0) {
			return LDB_ERR_UNAVAILABLE;
		}
	}

	ret = mdb_env_create(&lmdb->env);
	if (ret != 0) {
		ldb_asprintf_errstring(
			ldb,
			"Could not create MDB environment %s: %s\n",
			path,
			mdb_strerror(ret));
		return LDB_ERR_OPERATIONS_ERROR;
	}

	/* Close when lmdb is released */
	talloc_set_destructor(lmdb, lmdb_pvt_destructor);

	ret = mdb_env_set_mapsize(lmdb->env, 100 * MEGABYTE);
	if (ret != 0) {
		ldb_asprintf_errstring(
			ldb,
			"Could not open MDB environment %s: %s\n",
			path,
			mdb_strerror(ret));
		return ldb_mdb_err_map(ret);
	}

	mdb_env_set_maxreaders(lmdb->env, 100000);
	/* MDB_NOSUBDIR implies there is a separate file called path and a
	 * separate lockfile called path-lock
	 */
	mdb_flags = MDB_NOSUBDIR|MDB_NOTLS;
	if (flags & LDB_FLG_RDONLY) {
		mdb_flags |= MDB_RDONLY;
	}
	ret = mdb_env_open(lmdb->env, path, mdb_flags, 0644);
	if (ret != 0) {
		ldb_asprintf_errstring(ldb,
				"Could not open DB %s: %s\n",
				path, mdb_strerror(ret));
		talloc_free(lmdb);
		return ldb_mdb_err_map(ret);
	}

	return LDB_SUCCESS;

}

int lmdb_connect(struct ldb_context *ldb,
		 const char *url,
		 unsigned int flags,
		 const char *options[],
		 struct ldb_module **_module)
{
	const char *path = NULL;
	struct lmdb_private *lmdb = NULL;
	struct ltdb_private *ltdb = NULL;
	int ret;

	path = lmdb_get_path(url);
	if (path == NULL) {
		ldb_debug(ldb, LDB_DEBUG_ERROR, "Invalid mdb URL '%s'", url);
		return LDB_ERR_OPERATIONS_ERROR;
	}

        ltdb = talloc_zero(ldb, struct ltdb_private);
        if (!ltdb) {
                ldb_oom(ldb);
                return LDB_ERR_OPERATIONS_ERROR;
        }

	lmdb = talloc_zero(ldb, struct lmdb_private);
	if (lmdb == NULL) {
		TALLOC_FREE(ltdb);
                ldb_oom(ldb);
                return LDB_ERR_OPERATIONS_ERROR;
	}
	lmdb->ldb = ldb;
	ltdb->kv_ops = &lmdb_key_value_ops;

	ret = lmdb_pvt_open(ldb, ldb, path, flags, lmdb);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ltdb->lmdb_private = lmdb;
	if (flags & LDB_FLG_RDONLY) {
		ltdb->read_only = true;
	}
        return init_store(ltdb, "ldb_mdb backend", ldb, options, _module);
}

