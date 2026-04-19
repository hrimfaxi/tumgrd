#include "db.h"
#include "helper.h"
#include "log.h"
#include "tumgrd.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t tumgrd_now_unix(void) {
  return (int64_t) time(NULL);
}

static void tumgrd_sqlite_log_error(sqlite3 *conn, const char *where, int err) {
  log_error("[db] %s failed: err=%d msg=%s", where, err, conn ? sqlite3_errmsg(conn) : "no sqlite handle");
}

static int tumgrd_bind_text_or_null(sqlite3_stmt *stmt, int idx, const char *value) {
  if (!value || value[0] == '\0') {
    return sqlite3_bind_null(stmt, idx);
  }
  return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

static int tumgrd_bind_required_text(sqlite3_stmt *stmt, int idx, const char *value) {
  if (!value) {
    value = "";
  }
  return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

static void tumgrd_column_text(sqlite3_stmt *stmt, int col, char *dst, size_t dst_len) {
  const unsigned char *text;

  if (!dst || dst_len == 0) {
    return;
  }

  if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
    dst[0] = '\0';
    return;
  }

  text = sqlite3_column_text(stmt, col);
  copy_string(dst, dst_len, (const char *) text);
}

static int tumgrd_row_to_node(sqlite3_stmt *stmt, struct tumgrd_node *node) {
  if (!stmt || !node) {
    return -1;
  }

  memset(node, 0, sizeof(*node));

  tumgrd_column_text(stmt, 0, node->uid, sizeof(node->uid));
  tumgrd_column_text(stmt, 1, node->description, sizeof(node->description));
  tumgrd_column_text(stmt, 2, node->client_comment, sizeof(node->client_comment));
  node->created_at = (int64_t) sqlite3_column_int64(stmt, 3);
  tumgrd_column_text(stmt, 4, node->server_host, sizeof(node->server_host));
  node->server_port = sqlite3_column_int(stmt, 5);
  tumgrd_column_text(stmt, 6, node->psk, sizeof(node->psk));
  node->client_port = sqlite3_column_int(stmt, 7);

  if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
    node->has_memlimit = 1;
    node->memlimit     = sqlite3_column_int(stmt, 8);
  } else {
    node->has_memlimit = 0;
    node->memlimit     = 0;
  }

  tumgrd_column_text(stmt, 9, node->ip_check_url, sizeof(node->ip_check_url));
  tumgrd_column_text(stmt, 10, node->ip_version, sizeof(node->ip_version));
  tumgrd_column_text(stmt, 11, node->current_ip, sizeof(node->current_ip));
  node->last_updated = (int64_t) sqlite3_column_int64(stmt, 12);
  tumgrd_column_text(stmt, 13, node->status, sizeof(node->status));

  return 0;
}

