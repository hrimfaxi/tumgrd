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

int tumgrd_reconcile_one(struct tumgrd_db *db, struct tumgrd_node *node, bool force) {
  char    detected_ip[64] = {0};
  int64_t now;
  int     ret;
  int     ip_changed;
  int     need_apply;

  if (!db || !node) {
    return -1;
  }

  now = tumgrd_now_unix();

  fprintf(stderr, "[reconcile] start uid=%s server=%s:%d client_port=%d force=%d old_ip=%s\n", node->uid, node->server_host,
          node->server_port, node->client_port, force ? 1 : 0, node->current_ip);

  ret = tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_SYNCING, now);
  if (ret != 0) {
    fprintf(stderr, "[reconcile] failed to mark syncing uid=%s\n", node->uid);
    return -1;
  }

  ret = tumgrd_detect_public_ip(tumgrd_pick_ip_check_url(node), node->ip_version, detected_ip, sizeof(detected_ip));
  if (ret != 0) {
    fprintf(stderr, "[reconcile] detect ip failed uid=%s\n", node->uid);
    tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
    return -1;
  }

  ip_changed = strcmp(node->current_ip, detected_ip) != 0;
  need_apply = force || ip_changed;

  fprintf(stderr, "[reconcile] uid=%s detected_ip=%s old_ip=%s ip_changed=%d need_apply=%d\n", node->uid, detected_ip,
          node->current_ip, ip_changed ? 1 : 0, need_apply ? 1 : 0);

  if (need_apply) {
    ret = tumgrd_runner_server_add(node, detected_ip);
    if (ret != 0) {
      fprintf(stderr, "[reconcile] server-add failed uid=%s ip=%s\n", node->uid, detected_ip);
      tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
      return -1;
    }

    ret = tumgrd_runner_reset_local_client(node);
    if (ret != 0) {
      fprintf(stderr, "[reconcile] reset local client failed uid=%s\n", node->uid);
      tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
      return -1;
    }
  } else {
    fprintf(stderr, "[reconcile] skip apply uid=%s ip unchanged and force=0\n", node->uid);
  }

  ret = tumgrd_mark_runtime(db, node, detected_ip, TUMGRD_STATUS_ACTIVE, tumgrd_now_unix());
  if (ret != 0) {
    fprintf(stderr, "[reconcile] update runtime failed uid=%s ip=%s\n", node->uid, detected_ip);
    return -1;
  }

  fprintf(stderr, "[reconcile] success uid=%s ip=%s applied=%d\n", node->uid, detected_ip, need_apply ? 1 : 0);

  return 0;
}

int tumgrd_reconcile_all(struct tumgrd_db *db, bool force) {
  struct tumgrd_node *nodes = NULL;
  size_t              count = 0;
  size_t              i;
  int                 rc;
  int                 failed = 0;

  if (!db) {
    return -1;
  }

  rc = tumgrd_db_list_nodes(db, &nodes, &count);
  if (rc != 0) {
    fprintf(stderr, "[reconcile] list nodes failed\n");
    return -1;
  }

  fprintf(stderr, "[reconcile] reconcile_all count=%zu force=%d\n", count, force ? 1 : 0);

  for (i = 0; i < count; i++) {
    if (tumgrd_reconcile_one(db, &nodes[i], force) != 0) {
      failed++;
    }
  }

  tumgrd_db_free_nodes(nodes);

  if (failed != 0) {
    fprintf(stderr, "[reconcile] reconcile_all done with failures=%d\n", failed);
    return -1;
  }

  fprintf(stderr, "[reconcile] reconcile_all done\n");
  return 0;
}
