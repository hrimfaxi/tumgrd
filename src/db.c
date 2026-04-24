#include "db.h"
#include "helper.h"
#include "log.h"
#include "try.h"
#include "tumgrd.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SQLITE_TRY(expr, conn, what)                                                                                           \
  ({                                                                                                                           \
    int _sql_ret = (expr);                                                                                                     \
    if (unlikely(_sql_ret != SQLITE_OK)) {                                                                                     \
      sqlite_log_error(conn, what, _sql_ret);                                                                                  \
      err = -1;                                                                                                                \
      goto err_cleanup;                                                                                                        \
    }                                                                                                                          \
    _sql_ret;                                                                                                                  \
  })

#define SQLITE_TRY_STEP_DONE(expr, conn, what)                                                                                 \
  ({                                                                                                                           \
    int _sql_ret = (expr);                                                                                                     \
    if (unlikely(_sql_ret != SQLITE_DONE)) {                                                                                   \
      sqlite_log_error(conn, what, _sql_ret);                                                                                  \
      err = -1;                                                                                                                \
      goto err_cleanup;                                                                                                        \
    }                                                                                                                          \
    _sql_ret;                                                                                                                  \
  })

static int64_t now_unix(void) {
  return (int64_t) time(NULL);
}

static void sqlite_log_error(sqlite3 *conn, const char *where, int err) {
  log_error("[db] %s failed: err=%d msg=%s", where, err, conn ? sqlite3_errmsg(conn) : "no sqlite handle");
}

