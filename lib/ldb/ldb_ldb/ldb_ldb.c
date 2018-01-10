
#include "ldb_private.h"
#include "../ldb_tdb/ldb_tdb.h"
#include "../ldb_mdb/ldb_mdb.h"

/*
  connect to the database
*/
static int lldb_connect(struct ldb_context *ldb,
			const char *url,
			unsigned int flags,
			const char *options[],
			struct ldb_module **module)
{
	const char *path;
	int ret;

	/*
	 * Check and remove the url prefix
	 */
	if (strchr(url, ':')) {
		if (strncmp(url, "ldb://", 6) != 0) {
			ldb_debug(ldb, LDB_DEBUG_ERROR,
				  "Invalid ldb URL '%s'", url);
			return LDB_ERR_OPERATIONS_ERROR;
		}
		path = url+6;
	} else {
		path = url;
	}

	/*
	 * Don't create the database if it's not there
	 */
	flags |= LDB_FLG_DONT_CREATE_DB;
	/*
	 * Try opening the database as an lmdb
	 */
	ret = lmdb_connect(ldb, path, flags, options, module);
	if (ret == LDB_SUCCESS) {
		return ret;
	}
	if (ret == LDB_ERR_UNAVAILABLE) {
		/*
		* Not mbd to try as tdb
		*/
		ret = ltdb_connect(ldb, path, flags, options, module);
	}
	return ret;
}

int ldb_ldb_init(const char *version)
{
	LDB_MODULE_CHECK_VERSION(version);
	return ldb_register_backend("ldb", lldb_connect, false);
}
