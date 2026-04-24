#include "ubus_if.h"
#include "helper.h"
#include "log.h"

#include "reconcile.h"
#include "runner.h"
#include "tumgrd.h"

#include <libubox/blobmsg_json.h>
#include <libubox/utils.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TUMGRD_STARTUP_RECONCILE_DELAY_MS 3000

static bool blobmsg_get_bool_default(struct blob_attr *attr, bool defval) {
  if (!attr) {
    return defval;
  }
  return blobmsg_get_u8(attr) ? true : false;
}

static void reply_simple(struct ubus_context *ctx, struct ubus_request_data *req, const char *status, const char *message) {
  struct blob_buf b = {0};

  blob_buf_init(&b, 0);

  if (status) {
    blobmsg_add_string(&b, "status", status);
  }
  if (message) {
    blobmsg_add_string(&b, "message", message);
  }

  ubus_send_reply(ctx, req, b.head);
  blob_buf_free(&b);
}

static void add_node_brief(struct blob_buf *b, const struct tumgrd_node *n) {
  if (!b || !n) {
    return;
  }

  blobmsg_add_string(b, "uid", n->uid);
  blobmsg_add_string(b, "server_host", n->server_host);
  blobmsg_add_u32(b, "server_port", (uint32_t) n->server_port);
  blobmsg_add_string(b, "psk", n->psk);
  blobmsg_add_u32(b, "client_port", (uint32_t) n->client_port);

  if (n->description[0] != '\0') {
    blobmsg_add_string(b, "description", n->description);
  }
  if (n->client_comment[0] != '\0') {
    blobmsg_add_string(b, "client_comment", n->client_comment);
  }
  if (n->ip_check_url[0] != '\0') {
    blobmsg_add_string(b, "ip_check_url", n->ip_check_url);
  }
  if (n->ip_version[0] != '\0') {
    blobmsg_add_string(b, "ip_version", n->ip_version);
  }
  if (n->current_ip[0] != '\0') {
    blobmsg_add_string(b, "current_ip", n->current_ip);
  } else {
    blobmsg_add_string(b, "current_ip", "");
  }
  if (n->status[0] != '\0') {
    blobmsg_add_string(b, "node_status", n->status);
  } else {
    blobmsg_add_string(b, "node_status", "");
  }

  blobmsg_add_u32(b, "created_at", (uint32_t) n->created_at);
  blobmsg_add_u32(b, "last_updated", (uint32_t) n->last_updated);

  if (n->has_memlimit) {
    blobmsg_add_u32(b, "memlimit", (uint32_t) n->memlimit);
  }
}

/*
 * 按真实主键 (server_host, server_port, uid) 查找节点。
 *
 * 返回：
 *   0  成功
 *   1  未找到
 *  -1  数据库错误 / 参数错误
 */
static int resolve_node(struct tumgrd_db *db, const char *uid, const char *server_host, int server_port, const char *ip_version,
                        struct tumgrd_node *out) {
  int err;

  if (!db || !uid || !server_host || !ip_version || !out) {
    return -1;
  }

  err = tumgrd_db_get_node(db, server_host, server_port, uid, ip_version, out);
  if (err == 0) {
    return 0;
  }
  if (err == 1) {
    return 1;
  }

  return -1;
}

/* =========================================================================
 * 1. register
 * ========================================================================= */

enum {
  REG_UID,
  REG_SERVER_HOST,
  REG_SERVER_PORT,
  REG_CLIENT_PORT,
  REG_PSK,
  REG_DESCRIPTION,
  REG_CLIENT_COMMENT,
  REG_MEMLIMIT,
  REG_IP_CHECK_URL,
  REG_IP_VERSION,
  __REG_MAX
};

static const struct blobmsg_policy reg_policy[__REG_MAX] = {
  [REG_UID]            = {.name = "uid", .type = BLOBMSG_TYPE_STRING},
  [REG_SERVER_HOST]    = {.name = "server_host", .type = BLOBMSG_TYPE_STRING},
  [REG_SERVER_PORT]    = {.name = "server_port", .type = BLOBMSG_TYPE_INT32},
  [REG_CLIENT_PORT]    = {.name = "client_port", .type = BLOBMSG_TYPE_INT32},
  [REG_PSK]            = {.name = "psk", .type = BLOBMSG_TYPE_STRING},
  [REG_DESCRIPTION]    = {.name = "description", .type = BLOBMSG_TYPE_STRING},
  [REG_CLIENT_COMMENT] = {.name = "client_comment", .type = BLOBMSG_TYPE_STRING},
  [REG_MEMLIMIT]       = {.name = "memlimit", .type = BLOBMSG_TYPE_INT32},
  [REG_IP_CHECK_URL]   = {.name = "ip_check_url", .type = BLOBMSG_TYPE_STRING},
  [REG_IP_VERSION]     = {.name = "ip_version", .type = BLOBMSG_TYPE_STRING},
};

