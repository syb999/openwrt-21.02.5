/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MAX_FIELDS 8
#define MAX_FIELD  512

/*
 * Split one UCI statement honouring single/double quotes and backslash escapes.
 * Returns the field count, or -1 on unterminated quoting.
 */
static int split_uci_line(const char *line, char fields[MAX_FIELDS][MAX_FIELD])
{
	int nf = 0, fi = 0, started = 0, escaped = 0;
	char quote = 0;

	for (const char *p = line; *p; p++) {
		char c = *p;

		if (escaped) {
			if (nf < MAX_FIELDS && fi < MAX_FIELD - 1)
				fields[nf][fi++] = c;
			started = escaped = 0;
			continue;
		}
		if (c == '\\' && quote != '\'') {
			escaped = 1;
			continue;
		}
		if (quote) {
			if (c == quote)
				quote = 0;
			else if (nf < MAX_FIELDS && fi < MAX_FIELD - 1)
				fields[nf][fi++] = c;
			continue;
		}
		if (c == '\'' || c == '"') {
			quote = c;
			started = 1;
		} else if (c == '#') {
			break;
		} else if (isspace((unsigned char)c)) {
			if (started) {
				fields[nf][fi] = 0;
				nf++;
				fi = 0;
				started = 0;
			}
		} else {
			if (nf < MAX_FIELDS && fi < MAX_FIELD - 1)
				fields[nf][fi++] = c;
			started = 1;
		}
	}
	if (escaped || quote)
		return -1;
	if (started) {
		fields[nf][fi] = 0;
		nf++;
	}
	return nf;
}

static struct pond_option *find_option(struct pond_section *s, const char *name)
{
	for (int i = 0; i < s->n_options; i++)
		if (!strcmp(s->options[i].name, name))
			return &s->options[i];
	return NULL;
}

int pond_config_load(struct pond_config *cfg, const char *path,
		     char *errbuf, size_t errlen)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	int lineno = 0;

	memset(cfg, 0, sizeof(*cfg));
	if (!f) {
		snprintf(errbuf, errlen, "cannot open %s: %s", path, strerror(errno));
		return -errno;
	}
	while (fgets(line, sizeof(line), f)) {
		char fields[MAX_FIELDS][MAX_FIELD];
		int nf;
		struct pond_section *s;

		lineno++;
		nf = split_uci_line(line, fields);
		if (nf < 0) {
			snprintf(errbuf, errlen, "line %d: unterminated quoted value", lineno);
			fclose(f);
			return -EINVAL;
		}
		if (!nf)
			continue;
		if (!strcmp(fields[0], "config") && nf == 3) {
			if (cfg->n_sections >= POND_MAX_SECTIONS) {
				snprintf(errbuf, errlen, "too many sections");
				fclose(f);
				return -E2BIG;
			}
			s = &cfg->sections[cfg->n_sections++];
			snprintf(s->kind, sizeof(s->kind), "%s", fields[1]);
			snprintf(s->name, sizeof(s->name), "%s", fields[2]);
			s->n_options = 0;
		} else if ((!strcmp(fields[0], "option") || !strcmp(fields[0], "list")) && nf == 3) {
			struct pond_option *o;

			if (!cfg->n_sections) {
				snprintf(errbuf, errlen, "line %d: %s appears before config",
					 lineno, fields[0]);
				fclose(f);
				return -EINVAL;
			}
			s = &cfg->sections[cfg->n_sections - 1];
			o = find_option(s, fields[1]);
			if (o && !strcmp(fields[0], "list")) {
				size_t used = strlen(o->value);

				if (used + 1 + strlen(fields[2]) < sizeof(o->value)) {
					o->value[used] = ' ';
					snprintf(o->value + used + 1,
						 sizeof(o->value) - used - 1, "%s", fields[2]);
				}
			} else if (!o) {
				if (s->n_options >= POND_MAX_OPTIONS) {
					snprintf(errbuf, errlen, "too many options");
					fclose(f);
					return -E2BIG;
				}
				o = &s->options[s->n_options++];
				snprintf(o->name, sizeof(o->name), "%s", fields[1]);
				snprintf(o->value, sizeof(o->value), "%s", fields[2]);
			}
		} else {
			snprintf(errbuf, errlen, "line %d: invalid UCI statement", lineno);
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	return 0;
}

const char *pond_section_option(const struct pond_section *s, const char *name)
{
	for (int i = 0; i < s->n_options; i++)
		if (!strcmp(s->options[i].name, name))
			return s->options[i].value;
	return NULL;
}

const struct pond_section *pond_config_section(const struct pond_config *cfg,
					       const char *name, const char *kind)
{
	for (int i = 0; i < cfg->n_sections; i++) {
		const struct pond_section *s = &cfg->sections[i];

		if (!strcmp(s->name, name) && !strcmp(s->kind, kind))
			return s;
	}
	return NULL;
}

const struct pond_section *pond_config_linked(const struct pond_config *cfg,
					      const char *kind, const char *line)
{
	for (int i = 0; i < cfg->n_sections; i++) {
		const struct pond_section *s = &cfg->sections[i];
		const char *l;

		if (strcmp(s->kind, kind))
			continue;
		l = pond_section_option(s, "line");
		if (l && !strcmp(l, line))
			return s;
	}
	return NULL;
}
