#pragma once

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tumgrd_node {
  char uid[128];
  char description[256];
  char client_comment[256];

  int64_t created_at;

  char server_host[256];
  int  server_port;
  char psk[256];

  int client_port;

  int has_memlimit;
  int memlimit;

  char ip_check_url[256];
  char ip_version[16];

  char    current_ip[64];
  int64_t last_updated;
  char    status[32];
  char    xor_key[129];
};

struct tumgrd_db {
  sqlite3 *conn;
  char     path[256];
};

struct tumgrd_config {
  const char *db_path;
  const char *socket_path;
  const char *log_level;
  uint32_t    interval_sec;
  bool        enable_xor;
  int         fwmark;
};

int  tumgrd_db_open(struct tumgrd_db *db, const char *path);
int  tumgrd_db_init_schema(struct tumgrd_db *db);
void tumgrd_db_close(struct tumgrd_db *db);

int tumgrd_db_upsert_node(struct tumgrd_db *db, const struct tumgrd_node *node);

int tumgrd_db_get_node(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid, const char *ip_version,
                       struct tumgrd_node *out);

/*
 * 返回 0 表示成功
 * 返回 1 表示未找到
 * 返回 -1 表示数据库错误
 */
int tumgrd_db_delete_node(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid,
                          const char *ip_version);

int tumgrd_db_list_nodes(struct tumgrd_db *db, struct tumgrd_node **nodes, size_t *count);

void tumgrd_db_free_nodes(struct tumgrd_node *nodes);

int tumgrd_db_update_runtime(struct tumgrd_db *db, const char *server_host, int server_port, const char *uid,
                             const char *ip_version, const char *current_ip, const char *status, int64_t last_updated);

// vim :set sw=2 ts=2 et:
