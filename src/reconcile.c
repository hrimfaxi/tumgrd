#include "reconcile.h"

#include "ipdetect.h"
#include "runner.h"
#include "tumgrd.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t tumgrd_now_unix(void) {
  return (int64_t) time(NULL);
}

static const char *tumgrd_pick_ip_check_url(const struct tumgrd_node *node) {
  if (node && node->ip_check_url[0] != '\0') {
    return node->ip_check_url;
  }
  return TUMGRD_DEFAULT_IP_CHECK_URL;
}

static int tumgrd_mark_runtime(struct tumgrd_db *db, const struct tumgrd_node *node, const char *current_ip, const char *status,
                               int64_t ts) {
  if (!db || !node || !status) {
    return -1;
  }

  return tumgrd_db_update_runtime(db, node->server_host, node->server_port, node->uid, current_ip, status, ts);
}

int tumgrd_reconcile_one(struct tumgrd_db *db, struct tumgrd_node *node) {
  char    detected_ip[64] = {0};
  int64_t now;
  int     ret;

  if (!db || !node) {
    return -1;
  }

  now = tumgrd_now_unix();

  fprintf(stderr, "[reconcile] start uid=%s server=%s:%d client_port=%d\n", node->uid, node->server_host, node->server_port,
          node->client_port);

  /*
   * 先把状态置为 syncing。
   * 这里 current_ip 先保留数据库旧值，只有全部成功后才写入新 IP。
   */
  ret = tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_SYNCING, now);
  if (ret != 0) {
    fprintf(stderr, "[reconcile] failed to mark syncing uid=%s\n", node->uid);
    return -1;
  }

  /*
   * 1. 探测当前公网 IP
   */
  ret = tumgrd_detect_public_ip(tumgrd_pick_ip_check_url(node), node->ip_version, detected_ip, sizeof(detected_ip));
  if (ret != 0) {
    fprintf(stderr, "[reconcile] detect ip failed uid=%s\n", node->uid);

    tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
    return -1;
  }

  /*
   * 2. 恢复远端配置
   *
   * 启动自愈这里建议始终执行，而不是只在 IP 变化时执行。
   * 因为你定义的“自愈”就是重启后主动把运行态重新打起来。
   */
  ret = tumgrd_runner_server_add(node, detected_ip);
  if (ret != 0) {
    fprintf(stderr, "[reconcile] server-add failed uid=%s ip=%s\n", node->uid, detected_ip);

    tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
    return -1;
  }

  /*
   * 3. 重建本地 ktuctl 配置
   *
   * 这里建议 runner 内部实现成：
   *   client-del (忽略错误) + client-add
   */
  ret = tumgrd_runner_reset_local_client(node);
  if (ret != 0) {
    fprintf(stderr, "[reconcile] reset local client failed uid=%s\n", node->uid);

    tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
    return -1;
  }

  /*
   * 4. 全部成功后更新数据库运行态
   */
  ret = tumgrd_mark_runtime(db, node, detected_ip, TUMGRD_STATUS_ACTIVE, tumgrd_now_unix());
  if (ret != 0) {
    fprintf(stderr, "[reconcile] update runtime failed uid=%s ip=%s\n", node->uid, detected_ip);
    return -1;
  }

  /*
   * 同步内存中的 node，方便上层继续使用。
   */
  snprintf(node->current_ip, sizeof(node->current_ip), "%s", detected_ip);
  snprintf(node->status, sizeof(node->status), "%s", TUMGRD_STATUS_ACTIVE);
  node->last_updated = tumgrd_now_unix();

  fprintf(stderr, "[reconcile] success uid=%s ip=%s\n", node->uid, detected_ip);

  return 0;
}

int tumgrd_reconcile_all(struct tumgrd_db *db) {
  struct tumgrd_node *nodes = NULL;
  size_t              count = 0;
  size_t              i;
  int                 failed = 0;
  int                 ret;

  if (!db) {
    return -1;
  }

  ret = tumgrd_db_list_nodes(db, &nodes, &count);
  if (ret != 0) {
    fprintf(stderr, "[reconcile] list nodes failed\n");
    return -1;
  }

  fprintf(stderr, "[reconcile] loaded %zu node(s) from db\n", count);

  for (i = 0; i < count; i++) {
    if (tumgrd_reconcile_one(db, &nodes[i]) != 0) {
      failed++;
    }
  }

  tumgrd_db_free_nodes(nodes);

  if (failed > 0) {
    fprintf(stderr, "[reconcile] finished with failures: total=%zu failed=%d\n", count, failed);
    return -1;
  }

  fprintf(stderr, "[reconcile] finished successfully: total=%zu\n", count);
  return 0;
}
