#pragma once

#include "db.h"

#include <libubus.h>

int  tumgrd_ubus_init(struct ubus_context *ubus, struct tumgrd_ctx *ctx);
void tumgrd_ubus_cleanup(void);
