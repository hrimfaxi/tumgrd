#pragma once

#include "db.h"

int tumgrd_reconcile_one(struct tumgrd_db *db, struct tumgrd_node *node);
int tumgrd_reconcile_all(struct tumgrd_db *db);