static int tumgrd_mkdir_p(const char *dir, mode_t mode) {
  char   tmp[PATH_MAX];
  size_t len;
  char  *p;

  if (!dir || dir[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  len = strlen(dir);
  if (len >= sizeof(tmp)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  snprintf(tmp, sizeof(tmp), "%s", dir);

  if (tmp[len - 1] == '/') {
    tmp[len - 1] = '\0';
  }

  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }

  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    return -1;
  }

  return 0;
}

static int tumgrd_ensure_parent_dir(const char *path) {
  char  dir[PATH_MAX];
  char *slash;

  if (!path || path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  if (strlen(path) >= sizeof(dir)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  snprintf(dir, sizeof(dir), "%s", path);

  slash = strrchr(dir, '/');
  if (!slash) {
    return 0;
  }

  if (slash == dir) {
    return 0;
  }

  *slash = '\0';

  return tumgrd_mkdir_p(dir, 0755);
}

int tumgrd_db_open(struct tumgrd_db *db, const char *path) {
  int         err;
  const char *db_path;

  if (!db) {
    return -1;
  }

  memset(db, 0, sizeof(*db));

  db_path = (path && path[0] != '\0') ? path : TUMGRD_DB_PATH;
  copy_string(db->path, sizeof(db->path), db_path);

  if (tumgrd_ensure_parent_dir(db->path) != 0) {
    log_error("[db] ensure parent dir failed for %s: %s", db->path, strerror(errno));
    return -1;
  }

  err = sqlite3_open_v2(db->path, &db->conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_open_v2", err);
    if (db->conn) {
      sqlite3_close(db->conn);
      db->conn = NULL;
    }
    return -1;
  }

  sqlite3_busy_timeout(db->conn, 3000);

  return 0;
}

int tumgrd_db_init_schema(struct tumgrd_db *db) {
  static const char *sql = "CREATE TABLE IF NOT EXISTS nodes ("
                           " uid TEXT NOT NULL,"
                           " description TEXT,"
                           " client_comment TEXT,"
                           " created_at INTEGER,"
                           " server_host TEXT NOT NULL,"
                           " server_port INTEGER NOT NULL,"
                           " psk TEXT NOT NULL,"
                           " client_port INTEGER NOT NULL,"
                           " memlimit INTEGER,"
                           " ip_check_url TEXT DEFAULT 'ip.3322.net',"
                           " ip_version TEXT,"
                           " current_ip TEXT,"
                           " last_updated INTEGER,"
                           " status TEXT DEFAULT 'active',"
                           " PRIMARY KEY (server_host, server_port, uid, ip_version),"
                           " UNIQUE (server_host, server_port, client_port, ip_version)"
                           ");";

  char *errmsg = NULL;
  int   err;

  if (!db || !db->conn) {
    return -1;
  }

  err = sqlite3_exec(db->conn, sql, NULL, NULL, &errmsg);
  if (err != SQLITE_OK) {
    log_error("[db] init schema failed: err=%d msg=%s", err, errmsg ? errmsg : "unknown");
    sqlite3_free(errmsg);
    return -1;
  }

  return 0;
}

void tumgrd_db_close(struct tumgrd_db *db) {
  if (!db) {
    return;
  }

  if (db->conn) {
    sqlite3_close(db->conn);
    db->conn = NULL;
  }

  db->path[0] = '\0';
}

int tumgrd_db_upsert_node(struct tumgrd_db *db, const struct tumgrd_node *node) {
  static const char *sql = "INSERT INTO nodes ("
                           " uid, description, client_comment, created_at,"
                           " server_host, server_port, psk, client_port,"
                           " memlimit, ip_check_url, ip_version, current_ip,"
                           " last_updated, status"
                           ") VALUES ("
                           " ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"
                           ") "
                           "ON CONFLICT(server_host, server_port, uid) DO UPDATE SET "
                           " description=excluded.description,"
                           " client_comment=excluded.client_comment,"
                           " created_at=excluded.created_at,"
                           " psk=excluded.psk,"
                           " client_port=excluded.client_port,"
                           " memlimit=excluded.memlimit,"
                           " ip_check_url=excluded.ip_check_url,"
                           " ip_version=excluded.ip_version,"
                           " current_ip=excluded.current_ip,"
                           " last_updated=excluded.last_updated,"
                           " status=excluded.status;";

  sqlite3_stmt      *stmt = NULL;
  struct tumgrd_node n;
  int                err;

  if (!db || !db->conn || !node) {
    return -1;
  }

  memset(&n, 0, sizeof(n));
  n = *node;

  if (n.created_at == 0) {
    n.created_at = tumgrd_now_unix();
  }

  if (n.last_updated == 0) {
    n.last_updated = tumgrd_now_unix();
  }

  if (n.ip_check_url[0] == '\0') {
    copy_string(n.ip_check_url, sizeof(n.ip_check_url), TUMGRD_DEFAULT_IP_CHECK_URL);
  }

  if (n.status[0] == '\0') {
    copy_string(n.status, sizeof(n.status), TUMGRD_STATUS_ACTIVE);
  }

  err = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_prepare_v2(upsert)", err);
    return -1;
  }

  err = tumgrd_bind_required_text(stmt, 1, n.uid);
  err = err == SQLITE_OK ? tumgrd_bind_text_or_null(stmt, 2, nonempty_or_null(n.description)) : err;
  err = err == SQLITE_OK ? tumgrd_bind_text_or_null(stmt, 3, nonempty_or_null(n.client_comment)) : err;
  err = err == SQLITE_OK ? sqlite3_bind_int64(stmt, 4, (sqlite3_int64) n.created_at) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 5, n.server_host) : err;
  err = err == SQLITE_OK ? sqlite3_bind_int(stmt, 6, n.server_port) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 7, n.psk) : err;
  err = err == SQLITE_OK ? sqlite3_bind_int(stmt, 8, n.client_port) : err;
  err = err == SQLITE_OK ? (n.has_memlimit ? sqlite3_bind_int(stmt, 9, n.memlimit) : sqlite3_bind_null(stmt, 9)) : err;
  err = err == SQLITE_OK ? tumgrd_bind_text_or_null(stmt, 10, nonempty_or_null(n.ip_check_url)) : err;
  err = err == SQLITE_OK ? tumgrd_bind_text_or_null(stmt, 11, nonempty_or_null(n.ip_version)) : err;
  err = err == SQLITE_OK ? tumgrd_bind_text_or_null(stmt, 12, nonempty_or_null(n.current_ip)) : err;
  err = err == SQLITE_OK ? sqlite3_bind_int64(stmt, 13, (sqlite3_int64) n.last_updated) : err;
  err = err == SQLITE_OK ? tumgrd_bind_text_or_null(stmt, 14, nonempty_or_null(n.status)) : err;

  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "bind(upsert)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  err = sqlite3_step(stmt);
  if (err != SQLITE_DONE) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_step(upsert)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  sqlite3_finalize(stmt);
  return 0;
}

