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

int tumgrd_reconcile_one(struct tumgrd_db *db, const struct tumgrd_config *cfg, struct tumgrd_node *node, bool force) {
  char    detected_ip[64]  = {0};
  char    new_xor_key[129] = {0};
  int64_t now;
  int     err;
  int     ip_changed;
  int     need_apply;
  int     was_error;
  bool    expired                = false;
  bool    server_apply_succeeded = false;

  if (!db || !cfg || !node) {
    return -1;
  }

  now = now_unix();

  if (node->rotation_state < TUMGRD_ROTATION_NONE || node->rotation_state > TUMGRD_ROTATION_PENDING_LOCAL_RECOVERY) {
    log_warn("[reconcile] invalid rotation_state %d uid=%s", node->rotation_state, node->uid);
    if (cfg->enable_xor && node->xor_key[0] != '\0') {
      if (generate_random_hex_key(new_xor_key, sizeof(new_xor_key), 64) != 0) {
        log_error("[reconcile] failed to generate replacement key uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      if (tumgrd_db_replace_rotation(db, node->server_host, node->server_port, node->uid, node->ip_version, new_xor_key, now) !=
          0) {
        log_error("[reconcile] failed to replace rotation state uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      snprintf(node->xor_key, sizeof(node->xor_key), "%s", new_xor_key);
      node->rotation_state = TUMGRD_ROTATION_PENDING_REMOTE;
      snprintf(node->xor_key_pending, sizeof(node->xor_key_pending), "%s", new_xor_key);
      log_info("[reconcile] replaced invalid state with fresh rotation uid=%s", node->uid);
    } else {
      if (tumgrd_db_clear_rotation(db, node->server_host, node->server_port, node->uid, node->ip_version) != 0) {
        log_error("[reconcile] failed to reset invalid state uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      node->rotation_state     = TUMGRD_ROTATION_NONE;
      node->xor_key_pending[0] = '\0';
    }
  }

  if (node->rotation_state != TUMGRD_ROTATION_NONE &&
      (!node->xor_key_pending[0] || strcmp(node->xor_key, node->xor_key_pending) != 0)) {
    log_warn("[reconcile] rotation key mismatch uid=%s", node->uid);
    if (cfg->enable_xor && node->xor_key[0] != '\0') {
      if (generate_random_hex_key(new_xor_key, sizeof(new_xor_key), 64) != 0) {
        log_error("[reconcile] failed to generate replacement key uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      if (tumgrd_db_replace_rotation(db, node->server_host, node->server_port, node->uid, node->ip_version, new_xor_key, now) !=
          0) {
        log_error("[reconcile] failed to replace rotation state uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      snprintf(node->xor_key, sizeof(node->xor_key), "%s", new_xor_key);
      node->rotation_state = TUMGRD_ROTATION_PENDING_REMOTE;
      snprintf(node->xor_key_pending, sizeof(node->xor_key_pending), "%s", new_xor_key);
      log_info("[reconcile] replaced mismatched key with fresh rotation uid=%s", node->uid);
    } else {
      if (tumgrd_db_clear_rotation(db, node->server_host, node->server_port, node->uid, node->ip_version) != 0) {
        log_error("[reconcile] failed to reset mismatched state uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      node->rotation_state     = TUMGRD_ROTATION_NONE;
      node->xor_key_pending[0] = '\0';
    }
  }

  if (node->rotation_state == TUMGRD_ROTATION_PENDING_LOCAL_RECOVERY) {
    /* server_add 已确认成功，但本地恢复未完成。
     * 重发 server_add（幂等）以处理期间 IP 变化，然后重试本地恢复。 */
    log_info("[reconcile] resuming pending local recovery uid=%s", node->uid);
    force   = true;
    expired = true;
  } else if (node->rotation_state == TUMGRD_ROTATION_PENDING_REMOTE) {
    log_info("[reconcile] resuming pending remote apply uid=%s", node->uid);
    force   = true;
    expired = true;
  } else if (now < TUMGRD_MIN_SANE_TIME) {
    log_warn("[reconcile] system clock unreliable (now=%lld), skipping lifetime check uid=%s", (long long) now, node->uid);
  } else if (node->lifetime > 0 && node->last_applied_at == 0) {
    expired = true;
    force   = true;
    log_info("[reconcile] never applied, seeding lifetime timer uid=%s", node->uid);
  } else if (node->lifetime > 0 && (now - node->last_applied_at) >= node->lifetime) {
    expired = true;
    force   = true;
    log_info("[reconcile] node expired uid=%s lifetime=%lld last_applied_at=%lld now=%lld", node->uid,
             (long long) node->lifetime, (long long) node->last_applied_at, (long long) now);

    if (cfg->enable_xor && node->xor_key[0] != '\0') {
      if (generate_random_hex_key(new_xor_key, sizeof(new_xor_key), 64) != 0) {
        log_error("[reconcile] failed to generate xor key uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      if (tumgrd_db_begin_rotation(db, node->server_host, node->server_port, node->uid, node->ip_version, new_xor_key,
                                   TUMGRD_ROTATION_PENDING_REMOTE, now) != 0) {
        log_error("[reconcile] failed to begin rotation uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      snprintf(node->xor_key, sizeof(node->xor_key), "%s", new_xor_key);
      node->rotation_state = TUMGRD_ROTATION_PENDING_REMOTE;
      snprintf(node->xor_key_pending, sizeof(node->xor_key_pending), "%s", new_xor_key);
      log_info("[reconcile] xor key rotated uid=%s, pending remote apply", node->uid);
    }
  }

  log_info("[reconcile] start uid=%s server=%s:%d client_port=%d force=%d old_ip=%s ip_version=%s expired=%d rot_state=%d",
           node->uid, node->server_host, node->server_port, node->client_port, force ? 1 : 0, node->current_ip,
           node->ip_version, expired ? 1 : 0, node->rotation_state);

  try2(detect_public_ip(pick_ip_check_url(node), node->ip_version, detected_ip, sizeof(detected_ip)),
       "[reconcile] detect ip failed uid=%s", node->uid);

  ip_changed = !streqcase(node->current_ip, detected_ip);
  was_error  = streq(node->status, TUMGRD_STATUS_ERROR) || streq(node->status, TUMGRD_STATUS_SYNCING);
  need_apply = force || ip_changed || was_error;

  log_info("[reconcile] uid=%s detected_ip=%s old_ip=%s ip_changed=%d was_error=%d need_apply=%d", node->uid, detected_ip,
           node->current_ip, ip_changed ? 1 : 0, was_error ? 1 : 0, need_apply ? 1 : 0);

  if (!need_apply) {
    log_info("[reconcile] skip uid=%s - ip unchanged, not forced, no error", node->uid);
    return 0;
  }

  try2(tumgrd_runner_server_add(node, cfg, detected_ip), "[reconcile] server-add failed uid=%s ip=%s", node->uid, detected_ip);
  server_apply_succeeded = true;

  if (node->rotation_state == TUMGRD_ROTATION_PENDING_REMOTE) {
    if (tumgrd_db_update_rotation_state(db, node->server_host, node->server_port, node->uid, node->ip_version,
                                         TUMGRD_ROTATION_PENDING_LOCAL_RECOVERY) != 0) {
      log_error("[reconcile] failed to update rotation state to PENDING_LOCAL_RECOVERY uid=%s", node->uid);
      err = -1;
      goto err_cleanup;
    }
    node->rotation_state = TUMGRD_ROTATION_PENDING_LOCAL_RECOVERY;
    log_info("[reconcile] server_add confirmed, pending local recovery uid=%s", node->uid);
  }

  try2(tumgrd_runner_reset_local_client(node), "[reconcile] reset local client failed uid=%s", node->uid);

  try2(mark_runtime(db, node, detected_ip, TUMGRD_STATUS_ACTIVE, now_unix()), "[reconcile] update runtime failed uid=%s ip=%s",
       node->uid, detected_ip);

  {
    int64_t applied_ts = (now >= TUMGRD_MIN_SANE_TIME) ? now : 0;

    if (node->rotation_state != TUMGRD_ROTATION_NONE && server_apply_succeeded) {
      if (tumgrd_db_complete_rotation(db, node->server_host, node->server_port, node->uid, node->ip_version, applied_ts) != 0) {
        log_error("[reconcile] failed to complete rotation uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      node->rotation_state     = TUMGRD_ROTATION_NONE;
      node->xor_key_pending[0] = '\0';
      node->last_applied_at    = applied_ts;
      log_info("[reconcile] rotation completed uid=%s", node->uid);
    } else if (expired && server_apply_succeeded) {
      /* 不能用 need_apply：否则频繁 IP 变化会无限推迟到期，破坏密钥寿命上界 */
      /* 此处 now 可信性由 server_apply_succeeded 守卫保证：
       * - rotation_state == NONE 时 expired 只可能来自时钟守卫之后的分支 → now 可信
       * - rotation_state != NONE 时（PENDING_* 恢复，now 可能不可信）必然 !server_apply_succeeded，
       *   否则会走上面的 complete_rotation 分支 → 不会写入
       * 若移除任一守卫，必须在此重新加回 now >= TUMGRD_MIN_SANE_TIME 检查。 */
      if (tumgrd_db_update_applied(db, node->server_host, node->server_port, node->uid, node->ip_version, now) != 0) {
        log_error("[reconcile] failed to update last_applied_at uid=%s", node->uid);
        err = -1;
        goto err_cleanup;
      }
      node->last_applied_at = now;
    }
  }

  log_info("[reconcile] success uid=%s ip=%s applied=1", node->uid, detected_ip);

  err = 0;
err_cleanup:
  if (err) {
    mark_runtime(db, node, detected_ip[0] ? detected_ip : node->current_ip, TUMGRD_STATUS_ERROR, now_unix());
    if (server_apply_succeeded) {
      log_error("[reconcile] CRITICAL: local steps failed after server_add succeeded uid=%s; "
                "rotation_state=%d preserved, will retry on next cycle",
                node->uid, node->rotation_state);
    }
  }
  return err;
}

int tumgrd_reconcile_all(struct tumgrd_db *db, const struct tumgrd_config *cfg, bool force) {
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
    if (tumgrd_reconcile_one(db, cfg, &nodes[i], force)) {
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
