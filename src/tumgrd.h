#ifndef TUMGRD_H
#define TUMGRD_H

#include "db.h"

#include <libubus.h>
#include <stdbool.h>

static inline const char *nonempty_or_default(const char *input, const char *def_str) {
  return input ? input : def_str;
}

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

#define TUMGRD_DB_PATH              "/lib/tumgrd/tumgrd.db"
#define TUMGRD_DEFAULT_IP_CHECK_URL "ip.3322.net"

#define TUMGRD_STATUS_ACTIVE  "active"
#define TUMGRD_STATUS_ERROR   "error"
#define TUMGRD_STATUS_SYNCING "syncing"

#endif // TUMGRD_H
