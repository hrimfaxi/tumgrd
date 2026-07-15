#pragma once

#include "db.h"

int tumgrd_runner_server_add(const struct tumgrd_node *node, const struct tumgrd_config *cfg, const char *current_ip);

int tumgrd_runner_server_del(const struct tumgrd_node *node);

int tumgrd_runner_client_add(const struct tumgrd_node *node);
int tumgrd_runner_client_del(const struct tumgrd_node *node);
int tumgrd_runner_reset_local_client(const struct tumgrd_node *node);
