/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#pragma once

#include <postgres.h>

#include "export.h"

extern TSDLLEXPORT void ts_cagg_refresh_queue_insert(int32 materialization_id, int64 start_range,
													 int64 end_range, int32 job_id);
extern TSDLLEXPORT void
ts_cagg_refresh_queue_delete_by_mat_hypertable_id(int32 materialization_id);