static int handle_register(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method,
                           struct blob_attr *msg) {
  struct blob_attr  *tb[__REG_MAX];
  struct tumgrd_node node;
  struct tumgrd_node old_node;
  int                get_rc;
  const char        *action;
  int                err;
  struct blob_buf    b = {0};

  (void) method;
  struct tumgrd_ctx *tctx = container_of(obj, struct tumgrd_ctx, ubus_obj);

  memset(&node, 0, sizeof(node));
  memset(&old_node, 0, sizeof(old_node));

  blobmsg_parse(reg_policy, __REG_MAX, tb, blob_data(msg), blob_len(msg));

  if (!tb[REG_UID] || !tb[REG_SERVER_HOST] || !tb[REG_SERVER_PORT] || !tb[REG_CLIENT_PORT] || !tb[REG_PSK]) {
    return UBUS_STATUS_INVALID_ARGUMENT;
  }

  get_rc = tumgrd_db_get_node(&tctx->db, blobmsg_get_string(tb[REG_SERVER_HOST]), (int) blobmsg_get_u32(tb[REG_SERVER_PORT]),
                              blobmsg_get_string(tb[REG_UID]), blobmsg_get_string(tb[REG_IP_VERSION]), &old_node);

  if (get_rc == 0) {
    node   = old_node;
    action = "updated";
  } else if (get_rc == 1) {
    action = "created";
  } else {
    return UBUS_STATUS_UNKNOWN_ERROR;
  }

  snprintf(node.uid, sizeof(node.uid), "%s", blobmsg_get_string(tb[REG_UID]));
  snprintf(node.server_host, sizeof(node.server_host), "%s", blobmsg_get_string(tb[REG_SERVER_HOST]));
  node.server_port = (int) blobmsg_get_u32(tb[REG_SERVER_PORT]);
  node.client_port = (int) blobmsg_get_u32(tb[REG_CLIENT_PORT]);
  snprintf(node.psk, sizeof(node.psk), "%s", blobmsg_get_string(tb[REG_PSK]));

  if (tb[REG_DESCRIPTION]) {
    snprintf(node.description, sizeof(node.description), "%s", blobmsg_get_string(tb[REG_DESCRIPTION]));
  }

  if (tb[REG_CLIENT_COMMENT]) {
    snprintf(node.client_comment, sizeof(node.client_comment), "%s", blobmsg_get_string(tb[REG_CLIENT_COMMENT]));
  }

  if (tb[REG_MEMLIMIT]) {
    node.has_memlimit = 1;
    node.memlimit     = (int) blobmsg_get_u32(tb[REG_MEMLIMIT]);
  }

  if (tb[REG_IP_CHECK_URL]) {
    snprintf(node.ip_check_url, sizeof(node.ip_check_url), "%s", blobmsg_get_string(tb[REG_IP_CHECK_URL]));
  }

  if (tb[REG_IP_VERSION]) {
    snprintf(node.ip_version, sizeof(node.ip_version), "%s", blobmsg_get_string(tb[REG_IP_VERSION]));
  }

  err = tumgrd_db_upsert_node(&tctx->db, &node);
  if (err != 0) {
    return UBUS_STATUS_UNKNOWN_ERROR;
  }

  /*
   * register 后立刻应用配置。
   * 失败时仍保留 DB 中的 desired state，方便后续自愈。
   */
  err = tumgrd_reconcile_one(&tctx->db, &node, true);

  blob_buf_init(&b, 0);
  blobmsg_add_string(&b, "status", err == 0 ? "ok" : "stored_but_apply_failed");
  blobmsg_add_string(&b, "action", action);
  blobmsg_add_u8(&b, "applied", err == 0 ? 1 : 0);
  add_node_brief(&b, &node);
  ubus_send_reply(ctx, req, b.head);
  blob_buf_free(&b);

  return UBUS_STATUS_OK;
}

/* =========================================================================
 * 2. deregister
 * ========================================================================= */

enum { DEREG_UID, DEREG_SERVER_HOST, DEREG_SERVER_PORT, DEREG_IP_VERSION, __DEREG_MAX };