static int bind_text_or_null(sqlite3_stmt *stmt, int idx, const char *value) {
  if (!value || value[0] == '\0') {
    return sqlite3_bind_null(stmt, idx);
  }
  return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

static int bind_required_text(sqlite3_stmt *stmt, int idx, const char *value) {
  if (!value) {
    value = "";
  }
  return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

static void column_text(sqlite3_stmt *stmt, int col, char *dst, size_t dst_len) {
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

static int row_to_node(sqlite3_stmt *stmt, struct tumgrd_node *node) {
  if (!stmt || !node) {
    return -1;
  }

  memset(node, 0, sizeof(*node));

  column_text(stmt, 0, node->uid, sizeof(node->uid));
  column_text(stmt, 1, node->description, sizeof(node->description));
  column_text(stmt, 2, node->client_comment, sizeof(node->client_comment));
  node->created_at = (int64_t) sqlite3_column_int64(stmt, 3);
  column_text(stmt, 4, node->server_host, sizeof(node->server_host));
  node->server_port = sqlite3_column_int(stmt, 5);
  column_text(stmt, 6, node->psk, sizeof(node->psk));
  node->client_port = sqlite3_column_int(stmt, 7);

  if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
    node->has_memlimit = 1;
    node->memlimit     = sqlite3_column_int(stmt, 8);
  } else {
    node->has_memlimit = 0;
    node->memlimit     = 0;
  }

  column_text(stmt, 9, node->ip_check_url, sizeof(node->ip_check_url));
  column_text(stmt, 10, node->ip_version, sizeof(node->ip_version));
  column_text(stmt, 11, node->current_ip, sizeof(node->current_ip));
  node->last_updated = (int64_t) sqlite3_column_int64(stmt, 12);
  column_text(stmt, 13, node->status, sizeof(node->status));

  return 0;
}

int tumgrd_db_open(struct tumgrd_db *db, const char *path) {
  int         err = -1;
  const char *db_path;
  sqlite3    *conn = NULL;

  if (!db) {
    goto err_cleanup;
  }

  memset(db, 0, sizeof(*db));
  db_path = (path && path[0] != '\0') ? path : TUMGRD_DB_PATH;
  copy_string(db->path, sizeof(db->path), db_path);
  try2(ensure_parent_dir(db->path), "[db] ensure parent dir failed for %s: %s", db->path, strerror(errno));
  SQLITE_TRY(sqlite3_open_v2(db->path, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), conn, "open_v2");
  db->conn = conn;
  sqlite3_busy_timeout(db->conn, 3000);
  err = 0;

err_cleanup:
  if (err < 0 && conn) {
    sqlite3_close(conn);
    conn = NULL;
  }
  return err;
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
  int   err    = -1;

  if (!db || !db->conn) {
    goto err_cleanup;
  }

  SQLITE_TRY(sqlite3_exec(db->conn, sql, NULL, NULL, &errmsg), db->conn, "init_schema");
  err = 0;
err_cleanup:
  sqlite3_free(errmsg);
  return err;
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
                           "ON CONFLICT(server_host, server_port, uid, ip_version) DO UPDATE SET "
                           " description=excluded.description,"
                           " client_comment=excluded.client_comment,"
                           " created_at=excluded.created_at,"
                           " psk=excluded.psk,"
                           " client_port=excluded.client_port,"
                           " memlimit=excluded.memlimit,"
                           " ip_check_url=excluded.ip_check_url,"
                           " current_ip=excluded.current_ip,"
                           " last_updated=excluded.last_updated,"
                           " status=excluded.status;";

  sqlite3_stmt      *stmt = NULL;
  struct tumgrd_node n;
  int                err = -1;

  if (!db || !db->conn || !node) {
    goto err_cleanup;
  }

  memset(&n, 0, sizeof(n));
  n = *node;

  if (n.created_at == 0) {
    n.created_at = now_unix();
  }

  if (n.last_updated == 0) {
    n.last_updated = now_unix();
  }

  if (n.ip_check_url[0] == '\0') {
    copy_string(n.ip_check_url, sizeof(n.ip_check_url), TUMGRD_DEFAULT_IP_CHECK_URL);
  }

  if (n.status[0] == '\0') {
    copy_string(n.status, sizeof(n.status), TUMGRD_STATUS_ACTIVE);
  }

  sqlite3 *conn = db->conn;

  SQLITE_TRY(sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL), conn, "prepare(upsert)");
  SQLITE_TRY(bind_required_text(stmt, 1, n.uid), conn, "bind(uid)");
  SQLITE_TRY(bind_text_or_null(stmt, 2, nonempty_or_null(n.description)), conn, "bind(description)");
  SQLITE_TRY(bind_text_or_null(stmt, 3, nonempty_or_null(n.client_comment)), conn, "bind(client_comment)");
  SQLITE_TRY(sqlite3_bind_int64(stmt, 4, (sqlite3_int64) n.created_at), conn, "bind(created_at)");
  SQLITE_TRY(bind_required_text(stmt, 5, n.server_host), conn, "bind(server_host)");
  SQLITE_TRY(sqlite3_bind_int(stmt, 6, n.server_port), conn, "bind(server_port)");
  SQLITE_TRY(bind_required_text(stmt, 7, n.psk), conn, "bind(psk)");
  SQLITE_TRY(sqlite3_bind_int(stmt, 8, n.client_port), conn, "bind(client_port)");
  SQLITE_TRY(n.has_memlimit ? sqlite3_bind_int(stmt, 9, n.memlimit) : sqlite3_bind_null(stmt, 9), conn, "bind(memlimit)");
  SQLITE_TRY(bind_text_or_null(stmt, 10, nonempty_or_null(n.ip_check_url)), conn, "bind(ip_check_url)");
  SQLITE_TRY(bind_text_or_null(stmt, 11, nonempty_or_null(n.ip_version)), conn, "bind(ip_version)");
  SQLITE_TRY(bind_text_or_null(stmt, 12, nonempty_or_null(n.current_ip)), conn, "bind(current_ip)");
  SQLITE_TRY(sqlite3_bind_int64(stmt, 13, (sqlite3_int64) n.last_updated), conn, "bind(last_updated)");
  SQLITE_TRY(bind_text_or_null(stmt, 14, nonempty_or_null(n.status)), conn, "bind(status)");
  SQLITE_TRY_STEP_DONE(sqlite3_step(stmt), conn, "step(upsert)");
  err = 0;
err_cleanup:
  if (stmt)
    sqlite3_finalize(stmt);
  return err;
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
  int           err  = -1;

  if (!db || !db->conn || !server_host || !uid || !out) {
    goto err_cleanup;
  }

  sqlite3 *conn = db->conn;

  SQLITE_TRY(sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL), conn, "prepare(get)");
  SQLITE_TRY(bind_required_text(stmt, 1, server_host), conn, "bind(server_host)");
  SQLITE_TRY(sqlite3_bind_int(stmt, 2, server_port), conn, "bind(server_port)");
  SQLITE_TRY(bind_required_text(stmt, 3, uid), conn, "bind(uid)");
  SQLITE_TRY(bind_required_text(stmt, 4, ip_version), conn, "bind(ip_version)");

  err = sqlite3_step(stmt);

  if (err == SQLITE_DONE) {
    err = 1; // 没找到
    goto err_cleanup;
  }

  if (err != SQLITE_ROW) {
    sqlite_log_error(conn, "step(get)", err);
    goto err_cleanup;
  }

  row_to_node(stmt, out);
  err = 0;
err_cleanup:
  if (stmt)
    sqlite3_finalize(stmt);
  return err;
}

