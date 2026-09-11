#ifndef TUMGRD_H
#define TUMGRD_H

#include "db.h"

#include <libubus.h>
#include <stdbool.h>

struct tumgrd_ctx {
  struct tumgrd_db          db;
  struct tumgrd_config      cfg;
  struct ubus_context      *ubus;
  struct ubus_object        ubus_obj;
  struct ubus_event_handler net_event_handler;
  struct uloop_timeout      startup_reconcile_timer;
  struct uloop_timeout      periodic_reconcile_timer;
  bool                      net_event_registered;
  bool                      ubus_obj_added;
};

#define TUMGRD_DB_PATH               "/lib/tumgrd/tumgrd.db"
#define TUMGRD_DEFAULT_IP_CHECK_URL  "http://ip.3322.net/"
#define TUMGRD_DEFAULT_IP_CHECK_HOST "ip.3322.net"
#define TUMGRD_DEFAULT_IP_CHECK_PATH "/"
#define TUMGRD_DEFAULT_IP_CHECK_PORT 80

#define TUMGRD_IPDETECT_FWMARK 2

/* IP 探测 HTTP 请求的 connect/send/recv 超时(秒) */
#define TUMGRD_IPDETECT_TIMEOUT_S 5

/* 子进程执行(tuctl_client/ktuctl)的 deadline(秒)：超时则强杀子进程组，本次 reconcile 记为失败 */
#define TUMGRD_SUBPROC_TIMEOUT_S 90

/* deadline 到期后重新武装 alarm 的间隔(秒)：保证之后每个阻塞 syscall 都能被打断 */
#define TUMGRD_SUBPROC_TIMEOUT_RETRY_S 5

#define TUMGRD_STATUS_ACTIVE  "active"
#define TUMGRD_STATUS_ERROR   "error"
#define TUMGRD_STATUS_SYNCING "syncing"

#endif // TUMGRD_H
