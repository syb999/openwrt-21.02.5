/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal UCI config parser (port of airoha-pon-daemons config.rs). */
#ifndef POND_CONFIG_H
#define POND_CONFIG_H

#include <stddef.h>

#define POND_MAX_SECTIONS 32
#define POND_MAX_OPTIONS  48

struct pond_option {
	char name[64];
	char value[512];
};

struct pond_section {
	char name[64];
	char kind[32];
	struct pond_option options[POND_MAX_OPTIONS];
	int n_options;
};

struct pond_config {
	struct pond_section sections[POND_MAX_SECTIONS];
	int n_sections;
};

/* Returns 0 on success, negative errno on failure (message in errbuf). */
int pond_config_load(struct pond_config *cfg, const char *path,
		     char *errbuf, size_t errlen);

/* Find a section by name+kind; NULL if absent. */
const struct pond_section *pond_config_section(const struct pond_config *cfg,
					       const char *name, const char *kind);

/* Find the first section of `kind` whose "line" option equals `line`. */
const struct pond_section *pond_config_linked(const struct pond_config *cfg,
					      const char *kind, const char *line);

/* Option lookup (NULL when absent). */
const char *pond_section_option(const struct pond_section *s, const char *name);

#endif
