/**
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the LGPL License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 *
 * (C) Copyright 2016 Intel Corporation.
 */
/*
 * dsms: Pool Operations
 *
 * This file contains the server API methods and the RPC handlers that are both
 * related pool metadata.
 */

#include <daos_srv/daos_m_srv.h>
#include <uuid/uuid.h>
#include <daos/daos_transport.h>
#include "dsm_rpc.h"
#include "dsms_internal.h"
#include "dsms_storage.h"

int
dsms_pool_create(const uuid_t pool_uuid, const char *path, uuid_t target_uuid)
{
	return 0;
}

int
dsms_hdlr_pool_connect(dtp_rpc_t *rpc)
{
	return 0;
}

int
dsms_hdlr_pool_disconnect(dtp_rpc_t *rpc)
{
	return 0;
}