int tumgrd_db_get_node(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid, const char *ip_version,
                       struct tumgrd_node *out) {
  static const char *sql = "SELECT "
                           " uid, description, client_comment, created_at,"
                           " server_host, server_port, psk, client_port,"
                           " memlimit, ip_check_url, ip_version, current_ip,"
                           " last_updated, status"
                           " FROM nodes"
                           " WHERE server_host = ? AND server_port = ? AND uid = ? AND ip_version = ?"
                           " LIMIT 1;";

  sqlite3_stmt *stmt = NULL;
  int           err;

  if (!db || !db->conn || !server_host || !uid || !out) {
    return -1;
  }

  err = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_prepare_v2(get)", err);
    return -1;
  }

  err = tumgrd_bind_required_text(stmt, 1, server_host);
  err = err == SQLITE_OK ? sqlite3_bind_int(stmt, 2, server_port) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 3, uid) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 4, ip_version) : err;

  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "bind(get)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  err = sqlite3_step(stmt);
  if (err == SQLITE_ROW) {
    tumgrd_row_to_node(stmt, out);
    sqlite3_finalize(stmt);
    return 0;
  }

  if (err == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return 1;
  }

  tumgrd_sqlite_log_error(db->conn, "sqlite3_step(get)", err);
  sqlite3_finalize(stmt);
  return -1;
}

int tumgrd_db_delete_node(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid,
                          const char *ip_version) {
  static const char *sql = "DELETE FROM nodes"
                           " WHERE server_host = ? AND server_port = ? AND uid = ? AND ip_version = ?;";

  sqlite3_stmt *stmt = NULL;
  int           err;
  int           changes;

  if (!db || !db->conn || !server_host || !uid) {
    return -1;
  }

  err = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_prepare_v2(delete)", err);
    return -1;
  }

  err = tumgrd_bind_required_text(stmt, 1, server_host);
  err = err == SQLITE_OK ? sqlite3_bind_int(stmt, 2, server_port) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 3, uid) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 4, ip_version) : err;

  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "bind(delete)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  err = sqlite3_step(stmt);
  if (err != SQLITE_DONE) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_step(delete)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  changes = sqlite3_changes(db->conn);
  sqlite3_finalize(stmt);

  return changes > 0 ? 0 : 1;
}