int tumgrd_db_delete_node(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid,
                          const char *ip_version) {
  static const char *sql = "DELETE FROM nodes"
                           " WHERE server_host = ? AND server_port = ? AND uid = ? AND ip_version = ?;";

  sqlite3_stmt *stmt = NULL;
  int           err  = -1;
  int           changes;

  if (!db || !db->conn || !server_host || !uid) {
    goto err_cleanup;
  }

  sqlite3 *conn = db->conn;

  SQLITE_TRY(sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL), conn, "prepare(delete)");
  SQLITE_TRY(bind_required_text(stmt, 1, server_host), conn, "bind(server_host)");
  SQLITE_TRY(sqlite3_bind_int(stmt, 2, server_port), conn, "bind(server_port)");
  SQLITE_TRY(bind_required_text(stmt, 3, uid), conn, "bind(uid)");
  SQLITE_TRY(bind_required_text(stmt, 4, ip_version), conn, "bind(ip_version)");
  SQLITE_TRY_STEP_DONE(sqlite3_step(stmt), conn, "step(delete)");

  changes = sqlite3_changes(db->conn);
  err     = (changes > 0) ? 0 : 1; /* 0=deleted, 1=not found */
err_cleanup:
  if (stmt)
    sqlite3_finalize(stmt);
  return err;
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
  int                 err  = -1;

  if (!db || !db->conn || !nodes || !count) {
    goto err_cleanup;
  }

  *nodes = NULL;
  *count = 0;

  sqlite3 *conn = db->conn;

  SQLITE_TRY(sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL), conn, "prepare(list)");

  for (;;) {
    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
      struct tumgrd_node *new_arr;

      if (n == cap) {
        size_t new_cap = (cap == 0) ? 8 : (cap * 2);

        new_arr = try2_p(realloc(arr, new_cap * sizeof(*arr)), "[db] realloc failed in list_nodes");

        arr = new_arr;
        cap = new_cap;
      }

      try2(row_to_node(stmt, &arr[n]), "[db] row_to_node failed in list_nodes");
      n++;
      continue;
    }

    if (rc == SQLITE_DONE) {
      break;
    }

    sqlite_log_error(conn, "step(list)", rc);
    goto err_cleanup;
  }

  *nodes = arr;
  *count = n;
  arr    = NULL; // 已转移
  err    = 0;

err_cleanup:
  if (stmt)
    sqlite3_finalize(stmt);
  stmt = NULL;
  free(arr);
  arr = NULL;
  return err;
}

void tumgrd_db_free_nodes(struct tumgrd_node *nodes) {
  free(nodes);
}

int tumgrd_db_update_runtime(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid,
                             const char *ip_version, const char *current_ip, const char *status, int64_t last_updated) {
  sqlite3_stmt      *stmt = NULL;
  int                err  = -1;
  static const char *sql  = "UPDATE nodes"
                            " SET current_ip = ?, status = ?, last_updated = ?"
                            " WHERE server_host = ? AND server_port = ? AND uid = ? AND ip_version = ?;";

  if (!db || !db->conn || !server_host || !uid || !ip_version || !status) {
    goto err_cleanup;
  }

  sqlite3 *conn = db->conn;

  SQLITE_TRY(sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL), conn, "prepare(update_runtime)");
  SQLITE_TRY(bind_text_or_null(stmt, 1, nonempty_or_null(current_ip)), conn, "bind(current_ip)");
  SQLITE_TRY(bind_required_text(stmt, 2, status), conn, "bind(status)");
  SQLITE_TRY(sqlite3_bind_int64(stmt, 3, last_updated), conn, "bind(last_updated)");
  SQLITE_TRY(bind_required_text(stmt, 4, server_host), conn, "bind(server_host)");
  SQLITE_TRY(sqlite3_bind_int(stmt, 5, server_port), conn, "bind(server_port)");
  SQLITE_TRY(bind_required_text(stmt, 6, uid), conn, "bind(uid)");
  SQLITE_TRY(bind_required_text(stmt, 7, ip_version), conn, "bind(ip_version)");
  SQLITE_TRY_STEP_DONE(sqlite3_step(stmt), conn, "step(update_runtime)");

  err = 0;

err_cleanup:
  if (stmt)
    sqlite3_finalize(stmt);
  return err;
}

// vim: set sw=2 ts=2 et:
