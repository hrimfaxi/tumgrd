#include "db.h"
#include "reconcile.h"
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
  fprintf(stderr, "[main] received signal %d, exiting\n", signo);
  uloop_end();
}

int main(int argc, char **argv) {
  const char *db_path = TUMGRD_DB_PATH;
  int         rc      = 1;

  if (argc > 1 && argv[1] && argv[1][0] != '\0') {
    db_path = argv[1];
  }

  memset(&g_db, 0, sizeof(g_db));
  memset(&g_reconcile_timer, 0, sizeof(g_reconcile_timer));

  if (uloop_init() != 0) {
    fprintf(stderr, "[main] uloop_init failed\n");
    return 1;
  }

  signal(SIGINT, tumgrd_signal_handler);
  signal(SIGTERM, tumgrd_signal_handler);

  if (tumgrd_db_open(&g_db, db_path) != 0) {
    fprintf(stderr, "[main] open db failed: %s\n", db_path);
    goto out_uloop;
  }

  if (tumgrd_db_init_schema(&g_db) != 0) {
    fprintf(stderr, "[main] init schema failed\n");
    goto out_db;
  }

  g_ubus = ubus_connect(NULL);
  if (!g_ubus) {
    fprintf(stderr, "[main] ubus_connect failed\n");
    goto out_db;
  }

  ubus_add_uloop(g_ubus);

  if (tumgrd_ubus_init(g_ubus, &g_db) != 0) {
    fprintf(stderr, "[main] tumgrd_ubus_init failed\n");
    goto out_ubus;
  }

  g_reconcile_timer.cb = tumgrd_reconcile_timer_cb;
  uloop_timeout_set(&g_reconcile_timer, TUMGRD_STARTUP_RECONCILE_DELAY_MS);

  fprintf(stderr, "[main] tumgrd started\n");
  uloop_run();

  rc = 0;

out_ubus:
  tumgrd_ubus_cleanup();
  if (g_ubus) {
    ubus_free(g_ubus);
    g_ubus = NULL;
  }

out_db:
  uloop_timeout_cancel(&g_reconcile_timer);
  tumgrd_db_close(&g_db);

out_uloop:
  uloop_done();
  return rc;
}
