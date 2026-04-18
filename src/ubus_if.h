#pragma once

#include <libubus.h>

struct tumgrd_ctx;
int  tumgrd_ubus_init(struct tumgrd_ctx *ctx);
void tumgrd_ubus_cleanup(struct tumgrd_ctx *ctx);
