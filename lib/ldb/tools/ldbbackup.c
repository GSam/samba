/*
   Unix SMB/CIFS implementation.
   low level tdb backup and restore utility
   Copyright (C) Andrew Tridgell              2002

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*

  This program is meant for backup/restore of mixed tdb/mdb databases.
  Typical usage would be:
     ldbbackup *.tdb
  when Samba shuts down cleanly, which will make a backup of all the local
  databases to .bak files. Then on Samba startup you would use:
     ldbbackup -v *.tdb
  and this will check the databases for corruption and if corruption is detected
  then the backup will be restored.

  You may also like to do a backup on a regular basis while Samba is
  running, perhaps using cron.

  The reason this program is needed is to cope with power failures
  while Samba is running. A power failure could lead to database
  corruption and Samba will then not start correctly.

  Note that many of the databases in Samba are transient and thus
  don't need to be backed up, so you can optimise the above a little
  by only running the backup on the critical databases.

 */

#include "replace.h"
#include "system/locale.h"
#include "system/time.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "tdb.h"
#include "ldb.h"
#include "ldb_private.h"

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef HAVE_LMDB
#include "lmdb.h"
#endif

static int failed;

static struct tdb_logging_context log_ctx;

#ifdef PRINTF_ATTRIBUTE
static void tdb_log(struct tdb_context *tdb,
		    enum tdb_debug_level level,
		    const char *format, ...) PRINTF_ATTRIBUTE(3,4);
#endif
static void tdb_log(struct tdb_context *tdb,
		    enum tdb_debug_level level,
		    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stdout, format, ap);
	va_end(ap);
	fflush(stdout);
}

static char *add_suffix(const char *name, const char *suffix)
{
	char *ret;
	int len = strlen(name) + strlen(suffix) + 1;
	ret = (char *)malloc(len);
	if (!ret) {
		fprintf(stderr,"Out of memory!\n");
		exit(1);
	}
	snprintf(ret, len, "%s%s", name, suffix);
	return ret;
}

static int copy_fn(TDB_CONTEXT *tdb, TDB_DATA key, TDB_DATA dbuf, void *state)
{
	TDB_CONTEXT *tdb_new = (TDB_CONTEXT *)state;

	if (tdb_store(tdb_new, key, dbuf, TDB_INSERT) != 0) {
		fprintf(stderr,"Failed to insert into %s\n", tdb_name(tdb_new));
		failed = 1;
		return 1;
	}
	return 0;
}


static int test_fn(TDB_CONTEXT *tdb, TDB_DATA key, TDB_DATA dbuf, void *state)
{
	return 0;
}

