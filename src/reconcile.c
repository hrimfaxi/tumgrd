#include "reconcile.h"
#include "helper.h"

#include "ipdetect.h"
#include "log.h"
#include "runner.h"
#include "try.h"
#include "tumgrd.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t now_unix(void) {
  return (int64_t) time(NULL);
}

static const char *pick_ip_check_url(const struct tumgrd_node *node) {
  if (node && node->ip_check_url[0] != '\0') {
    return node->ip_check_url;
  }
  return TUMGRD_DEFAULT_IP_CHECK_URL;
}

static int mark_runtime(struct tumgrd_db *db, const struct tumgrd_node *node, const char *current_ip, const char *status,
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
  int     err;
  int     ip_changed;
  int     need_apply;
  int     was_error;

  if (!db || !node) {
    return -1;
  }

  now = now_unix();

  log_info("[reconcile] start uid=%s server=%s:%d client_port=%d force=%d old_ip=%s ip_version=%s", node->uid,
           node->server_host, node->server_port, node->client_port, force ? 1 : 0, node->current_ip, node->ip_version);

  try2(mark_runtime(db, node, node->current_ip, TUMGRD_STATUS_SYNCING, now), "[reconcile] failed to mark syncing uid=%s",
       node->uid);
  try2(detect_public_ip(pick_ip_check_url(node), node->ip_version, detected_ip, sizeof(detected_ip)),
       "[reconcile] detect ip failed uid=%s", node->uid);

  ip_changed = !streqcase(node->current_ip, detected_ip);
  was_error  = streq(node->status, TUMGRD_STATUS_ERROR);
  need_apply = force || ip_changed || was_error;

  log_info("[reconcile] uid=%s detected_ip=%s old_ip=%s ip_changed=%d was_error=%d need_apply=%d", node->uid, detected_ip,
           node->current_ip, ip_changed ? 1 : 0, was_error ? 1 : 0, need_apply ? 1 : 0);

  if (need_apply) {
    try2(tumgrd_runner_server_add(node, detected_ip), "[reconcile] server-add failed uid=%s ip=%s", node->uid, detected_ip);

    try2(tumgrd_runner_reset_local_client(node), "[reconcile] reset local client failed uid=%s", node->uid);
  } else {
    log_info("[reconcile] skip apply uid=%s ip unchanged and force=0", node->uid);
  }

  try2(mark_runtime(db, node, detected_ip, TUMGRD_STATUS_ACTIVE, now_unix()), "[reconcile] update runtime failed uid=%s ip=%s",
       node->uid, detected_ip);
  log_info("[reconcile] success uid=%s ip=%s applied=%d", node->uid, detected_ip, need_apply ? 1 : 0);

  err = 0;
err_cleanup:
  if (err) {
    mark_runtime(db, node, detected_ip[0] ? detected_ip : node->current_ip, TUMGRD_STATUS_ERROR, now_unix());
  }
  return err;
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

  try2(tumgrd_db_list_nodes(db, &nodes, &count), "[reconcile] list nodes failed");
  log_info("[reconcile] establishing tuctl: reconcile_all count=%zu force=%d", count, force ? 1 : 0);

  for (i = 0; i < count; i++) {
    if (tumgrd_reconcile_one(db, &nodes[i], force)) {
      failed++;
    }
  }

  if (failed != 0) {
    log_error("[reconcile] reconcile_all done with failures=%d", failed);
    err = -1;
    goto err_cleanup;
  }

  log_info("[reconcile] reconcile_all done");
  err = 0;

err_cleanup:
  tumgrd_db_free_nodes(nodes);
  return err;
}

// vim: set sw=2 ts=2 et:
