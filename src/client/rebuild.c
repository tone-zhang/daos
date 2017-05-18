/**
 * (C) Copyright 2015, 2017 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
#define DD_SUBSYS	DD_FAC(client)

#include <daos/rebuild.h>
#include "client_internal.h"

int
daos_rebuild_tgt(uuid_t uuid, daos_rank_list_t *failed_list,
		 daos_event_t *ev)
{
	struct daos_task	*task;
	int			rc;

	rc = daos_client_task_prep(NULL, 0, &task, &ev);
	if (rc != 0)
		return rc;

	dc_rebuild_tgt(uuid, failed_list, task);

	return daos_client_result_wait(ev);
}

int
daos_rebuild_query(uuid_t uuid, daos_rank_list_t *failed_list,
		   int *done, int *status, daos_event_t *ev)
{
	struct daos_task	*task;
	int			rc;

	rc = daos_client_task_prep(NULL, 0, &task, &ev);
	if (rc != 0)
		return rc;

	dc_rebuild_query(uuid, failed_list, done, status, task);

	return daos_client_result_wait(ev);
}