#pragma once

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* Small INI reader (GetPrivateProfileString stand-in). Used on Linux where
 * there is no Win32 profile API; Windows loader still uses the native one. */

static inline void Vellum_IniTrim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		memmove(s, s + 1, strlen(s));
	}
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
		*--e = '\0';
	}
}

static inline int Vellum_IniGet(const char *path, const char *section, const char *key,
                                char *out, size_t outSize, const char *defVal)
{
	FILE *f;
	char line[1024];
	char want[128];
	int in_section = 0;

	if (out == NULL || outSize == 0) {
		return 0;
	}
	out[0] = '\0';
	if (path == NULL || section == NULL || key == NULL) {
		goto use_def;
	}

	f = fopen(path, "r");
	if (f == NULL) {
		goto use_def;
	}

	snprintf(want, sizeof(want), "[%s]", section);

	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq;
		Vellum_IniTrim(line);
		if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			in_section = (strcmp(line, want) == 0);
			continue;
		}
		if (!in_section) {
			continue;
		}
		eq = strchr(line, '=');
		if (eq == NULL) {
			continue;
		}
		*eq = '\0';
		Vellum_IniTrim(line);
		if (strcmp(line, key) != 0) {
			continue;
		}
		Vellum_IniTrim(eq + 1);
		strncpy(out, eq + 1, outSize - 1);
		out[outSize - 1] = '\0';
		fclose(f);
		return 1;
	}
	fclose(f);

use_def:
	if (defVal != NULL) {
		strncpy(out, defVal, outSize - 1);
		out[outSize - 1] = '\0';
	}
	return 0;
}
