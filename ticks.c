/*
    Copyright (C) 2012 Lauri Kasanen

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

#include "radeontop.h"
#include <pthread.h>

struct bits_t *results = NULL;

struct collector_args_t {
	unsigned int ticks;
	unsigned int dumpinterval;
};

static void *collector(void *arg) {
	struct collector_args_t *args = (struct collector_args_t *) arg;

	const unsigned int ticks = args->ticks;
	const unsigned int dumpinterval = args->dumpinterval;

	struct bits_t res[2];

	// Save one second's worth of history
	struct bits_t *history = calloc(ticks * dumpinterval, sizeof(struct bits_t));
	unsigned int cur = 0, curres = 0;

	const useconds_t sleeptime = 1e6 / ticks;

	while (1) {
		unsigned int stat;
		getgrbm(&stat);
		unsigned int uvd;
		if (bits.uvd) getsrbm(&uvd);
		unsigned int srbm2;
		// Only read SRBM_STATUS2 if we actually have a register-based VCN
		// source; on RDNA the upstream vcn_busy_percent sysfs supersedes
		// the unreliable SRBM_STATUS2 bit (which returns all-ones on
		// unwhitelisted reads and causes a spurious 100%).
		if ((bits.vce0 || bits.vcn) && !has_vcn_busy_sysfs) getsrbm2(&srbm2);

		memset(&history[cur], 0, sizeof(struct bits_t));

		if (stat & bits.ee) history[cur].ee = 1;
		if (stat & bits.vgt) history[cur].vgt = 1;
		if (stat & bits.gui) history[cur].gui = 1;
		if (stat & bits.ta) history[cur].ta = 1;
		if (stat & bits.tc) history[cur].tc = 1;
		if (stat & bits.sx) history[cur].sx = 1;
		if (stat & bits.sh) history[cur].sh = 1;
		if (stat & bits.spi) history[cur].spi = 1;
		if (stat & bits.smx) history[cur].smx = 1;
		if (stat & bits.sc) history[cur].sc = 1;
		if (stat & bits.pa) history[cur].pa = 1;
		if (stat & bits.db) history[cur].db = 1;
		if (stat & bits.cr) history[cur].cr = 1;
		if (stat & bits.cb) history[cur].cb = 1;
		if (uvd & bits.uvd) history[cur].uvd = 1;
		if (has_vcn_busy_sysfs) {
			// Upstream amdgpu reports VCN busy as percentage 0..100.
			// Treat any non-zero as "busy this sample" so the existing
			// aggregation produces "% of samples VCN was active",
			// consistent with the other bitfield metrics.
			uint32_t pct = 0;
			if (get_vcn_busy_sysfs(&pct) == 0 && pct > 0)
				history[cur].vcn = 1;
		} else if (bits.vce0 || bits.vcn) {
			if (srbm2 & bits.vce0) history[cur].vce0 = 1;
			if (srbm2 & bits.vcn) history[cur].vcn = 1;
		}
		getsclk(&history[cur].sclk);
		getmclk(&history[cur].mclk);
		gettemp(&history[cur].temperature);
		getpower(&history[cur].power);

		if (has_throttle_sensor) {
			uint32_t throttle = 0;
			get_throttle_sysfs(&throttle);
			history[cur].throttle = throttle;
		}
		if (has_se_sensors) {
			uint32_t se0_stat = 0, se1_stat = 0;
			get_grbm_se0_sysfs(&se0_stat);
			get_grbm_se1_sysfs(&se1_stat);
			history[cur].se0 = (se0_stat & bits.gui) ? 1 : 0;
			history[cur].se1 = (se1_stat & bits.gui) ? 1 : 0;
		}

		usleep(sleeptime);
		cur++;
		cur %= ticks * dumpinterval;

		// One second has passed, we have one sec's worth of data
		if (cur == 0) {
			unsigned int i;

			memset(&res[curres], 0, sizeof(struct bits_t));

			for (i = 0; i < ticks * dumpinterval; i++) {
				res[curres].ee += history[i].ee;
				res[curres].vgt += history[i].vgt;
				res[curres].gui += history[i].gui;
				res[curres].ta += history[i].ta;
				res[curres].tc += history[i].tc;
				res[curres].sx += history[i].sx;
				res[curres].sh += history[i].sh;
				res[curres].spi += history[i].spi;
				res[curres].smx += history[i].smx;
				res[curres].sc += history[i].sc;
				res[curres].pa += history[i].pa;
				res[curres].db += history[i].db;
				res[curres].cb += history[i].cb;
				res[curres].cr += history[i].cr;
				res[curres].uvd += history[i].uvd;
				res[curres].vce0 += history[i].vce0;
				res[curres].vcn += history[i].vcn;
				res[curres].mclk += history[i].mclk;
				res[curres].sclk += history[i].sclk;
				res[curres].temperature += history[i].temperature;
				res[curres].power += history[i].power;
				res[curres].throttle += history[i].throttle;
				res[curres].se0 += history[i].se0;
				res[curres].se1 += history[i].se1;
			}

			getvram(&res[curres].vram);
			getgtt(&res[curres].gtt);

			if (has_ecc)
				get_ecc_errors_sysfs(&res[curres].ecc_errors);

			// Atomically write it to the pointer
			__sync_bool_compare_and_swap(&results, results, &res[curres]);

			curres++;
			curres %= 2;
		}
	}

	return NULL;
}

void collect(unsigned int ticks, unsigned int dumpinterval) {

	// Start a thread collecting data
	pthread_t tid;
	pthread_attr_t attr;

	// We don't care to join this thread
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	struct collector_args_t *args = malloc(sizeof(*args));
	args->ticks = ticks;
	args->dumpinterval = dumpinterval;

	pthread_create(&tid, &attr, collector, args);
}
