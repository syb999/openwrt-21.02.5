/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef POND_OAM_AGENT_H
#define POND_OAM_AGENT_H

#include "oam_config.h"

/* Run the EPON OAM agent on `interface` (mode epon-*). 0 on success. */
int oam_run_agent(const char *interface, const struct oam_config *cfg,
		  const char *socket_path);

#endif