/*
  carefully backup a tdb, validating the contents and
  only doing the backup if its OK
  this function is also used for restore
*/
static int backup_tdb(const char *old_name, const char *new_name,
		      int hash_size, int nolock)
{
	TDB_CONTEXT *tdb;
	TDB_CONTEXT *tdb_new;
	char *tmp_name;
	struct stat st;
	int count1, count2;

	tmp_name = add_suffix(new_name, ".tmp");

	/* stat the old tdb to find its permissions */
	if (stat(old_name, &st) != 0) {
		perror(old_name);
		free(tmp_name);
		return 1;
	}

	/* open the old tdb */
	tdb = tdb_open_ex(old_name, 0,
			  TDB_DEFAULT | (nolock ? TDB_NOLOCK : 0),
			  O_RDWR, 0, &log_ctx, NULL);
	if (!tdb) {
		printf("Failed to open %s\n", old_name);
		free(tmp_name);
		return 1;
	}

	/* create the new tdb */
	unlink(tmp_name);
	tdb_new = tdb_open_ex(tmp_name,
			      hash_size ? hash_size : tdb_hash_size(tdb),
			      TDB_DEFAULT,
			      O_RDWR|O_CREAT|O_EXCL, st.st_mode & 0777,
			      &log_ctx, NULL);
	if (!tdb_new) {
		perror(tmp_name);
		free(tmp_name);
		return 1;
	}

	if (tdb_transaction_start(tdb) != 0) {
		printf("Failed to start transaction on old tdb\n");
		tdb_close(tdb);
		tdb_close(tdb_new);
		unlink(tmp_name);
		free(tmp_name);
		return 1;
	}

	/* lock the backup tdb so that nobody else can change it */
	if (tdb_lockall(tdb_new) != 0) {
		printf("Failed to lock backup tdb\n");
		tdb_close(tdb);
		tdb_close(tdb_new);
		unlink(tmp_name);
		free(tmp_name);
		return 1;
	}

	failed = 0;

	/* traverse and copy */
	count1 = tdb_traverse(tdb, copy_fn, (void *)tdb_new);
	if (count1 < 0 || failed) {
		fprintf(stderr,"failed to copy %s\n", old_name);
		tdb_close(tdb);
		tdb_close(tdb_new);
		unlink(tmp_name);
		free(tmp_name);
		return 1;
	}

	/* close the old tdb */
	tdb_close(tdb);

	/* copy done, unlock the backup tdb */
	tdb_unlockall(tdb_new);

#ifdef HAVE_FDATASYNC
	if (fdatasync(tdb_fd(tdb_new)) != 0) {
#else
	if (fsync(tdb_fd(tdb_new)) != 0) {
#endif
		/* not fatal */
		fprintf(stderr, "failed to fsync backup file\n");
	}

	/* close the new tdb and re-open read-only */
	tdb_close(tdb_new);
	tdb_new = tdb_open_ex(tmp_name,
			      0,
			      TDB_DEFAULT,
			      O_RDONLY, 0,
			      &log_ctx, NULL);
	if (!tdb_new) {
		fprintf(stderr,"failed to reopen %s\n", tmp_name);
		unlink(tmp_name);
		perror(tmp_name);
		free(tmp_name);
		return 1;
	}

	/* traverse the new tdb to confirm */
	count2 = tdb_traverse(tdb_new, test_fn, NULL);
	if (count2 != count1) {
		fprintf(stderr,"failed to copy %s\n", old_name);
		tdb_close(tdb_new);
		unlink(tmp_name);
		free(tmp_name);
		return 1;
	}

	/* close the new tdb and rename it to .bak */
	tdb_close(tdb_new);
	if (rename(tmp_name, new_name) != 0) {
		perror(new_name);
		free(tmp_name);
		return 1;
	}

	free(tmp_name);

	return 0;
}

/*
  verify a tdb and if it is corrupt then restore from *.bak
*/
static int verify_tdb(const char *fname, const char *bak_name)
{
	TDB_CONTEXT *tdb;
	int count = -1;

	/* open the tdb */
	tdb = tdb_open_ex(fname, 0, 0,
			  O_RDONLY, 0, &log_ctx, NULL);

	/* traverse the tdb, then close it */
	if (tdb) {
		count = tdb_traverse(tdb, test_fn, NULL);
		tdb_close(tdb);
	}

	/* count is < 0 means an error */
	if (count < 0) {
		printf("restoring %s\n", fname);
		return backup_tdb(bak_name, fname, 0, 0);
	}

	printf("%s : %d records\n", fname, count);

	return 0;
}

#ifdef HAVE_LMDB
static int verify_mdb(const char *fname)
{
	int ret;
	struct MDB_env *env = NULL;
	struct MDB_txn *txn = NULL;
	MDB_dbi dbi;
	struct MDB_cursor *cursor = NULL;
	struct ldb_context *ldb = NULL;
	struct MDB_val key;
	struct MDB_val data;

	ldb = ldb_init(NULL, NULL);
	if (ldb == NULL) {
		fprintf(stderr, "ldb: ldb_init failed()");
		return 1;
	}

	ret = mdb_env_create(&env);
	if (ret != 0) {
		fprintf(stderr,
			"Could not create MDB environment: (%d)  %s\n",
			ret,
			mdb_strerror(ret));
		goto close_env;
	}

	ret = mdb_env_open(env, fname, MDB_NOSUBDIR|MDB_NOTLS, 0644);
	if (ret != 0) {
		fprintf(stderr,
			"Could not open environment for %s: (%d)  %s\n",
			fname,
			ret,
			mdb_strerror(ret));
		goto close_env;
	}

	ret = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if (ret != 0) {
		fprintf(stderr,
			"Could not start transaction: (%d)  %s\n",
			ret,
			mdb_strerror(ret));
		goto close_env;
	}

	ret = mdb_dbi_open(txn, NULL, 0, &dbi);
	if (ret != 0) {
		fprintf(stderr,
			"Could not open database: (%d)  %s\n",
			ret,
			mdb_strerror(ret));
		goto close_txn;
	}

	ret = mdb_cursor_open(txn, dbi, &cursor);
	if (ret != 0) {
		fprintf(stderr,
			"Could not open cursor: (%d)  %s\n",
			ret,
			mdb_strerror(ret));
		goto close_txn;
	}

	ret = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
	if (ret != 0 && ret != MDB_NOTFOUND) {
		fprintf(stderr,
			"Could not find first record: (%d)  %s\n",
			ret,
			mdb_strerror(ret));
		goto close_cursor;
	}
	while (ret != MDB_NOTFOUND) {
		struct ldb_val ldb_data = {
			.data = data.mv_data,
			.length = data.mv_size
		};
		struct ldb_message *msg = talloc_zero(NULL, struct ldb_message);
		ret = ldb_unpack_data(ldb, &ldb_data, msg);
		// Not interested in the contents of the record just that
		// it can be parsed.
		TALLOC_FREE(msg);
		if (ret != 0) {
			fprintf(stderr,
				"Failed to parse record %*.*s "
				"as an LDB record\n",
				(int)key.mv_size,
				(int)key.mv_size,
				(char *)key.mv_data);
			goto close_cursor;
		}
		ret = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
		if (ret != 0 && ret != MDB_NOTFOUND) {
			fprintf(stderr,
				"Could not read next record: (%d)  %s\n",
				ret,
				mdb_strerror(ret));
			goto close_cursor;
		}

	}
	ret = 0;

close_cursor:
	mdb_cursor_close(cursor);
close_txn:
	mdb_txn_commit(txn);
close_env:
	mdb_env_close(env);

	if (ret != 0) {
		return 1;
	}
	return 0;
}

static int backup_mdb(const char *old_name, const char *new_name)
{
	int ret;
	struct MDB_env *env = NULL;

	ret = mdb_env_create(&env);
	if (ret != 0) {
		fprintf(stderr,
			"Could not create MDB environment: (%d)  %s\n",
			ret,
			mdb_strerror(ret));
		mdb_env_close(env);
		return 1;
	}

	ret = mdb_env_open(env, old_name, MDB_NOSUBDIR|MDB_NOTLS, 0644);
	if (ret != 0) {
		fprintf(stderr,
			"Could not open environment for %s: (%d)  %s\n",
			old_name,
			ret,
			mdb_strerror(ret));
		mdb_env_close(env);
		return 1;
	}

	ret = mdb_env_copy(env, new_name);
	mdb_env_close(env);
	if (ret != 0) {
		fprintf(stderr,
			"Could not backup %s to %s: (%d)  %s\n",
			old_name,
			new_name,
			ret,
			mdb_strerror(ret));
		mdb_env_close(env);
		return 1;
	}
	return 0;
}
#endif /* #ifdef HAVE_LMDB */
enum db_type { UNABLE_TO_OPEN, TDB, MDB};

static enum db_type get_db_type(const char* name)
{
	struct stat st;
	TDB_CONTEXT *tdb;

	/* stat the database file */
	if (stat(name, &st) != 0) {
		perror(name);
		return UNABLE_TO_OPEN;
	}

	/* open the old tdb */
	tdb = tdb_open_ex(name, 0,
			  TDB_DEFAULT,
			  O_RDWR, 0, &log_ctx, NULL);
	if (tdb) {
		tdb_close(tdb);
		return TDB;
	}
	return MDB;
}

static int backup(const char *old_name,
		  const char *new_name,
		  int hash_size,
		  int nolock)
{
	enum db_type type = get_db_type(old_name);
	switch (type) {
	case UNABLE_TO_OPEN:
		return 1;
	case TDB:
		return backup_tdb(old_name, new_name, hash_size, nolock);
	case MDB:
		return backup_mdb(old_name, new_name);
		return 1;
	}
	return 1;
}

static int verify(const char *fname, const char *bak_name)
{
	enum db_type type = get_db_type(fname);
	int ret = 0;
	switch (type) {
	case UNABLE_TO_OPEN:
		return 1;
	case TDB:
		return verify_tdb(fname, bak_name);
	case MDB:
		ret = verify_mdb(fname);
		if (ret != 0) {
			// Database corrupted try to restore the backup.
			ret = backup_mdb(bak_name, fname);
			if (ret != 0) {
				// DO SOMETHING
			}
		}
		return 1;
	}
	return 1;
}
/*
  see if one file is newer than another
*/
static int file_newer(const char *fname1, const char *fname2)
{
	struct stat st1, st2;
	if (stat(fname1, &st1) != 0) {
		return 0;
	}
	if (stat(fname2, &st2) != 0) {
		return 1;
	}
	return (st1.st_mtime > st2.st_mtime);
}

static void usage(void)
{
	printf("Usage: tdbbackup [options] <fname...>\n\n");
	printf("   -h            this help message\n");
	printf("   -s suffix     set the backup suffix\n");
	printf("   -v            verify mode (restore if corrupt)\n");
	printf("   -n hashsize   set the new hash size for the backup\n");
	printf("   -l            open without locking to back up mutex dbs\n");
}

 int main(int argc, char *argv[])
{
	int i;
	int ret = 0;
	int c;
	bool verify_db = false;
	int hashsize = 0;
	int nolock = 0;
	const char *suffix = ".bak";

	log_ctx.log_fn = tdb_log;

	while ((c = getopt(argc, argv, "vhs:n:l")) != -1) {
		switch (c) {
		case 'h':
			usage();
			exit(0);
		case 'v':
			verify_db = true;
			break;
		case 's':
			suffix = optarg;
			break;
		case 'n':
			hashsize = atoi(optarg);
			break;
		case 'l':
			nolock = 1;
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		exit(1);
	}

	for (i=0; i<argc; i++) {
		const char *fname = argv[i];
		char *bak_name;

		bak_name = add_suffix(fname, suffix);

		if (verify_db) {
			if (verify(fname, bak_name) != 0) {
				ret = 1;
			}
		} else {
			if (file_newer(fname, bak_name) &&
			    backup(fname, bak_name, hashsize, nolock) != 0) {
				ret = 1;
			}
		}

		free(bak_name);
	}

	return ret;
}
