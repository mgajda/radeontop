/*
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Portable sysfs-based sensor readers for amdgpu.
 *
 * Probes several paths in preference order so the code works across:
 *   - Upstream kernels (current and future)
 *   - Experimental kernel patches exposing extra amdgpu status
 *   - Out-of-tree helper modules (amdgpu_throttle_whitelist)
 *
 * If a path is not found, the feature is silently disabled.
 */

#include "radeontop.h"
#include <dirent.h>

unsigned int has_throttle_sensor = 0;
unsigned int has_se_sensors = 0;
unsigned int has_ecc = 0;

static char throttle_path[320] = "";
static char se0_path[320] = "";
static char se1_path[320] = "";
static char ras_path[320] = "";

// Candidate locations, tried in order. NULL-terminated.
#define CARD_DEV "/sys/class/drm/card%d/device"
#define WHITELIST "/sys/module/amdgpu_throttle_whitelist/amdgpu_throttle_whitelist"

static int read_u32(const char *path, uint32_t *out) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	unsigned int v = 0;
	int r = fscanf(f, "%i", (int *)&v);	// %i handles 0x... or decimal
	fclose(f);
	if (r != 1) return -1;
	*out = v;
	return 0;
}

static int file_exists(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	fclose(f);
	return 1;
}

// Try each candidate printf format with all card indices, return first match.
static int probe_card_path(const char *const candidates[], char *out, size_t outsz) {
	for (int i = 0; candidates[i] != NULL; i++) {
		for (int card = 0; card < 8; card++) {
			char path[320];
			snprintf(path, sizeof(path), candidates[i], card);
			if (file_exists(path)) {
				snprintf(out, outsz, "%s", path);
				return 0;
			}
		}
	}
	return -1;
}

static int probe_fixed_path(const char *const candidates[], char *out, size_t outsz) {
	for (int i = 0; candidates[i] != NULL; i++) {
		if (file_exists(candidates[i])) {
			snprintf(out, outsz, "%s", candidates[i]);
			return 0;
		}
	}
	return -1;
}

static int find_ras_dir(char *out, size_t outsz) {
	for (int card = 0; card < 8; card++) {
		char features[128];
		snprintf(features, sizeof(features),
			CARD_DEV "/ras/features", card);
		uint32_t f = 0;
		if (read_u32(features, &f) == 0 && f != 0) {
			snprintf(out, outsz, CARD_DEV "/ras", card);
			return 0;
		}
	}
	return -1;
}

void init_sysfs_whitelist(void) {
	// Throttle-active sensor. Prefer card sysfs, fall back to helper module.
	const char *const throttle_candidates[] = {
		CARD_DEV "/gpu_throttle",			// hypothetical upstream path
		WHITELIST "/throttle_active",		// helper module
		NULL
	};
	if (probe_card_path(throttle_candidates, throttle_path, sizeof(throttle_path)) != 0) {
		// Second pass for fixed (non-card) paths
		const char *const fixed[] = {
			WHITELIST "/throttle_active",
			NULL
		};
		probe_fixed_path(fixed, throttle_path, sizeof(throttle_path));
	}
	has_throttle_sensor = (throttle_path[0] != '\0');

	// Per-shader-engine load
	const char *const se0_fixed[] = { WHITELIST "/grbm_status_se0", NULL };
	const char *const se1_fixed[] = { WHITELIST "/grbm_status_se1", NULL };
	probe_fixed_path(se0_fixed, se0_path, sizeof(se0_path));
	probe_fixed_path(se1_fixed, se1_path, sizeof(se1_path));
	has_se_sensors = (se0_path[0] != '\0');

	// ECC support via standard upstream RAS sysfs
	if (find_ras_dir(ras_path, sizeof(ras_path)) == 0)
		has_ecc = 1;
}

int get_throttle_sysfs(uint32_t *out) {
	if (!has_throttle_sensor) {
		*out = 0;
		return -1;
	}
	uint32_t v = 0;
	if (read_u32(throttle_path, &v) != 0) {
		*out = 0;
		return -1;
	}
	// throttle_active is a boolean; throttle_register has bit 20 as active.
	*out = (v & (1U << 20)) ? 1 : (v ? 1 : 0);
	return 0;
}

int get_grbm_se0_sysfs(uint32_t *out) {
	if (se0_path[0] == '\0') { *out = 0; return -1; }
	return read_u32(se0_path, out);
}

int get_grbm_se1_sysfs(uint32_t *out) {
	if (se1_path[0] == '\0') { *out = 0; return -1; }
	return read_u32(se1_path, out);
}

int get_ecc_errors_sysfs(uint64_t *out) {
	if (!has_ecc || ras_path[0] == '\0') {
		*out = 0;
		return -1;
	}

	// Upstream amdgpu ECC counters: <block>_err_count files contain:
	//   ue: <uncorrectable>
	//   ce: <correctable>
	// Sum UE across all RAS blocks (uncorrectable errors are the critical ones).
	DIR *d = opendir(ras_path);
	if (!d) return -1;

	uint64_t total_ue = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		const char *name = ent->d_name;
		size_t len = strlen(name);
		if (len < 10 || strcmp(name + len - 10, "_err_count") != 0)
			continue;

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", ras_path, name);
		FILE *f = fopen(path, "r");
		if (!f) continue;

		char line[128];
		while (fgets(line, sizeof(line), f)) {
			unsigned long long v = 0;
			if (sscanf(line, "ue: %llu", &v) == 1)
				total_ue += v;
		}
		fclose(f);
	}
	closedir(d);

	*out = total_ue;
	return 0;
}