static const struct blobmsg_policy dereg_policy[__DEREG_MAX] = {
  [DEREG_UID]         = {.name = "uid", .type = BLOBMSG_TYPE_STRING},
  [DEREG_SERVER_HOST] = {.name = "server_host", .type = BLOBMSG_TYPE_STRING},
  [DEREG_SERVER_PORT] = {.name = "server_port", .type = BLOBMSG_TYPE_INT32},
  [DEREG_IP_VERSION]  = {.name = "ip_version", .type = BLOBMSG_TYPE_STRING},
};

static int handle_deregister(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
                             const char *method, struct blob_attr *msg) {
  struct blob_attr  *tb[__DEREG_MAX];
  struct tumgrd_node node;
  int                resolve_rc;
  int                rc_server;
  int                rc_client;
  int                rc_db;
  struct blob_buf    b = {0};

  (void) method;

  struct tumgrd_ctx *tctx = container_of(obj, struct tumgrd_ctx, ubus_obj);

  blobmsg_parse(dereg_policy, __DEREG_MAX, tb, blob_data(msg), blob_len(msg));

  if (!tb[DEREG_UID] || !tb[DEREG_SERVER_HOST] || !tb[DEREG_SERVER_PORT] || !tb[DEREG_IP_VERSION]) {
    return UBUS_STATUS_INVALID_ARGUMENT;
  }

  resolve_rc = resolve_node(&tctx->db, blobmsg_get_string(tb[DEREG_UID]), blobmsg_get_string(tb[DEREG_SERVER_HOST]),
                            (int) blobmsg_get_u32(tb[DEREG_SERVER_PORT]), blobmsg_get_string(tb[DEREG_IP_VERSION]), &node);

  if (resolve_rc == 1) {
    reply_simple(ctx, req, "not_found", "node not found");
    return UBUS_STATUS_OK;
  }

  if (resolve_rc != 0) {
    return UBUS_STATUS_UNKNOWN_ERROR;
  }

  rc_server = tumgrd_runner_server_del(&node);
  rc_client = tumgrd_runner_client_del(&node);
  rc_db     = tumgrd_db_delete_node(&tctx->db, node.server_host, node.server_port, node.uid, node.ip_version);

  blob_buf_init(&b, 0);

  if (rc_db != 0) {
    blobmsg_add_string(&b, "status", "error");
    blobmsg_add_string(&b, "message", "database delete failed");
  } else if (rc_server == 0 && rc_client == 0) {
    blobmsg_add_string(&b, "status", "deleted");
  } else {
    blobmsg_add_string(&b, "status", "deleted_with_cleanup_errors");
  }

  blobmsg_add_u8(&b, "server_deleted", rc_server == 0 ? 1 : 0);
  blobmsg_add_u8(&b, "client_deleted", rc_client == 0 ? 1 : 0);
  blobmsg_add_u8(&b, "db_deleted", rc_db == 0 ? 1 : 0);
  add_node_brief(&b, &node);

  ubus_send_reply(ctx, req, b.head);
  blob_buf_free(&b);

  return UBUS_STATUS_OK;
}

/* =========================================================================
 * 3. refresh
 * ========================================================================= */

enum { REF_UID, REF_SERVER_HOST, REF_SERVER_PORT, REF_IP_VERSION, REF_FORCE, REF_ALL, __REF_MAX };

static const struct blobmsg_policy ref_policy[__REF_MAX] = {
  [REF_UID]         = {.name = "uid", .type = BLOBMSG_TYPE_STRING},
  [REF_SERVER_HOST] = {.name = "server_host", .type = BLOBMSG_TYPE_STRING},
  [REF_SERVER_PORT] = {.name = "server_port", .type = BLOBMSG_TYPE_INT32},
  [REF_IP_VERSION]  = {.name = "ip_version", .type = BLOBMSG_TYPE_STRING},
  [REF_FORCE]       = {.name = "force", .type = BLOBMSG_TYPE_BOOL},
  [REF_ALL]         = {.name = "all", .type = BLOBMSG_TYPE_BOOL},
};