int tumgrd_db_list_nodes(struct tumgrd_db *db, struct tumgrd_node **nodes, size_t *count) {
  static const char *sql = "SELECT "
                           " uid, description, client_comment, created_at,"
                           " server_host, server_port, psk, client_port,"
                           " memlimit, ip_check_url, ip_version, current_ip,"
                           " last_updated, status"
                           " FROM nodes"
                           " ORDER BY server_host, server_port, uid;";

  sqlite3_stmt       *stmt = NULL;
  struct tumgrd_node *arr  = NULL;
  size_t              cap  = 0;
  size_t              n    = 0;
  int                 err;

  if (!db || !db->conn || !nodes || !count) {
    return -1;
  }

  *nodes = NULL;
  *count = 0;

  err = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_prepare_v2(list)", err);
    return -1;
  }

  for (;;) {
    err = sqlite3_step(stmt);

    if (err == SQLITE_ROW) {
      struct tumgrd_node *new_arr;

      if (n == cap) {
        size_t new_cap = (cap == 0) ? 8 : (cap * 2);

        new_arr = realloc(arr, new_cap * sizeof(*arr));
        if (!new_arr) {
          log_error("[db] realloc failed in list_nodes");
          sqlite3_finalize(stmt);
          free(arr);
          return -1;
        }

        arr = new_arr;
        cap = new_cap;
      }

      if (tumgrd_row_to_node(stmt, &arr[n]) != 0) {
        log_error("[db] row_to_node failed in list_nodes");
        sqlite3_finalize(stmt);
        free(arr);
        return -1;
      }

      n++;
      continue;
    }

    if (err == SQLITE_DONE) {
      break;
    }

    tumgrd_sqlite_log_error(db->conn, "sqlite3_step(list)", err);
    sqlite3_finalize(stmt);
    free(arr);
    return -1;
  }

  sqlite3_finalize(stmt);

  *nodes = arr;
  *count = n;
  return 0;
}

void tumgrd_db_free_nodes(struct tumgrd_node *nodes) {
  free(nodes);
}

int tumgrd_db_update_runtime(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid,
                             const char *ip_version, const char *current_ip, const char *status, int64_t last_updated) {
  static const char *sql = "UPDATE nodes"
                           " SET current_ip = ?, status = ?, last_updated = ?"
                           " WHERE server_host = ? AND server_port = ? AND uid = ? AND ip_version = ?;";

  sqlite3_stmt *stmt = NULL;
  int           err;

  if (!db || !db->conn || !server_host || !uid || !status) {
    return -1;
  }

  err = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_prepare_v2(update_runtime)", err);
    return -1;
  }

  err = tumgrd_bind_text_or_null(stmt, 1, nonempty_or_null(current_ip));
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 2, status) : err;
  err = err == SQLITE_OK ? sqlite3_bind_int64(stmt, 3, (sqlite3_int64) last_updated) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 4, server_host) : err;
  err = err == SQLITE_OK ? sqlite3_bind_int(stmt, 5, server_port) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 6, uid) : err;
  err = err == SQLITE_OK ? tumgrd_bind_required_text(stmt, 7, ip_version) : err;

  if (err != SQLITE_OK) {
    tumgrd_sqlite_log_error(db->conn, "bind(update_runtime)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  err = sqlite3_step(stmt);
  if (err != SQLITE_DONE) {
    tumgrd_sqlite_log_error(db->conn, "sqlite3_step(update_runtime)", err);
    sqlite3_finalize(stmt);
    return -1;
  }

  sqlite3_finalize(stmt);
  return 0;
}
