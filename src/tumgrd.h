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

#define TUMGRD_STATUS_ACTIVE  "active"
#define TUMGRD_STATUS_ERROR   "error"
#define TUMGRD_STATUS_SYNCING "syncing"

#endif // TUMGRD_H
