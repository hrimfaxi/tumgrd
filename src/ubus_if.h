#pragma once

#include "db.h"

#include <libubus.h>

int  tumgrd_ubus_init(struct ubus_context *ctx, struct tumgrd_db *db);
void tumgrd_ubus_cleanup(void);