static int handle_refresh(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg) {
  struct blob_attr *tb[__REF_MAX];
  bool              is_all;
  bool              is_force;
  struct blob_buf   b = {0};
  int               err;

  (void) method;

  struct tumgrd_ctx *tctx = container_of(obj, struct tumgrd_ctx, ubus_obj);

  blobmsg_parse(ref_policy, __REF_MAX, tb, blob_data(msg), blob_len(msg));

  is_all   = blobmsg_get_bool_default(tb[REF_ALL], false);
  is_force = blobmsg_get_bool_default(tb[REF_FORCE], false);

  blob_buf_init(&b, 0);

  if (is_all) {
    err = tumgrd_reconcile_all(&tctx->db, is_force);

    blobmsg_add_string(&b, "status", err == 0 ? "ok" : "partial_error");
    blobmsg_add_string(&b, "scope", "all");
    blobmsg_add_u8(&b, "force", is_force ? 1 : 0);

    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return UBUS_STATUS_OK;
  }

  if (!tb[REF_UID] || !tb[REF_SERVER_HOST] || !tb[REF_SERVER_PORT] || !tb[REF_IP_VERSION]) {
    blob_buf_free(&b);
    return UBUS_STATUS_INVALID_ARGUMENT;
  }

  {
    struct tumgrd_node node;
    int                resolve_rc;

    resolve_rc = resolve_node(&tctx->db, blobmsg_get_string(tb[REF_UID]), blobmsg_get_string(tb[REF_SERVER_HOST]),
                              (int) blobmsg_get_u32(tb[REF_SERVER_PORT]), blobmsg_get_string(tb[REF_IP_VERSION]), &node);

    if (resolve_rc == 1) {
      blobmsg_add_string(&b, "status", "not_found");
      ubus_send_reply(ctx, req, b.head);
      blob_buf_free(&b);
      return UBUS_STATUS_OK;
    }

    if (resolve_rc != 0) {
      blob_buf_free(&b);
      return UBUS_STATUS_UNKNOWN_ERROR;
    }

    err = tumgrd_reconcile_one(&tctx->db, &node, is_force);

    blobmsg_add_string(&b, "status", err == 0 ? "ok" : "error");
    blobmsg_add_string(&b, "scope", "one");
    blobmsg_add_u8(&b, "force", is_force ? 1 : 0);
    blobmsg_add_string(&b, "note", "force accepted for compatibility; current reconcile path is always full apply");
    add_node_brief(&b, &node);

    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return UBUS_STATUS_OK;
  }
}

/* =========================================================================
 * 4. status / dump
 * ========================================================================= */

static int handle_status(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method,
                         struct blob_attr *msg) {
  struct tumgrd_node *nodes = NULL;
  size_t              count = 0;
  size_t              i;
  int                 err;
  struct blob_buf     b = {0};
  void               *array;

  (void) method;
  (void) msg;

  struct tumgrd_ctx *tctx = container_of(obj, struct tumgrd_ctx, ubus_obj);

  err = tumgrd_db_list_nodes(&tctx->db, &nodes, &count);
  if (err != 0) {
    return UBUS_STATUS_UNKNOWN_ERROR;
  }

  blob_buf_init(&b, 0);
  blobmsg_add_string(&b, "status", "ok");
  blobmsg_add_u32(&b, "count", (uint32_t) count);

  array = blobmsg_open_array(&b, "nodes");
  for (i = 0; i < count; i++) {
    void *table = blobmsg_open_table(&b, NULL);
    add_node_brief(&b, &nodes[i]);
    blobmsg_close_table(&b, table);
  }
  blobmsg_close_array(&b, array);

  ubus_send_reply(ctx, req, b.head);
  blob_buf_free(&b);

  tumgrd_db_free_nodes(nodes);
  return UBUS_STATUS_OK;
}

/* =========================================================================
 * ubus object
 * ========================================================================= */

static const struct ubus_method tumgrd_methods[] = {
  UBUS_METHOD("register", handle_register, reg_policy),
  UBUS_METHOD("deregister", handle_deregister, dereg_policy),
  UBUS_METHOD("refresh", handle_refresh, ref_policy),
  UBUS_METHOD_NOARG("status", handle_status),
  UBUS_METHOD_NOARG("dump", handle_status),
};

static struct ubus_object_type tumgrd_obj_type = UBUS_OBJECT_TYPE("tumgrd", tumgrd_methods);

static void net_event_cb(struct ubus_context *ctx, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg);

static void startup_reconcile_timer_cb(struct uloop_timeout *t);
static void periodic_reconcile_timer_cb(struct uloop_timeout *t);

