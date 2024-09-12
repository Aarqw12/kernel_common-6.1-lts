/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Google LWIS IOCTL Handler
 *
 * Copyright (c) 2024 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "lwis_ioctl.h"

extern struct cmd_transaction_submit_ops transaction_cmd_v5_ops;
extern struct cmd_transaction_submit_ops transaction_cmd_v4_ops;
