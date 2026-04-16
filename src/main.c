#include "db.h"
#include "log.h"
#include "reconcile.h"
#include "try.h"
#include "tumgrd.h"
#include "ubus_if.h"

#include <libubox/uloop.h>
#include <libubus.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>

#define TUMGRD_STARTUP_RECONCILE_DELAY_MS 3000

static struct tumgrd_db     g_db;
static struct ubus_context *g_ubus = NULL;
static struct uloop_timeout g_reconcile_timer;

static void tumgrd_reconcile_timer_cb(struct uloop_timeout *t) {
  (void) t;
  (void) tumgrd_reconcile_all(&g_db, true);
}

static void tumgrd_signal_handler(int signo) {
  (void) signo;
  uloop_end();
}

int main(int argc, char **argv) {
  const char *db_path   = TUMGRD_DB_PATH;
  int         err       = 1;
  bool        db_opened = false;

  if (argc > 1 && argv[1] && argv[1][0] != '\0') {
    db_path = argv[1];
  }

  memset(&g_db, 0, sizeof(g_db));
  memset(&g_reconcile_timer, 0, sizeof(g_reconcile_timer));

  try2(uloop_init(), "[main] uloop_init failed");

  signal(SIGINT, tumgrd_signal_handler);
  signal(SIGTERM, tumgrd_signal_handler);

  try2(tumgrd_db_open(&g_db, db_path), "[main] open db failed: %s", db_path);
  db_opened = true;
  try2(tumgrd_db_init_schema(&g_db), "[main] init schema failed");

  g_ubus = try2_p(ubus_connect(NULL), "[main] ubus_connect failed");
  ubus_add_uloop(g_ubus);

  try2(tumgrd_ubus_init(g_ubus, &g_db), "[main] tumgrd_ubus_init failed");

  g_reconcile_timer.cb = tumgrd_reconcile_timer_cb;
  uloop_timeout_set(&g_reconcile_timer, TUMGRD_STARTUP_RECONCILE_DELAY_MS);

  log_info("[main] tumgrd started");
  uloop_run();

  err = 0;

err_cleanup:
  tumgrd_ubus_cleanup();
  if (g_ubus) {
    ubus_free(g_ubus);
    g_ubus = NULL;
  }

  uloop_timeout_cancel(&g_reconcile_timer);

  if (db_opened) {
    tumgrd_db_close(&g_db);
    db_opened = false;
  }

  uloop_done();
  return err;
}