int tumgrd_ubus_init(struct tumgrd_ctx *ctx) {
  int err;

  if (!ctx) {
    return -1;
  }

  ctx->ubus = ubus_connect(ctx->cfg.socket_path);
  if (!ctx->ubus) {
    log_error("ubus_connect failed: socket=%s", nonempty_or_default(ctx->cfg.socket_path, "(default)"));
    return -1;
  }
  ubus_add_uloop(ctx->ubus);

  ctx->net_event_registered = false;
  ctx->ubus_obj_added       = false;
  memset(&ctx->ubus_obj, 0, sizeof(ctx->ubus_obj));
  memset(&ctx->net_event_handler, 0, sizeof(ctx->net_event_handler));

  ctx->net_event_handler.cb = net_event_cb;
  err                       = ubus_register_event_handler(ctx->ubus, &ctx->net_event_handler, "network.interface");
  if (err != 0) {
    tumgrd_ubus_cleanup(ctx);
    return err;
  }
  ctx->net_event_registered = true;

  ctx->ubus_obj.name      = "tumgrd";
  ctx->ubus_obj.type      = &tumgrd_obj_type;
  ctx->ubus_obj.methods   = tumgrd_methods;
  ctx->ubus_obj.n_methods = ARRAY_SIZE(tumgrd_methods);

  err = ubus_add_object(ctx->ubus, &ctx->ubus_obj);
  if (err != 0) {
    tumgrd_ubus_cleanup(ctx);
    return err;
  }
  ctx->ubus_obj_added = true;

  ctx->startup_reconcile_timer.cb = startup_reconcile_timer_cb;
  uloop_timeout_set(&ctx->startup_reconcile_timer, TUMGRD_STARTUP_RECONCILE_DELAY_MS);

  ctx->periodic_reconcile_timer.cb = periodic_reconcile_timer_cb;
  uloop_timeout_set(&ctx->periodic_reconcile_timer, ctx->cfg.interval_sec * 1000);

  return 0;
}

void tumgrd_ubus_cleanup(struct tumgrd_ctx *ctx) {
  if (!ctx || !ctx->ubus)
    return;

  if (ctx->net_event_registered) {
    ubus_unregister_event_handler(ctx->ubus, &ctx->net_event_handler);
    ctx->net_event_registered = false;
  }

  if (ctx->ubus_obj_added) {
    ubus_remove_object(ctx->ubus, &ctx->ubus_obj);
    ctx->ubus_obj_added = false;
  }

  uloop_timeout_cancel(&ctx->startup_reconcile_timer);
  uloop_timeout_cancel(&ctx->periodic_reconcile_timer);

  if (ctx->ubus) {
    ubus_free(ctx->ubus);
    ctx->ubus = NULL;
  }
}

static void net_event_cb(struct ubus_context *ctx, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg) {
  (void) ctx;
  (void) type;

  if (!msg) {
    return;
  }

  struct tumgrd_ctx *tctx = container_of(ev, struct tumgrd_ctx, net_event_handler);

  /* ubus listen 看到的格式:
   * { "network.interface": {"action":"ifup","interface":"bird2"} }
   */
  struct blob_attr                  *tb[2];
  static const struct blobmsg_policy policy[] = {
    [0] = {"action", BLOBMSG_TYPE_STRING},
    [1] = {"interface", BLOBMSG_TYPE_STRING},
  };

  blobmsg_parse(policy, 2, tb, blob_data(msg), blob_len(msg));

  if (!tb[0] || !tb[1])
    return;

  const char *action = blobmsg_get_string(tb[0]);
  const char *ifname = blobmsg_get_string(tb[1]);

  log_debug("[ubus] network.interface %s %s", ifname, action);

  /* 只处理 ifup 事件，且接口匹配 */
  if (streq(action, "ifup")) {
    int  err;
    bool is_wan = false;

    err = check_wan_interface(ifname, &is_wan);
    if (err) {
      log_error("[uci] check wan interface failed: %d", err);
      return;
    }

    if (is_wan) {
      log_info("[ubus] WAN interface %s up, force reconcile", ifname);
      tumgrd_reconcile_all(&tctx->db, true);
    }
  }
}

static void startup_reconcile_timer_cb(struct uloop_timeout *t) {
  struct tumgrd_ctx *ctx = container_of(t, struct tumgrd_ctx, startup_reconcile_timer);

  tumgrd_reconcile_all(&ctx->db, true);
}

static void periodic_reconcile_timer_cb(struct uloop_timeout *t) {
  struct tumgrd_ctx *ctx = container_of(t, struct tumgrd_ctx, periodic_reconcile_timer);

  tumgrd_reconcile_all(&ctx->db, false);
  uloop_timeout_set(&ctx->periodic_reconcile_timer, ctx->cfg.interval_sec * 1000);
}

// vim: set sw=2 ts=2 et:
