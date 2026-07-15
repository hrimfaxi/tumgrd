#pragma once

#include "db.h"
#include <stdbool.h>

int tumgrd_reconcile_one(struct tumgrd_db *db, const struct tumgrd_config *cfg, struct tumgrd_node *node, bool force);

int tumgrd_reconcile_all(struct tumgrd_db *db, const struct tumgrd_config *cfg, bool force);
