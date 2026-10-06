module;

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <emmintrin.h>
import std.compat;
#include "std.macros.h"
#include "libc.macros.h"

#include "lmdb.h"

export module lmdb;

export using ::MDB_FIRST;
export using ::MDB_FIRST_DUP;
export using ::MDB_GET_BOTH;
export using ::MDB_GET_BOTH_RANGE;
export using ::MDB_GET_CURRENT;
export using ::MDB_GET_MULTIPLE;
export using ::MDB_LAST;
export using ::MDB_LAST_DUP;
export using ::MDB_NEXT;
export using ::MDB_NEXT_DUP;
export using ::MDB_NEXT_MULTIPLE;
export using ::MDB_NEXT_NODUP;
export using ::MDB_PREV;
export using ::MDB_PREV_DUP;
export using ::MDB_PREV_MULTIPLE;
export using ::MDB_PREV_NODUP;
export using ::MDB_SET;
export using ::MDB_SET_KEY;
export using ::MDB_SET_RANGE;
export using ::MDB_assert_func;
export using ::MDB_cmp_func;
export using ::MDB_cursor;
export using ::MDB_cursor_op;
export using ::MDB_dbi;
export using ::MDB_env;
export using ::MDB_envinfo;
export using ::MDB_msg_func;
export using ::MDB_rel_func;
export using ::MDB_stat;
export using ::MDB_txn;
export using ::MDB_val;
export using ::mdb_cmp;
export using ::mdb_cursor_close;
export using ::mdb_cursor_count;
export using ::mdb_cursor_dbi;
export using ::mdb_cursor_del;
export using ::mdb_cursor_get;
export using ::mdb_cursor_open;
export using ::mdb_cursor_put;
export using ::mdb_cursor_renew;
export using ::mdb_cursor_txn;
export using ::mdb_dbi_close;
export using ::mdb_dbi_flags;
export using ::mdb_dbi_open;
export using ::mdb_dcmp;
export using ::mdb_del;
export using ::mdb_drop;
export using ::mdb_env_close;
export using ::mdb_env_copy2;
export using ::mdb_env_copy;
export using ::mdb_env_copyfd2;
export using ::mdb_env_copyfd;
export using ::mdb_env_create;
export using ::mdb_env_get_fd;
export using ::mdb_env_get_flags;
export using ::mdb_env_get_maxkeysize;
export using ::mdb_env_get_maxreaders;
export using ::mdb_env_get_path;
export using ::mdb_env_get_userctx;
export using ::mdb_env_info;
export using ::mdb_env_open;
export using ::mdb_env_set_assert;
export using ::mdb_env_set_flags;
export using ::mdb_env_set_mapsize;
export using ::mdb_env_set_maxdbs;
export using ::mdb_env_set_maxreaders;
export using ::mdb_env_set_userctx;
export using ::mdb_env_stat;
export using ::mdb_env_sync;
export using ::mdb_filehandle_t;
export using ::mdb_get;
export using ::mdb_mode_t;
export using ::mdb_put;
export using ::mdb_reader_check;
export using ::mdb_reader_list;
export using ::mdb_set_compare;
export using ::mdb_set_dupsort;
export using ::mdb_set_relctx;
export using ::mdb_set_relfunc;
export using ::mdb_stat;
export using ::mdb_strerror;
export using ::mdb_txn_abort;
export using ::mdb_txn_begin;
export using ::mdb_txn_commit;
export using ::mdb_txn_env;
export using ::mdb_txn_id;
export using ::mdb_txn_renew;
export using ::mdb_txn_reset;
export using ::mdb_version;

