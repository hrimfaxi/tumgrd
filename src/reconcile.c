#include "reconcile.h"
#include "helper.h"

#include "ipdetect.h"
#include "log.h"
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

  return tumgrd_db_update_runtime(db, node->server_host, node->server_port, node->uid, node->ip_version, current_ip, status,
                                  ts);
}

int tumgrd_reconcile_one(struct tumgrd_db *db, struct tumgrd_node *node, bool force) {
  char    detected_ip[64] = {0};
  int64_t now;
  int     ret;
  int     ip_changed;
  int     need_apply;
  int     was_error;

  if (!db || !node) {
    return -1;
  }

  now = tumgrd_now_unix();

  log_info("[reconcile] start uid=%s server=%s:%d client_port=%d force=%d old_ip=%s", node->uid, node->server_host,
           node->server_port, node->client_port, force ? 1 : 0, node->current_ip);

  ret = tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_SYNCING, now);
  if (ret != 0) {
    log_error("[reconcile] failed to mark syncing uid=%s", node->uid);
    return -1;
  }

  ret = tumgrd_detect_public_ip(tumgrd_pick_ip_check_url(node), node->ip_version, detected_ip, sizeof(detected_ip));
  if (ret != 0) {
    log_error("[reconcile] detect ip failed uid=%s", node->uid);
    tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
    return -1;
  }

  ip_changed = !streqcase(node->current_ip, detected_ip);
  was_error  = streq(node->status, TUMGRD_STATUS_ERROR);
  need_apply = force || ip_changed || was_error;

  log_info("[reconcile] uid=%s detected_ip=%s old_ip=%s ip_changed=%d was_error=%d need_apply=%d", node->uid, detected_ip,
           node->current_ip, ip_changed ? 1 : 0, was_error ? 1 : 0, need_apply ? 1 : 0);

  if (need_apply) {
    ret = tumgrd_runner_server_add(node, detected_ip);
    if (ret != 0) {
      log_error("[reconcile] server-add failed uid=%s ip=%s", node->uid, detected_ip);
      tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
      return -1;
    }

    ret = tumgrd_runner_reset_local_client(node);
    if (ret != 0) {
      log_error("[reconcile] reset local client failed uid=%s", node->uid);
      tumgrd_mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_ERROR, tumgrd_now_unix());
      return -1;
    }
  } else {
    log_info("[reconcile] skip apply uid=%s ip unchanged and force=0", node->uid);
  }

  ret = tumgrd_mark_runtime(db, node, detected_ip, TUMGRD_STATUS_ACTIVE, tumgrd_now_unix());
  if (ret != 0) {
    log_error("[reconcile] update runtime failed uid=%s ip=%s", node->uid, detected_ip);
    return -1;
  }

  log_info("[reconcile] success uid=%s ip=%s applied=%d", node->uid, detected_ip, need_apply ? 1 : 0);

  return 0;
}

int tumgrd_reconcile_all(struct tumgrd_db *db, bool force) {
  struct tumgrd_node *nodes = NULL;
  size_t              count = 0;
  size_t              i;
  int                 err;
  int                 failed = 0;

  if (!db) {
    return -1;
  }

  err = tumgrd_db_list_nodes(db, &nodes, &count);
  if (err != 0) {
    log_error("[reconcile] list nodes failed");
    return -1;
  }

  log_info("[reconcile] reconcile_all count=%zu force=%d", count, force ? 1 : 0);

  for (i = 0; i < count; i++) {
    if (tumgrd_reconcile_one(db, &nodes[i], force) != 0) {
      failed++;
    }
  }

  tumgrd_db_free_nodes(nodes);

  if (failed != 0) {
    log_error("[reconcile] reconcile_all done with failures=%d", failed);
    return -1;
  }

  log_info("[reconcile] reconcile_all done");
  return 0;
}

// vim: set sw=2 ts=2 et:
