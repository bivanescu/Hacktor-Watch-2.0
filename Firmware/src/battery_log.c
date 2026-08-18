#include "battery_log.h"

#include <errno.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define FUEL_GAUGE_NODE DT_NODELABEL(fuel_gauge)

/*
 * Registers are read straight off the bus rather than through the Zephyr
 * fuel gauge driver, for two reasons:
 *
 *   - max17048_percent() returns `data / 256`, throwing away the low byte of
 *     the state of charge. One percent of this cell is 2 mAh, which is several
 *     minutes of idle — far too coarse to compare one power fix against the
 *     next. The raw register carries 1/256 %, or 0.0156 mAh.
 *   - the driver reads CRATE but only ever exposes it as a time-to-empty in
 *     whole minutes. Read directly it is a discharge rate, which is the
 *     closest thing to a current meter this board has.
 *
 * Talking to the chip directly also means the log keeps working when the
 * driver failed to initialize, which is exactly when it is most interesting.
 */
#define REG_VCELL 0x02
#define REG_SOC   0x04
#define REG_CRATE 0x16

/* Cell fitted on the board. Only used to turn percentages into mAh. */
#define BATTERY_CAPACITY_MAH 200

/* Datasheet Table 2: CRATE is 0.208 %/hr per LSB. Against a 200 mAh cell that
 * is 0.416 mA, held here as microamps so the arithmetic stays integer. */
#define CRATE_UA_PER_LSB ((208 * BATTERY_CAPACITY_MAH) / 100)

/* Full charge in the units of REG_SOC: 100 % at 1/256 % per LSB. */
#define SOC_FULL_Q8 25600

#define SAMPLE_SLOTS     512
#define PERIOD_MIN_S     10U
#define PERIOD_MAX_S     3600U
#define PERIOD_DEFAULT_S 60U

#define LOG_THREAD_STACK_SIZE 1536
#define LOG_THREAD_PRIORITY   10

struct battery_sample {
	uint32_t uptime_s;
	uint16_t soc_q8;
	uint16_t millivolts;
	uint16_t screen_on_s; /* lit time within the interval ending at this sample */
	int16_t crate_raw;
};

static const struct i2c_dt_spec fuel_gauge = I2C_DT_SPEC_GET(FUEL_GAUGE_NODE);

static K_MUTEX_DEFINE(log_mutex);
K_THREAD_STACK_DEFINE(log_thread_stack, LOG_THREAD_STACK_SIZE);
static struct k_thread log_thread_data;

static struct battery_sample ring[SAMPLE_SLOTS];
static uint32_t ring_head;
static uint32_t ring_count;

/*
 * The ring is allowed to overwrite its oldest entries, but the very first
 * sample is kept aside so a long run can still be summarised end to end after
 * the detail has rolled off.
 */
static struct battery_sample anchor;
static struct battery_sample latest;
static bool anchor_valid;

/*
 * Running totals, kept outside the ring so they survive rollover. Intervals
 * are split by whether the screen came on at all, which is what lets the two
 * currents be told apart.
 */
static uint64_t total_ms;
static int32_t total_drop_q8;
static uint64_t idle_ms;
static int32_t idle_drop_q8;
static uint64_t screen_ms;
static uint32_t charging_skips;
static uint32_t read_errors;

static uint32_t period_s = PERIOD_DEFAULT_S;

/* Screen accounting is written from the display thread and read from the
 * sampler, so it gets its own lock; the sections are a few lines each. */
static struct k_spinlock screen_lock;
static bool screen_lit;
static int64_t screen_since_ms;
static uint32_t screen_accum_ms;

void battery_log_display_changed(bool active)
{
	const int64_t now = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&screen_lock);

	if (active && !screen_lit) {
		screen_lit = true;
		screen_since_ms = now;
	} else if (!active && screen_lit) {
		screen_accum_ms += (uint32_t)(now - screen_since_ms);
		screen_lit = false;
	}

	k_spin_unlock(&screen_lock, key);
}

static uint32_t take_screen_ms(void)
{
	const int64_t now = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&screen_lock);
	uint32_t ms = screen_accum_ms;

	/* A screen that is still lit contributes the part elapsed so far, and
	 * the clock restarts here so the next interval is not double counted. */
	if (screen_lit) {
		ms += (uint32_t)(now - screen_since_ms);
		screen_since_ms = now;
	}

	screen_accum_ms = 0;
	k_spin_unlock(&screen_lock, key);

	return ms;
}

static int read_reg(uint8_t reg, uint16_t *value)
{
	uint8_t buf[2];
	const int ret = i2c_write_read_dt(&fuel_gauge, &reg, sizeof(reg), buf, sizeof(buf));

	if (ret < 0) {
		return ret;
	}

	*value = sys_get_be16(buf);
	return 0;
}

static int read_sample(struct battery_sample *out)
{
	uint16_t soc;
	uint16_t vcell;
	uint16_t crate;
	int ret;

	ret = read_reg(REG_SOC, &soc);
	if (ret < 0) {
		return ret;
	}

	ret = read_reg(REG_VCELL, &vcell);
	if (ret < 0) {
		return ret;
	}

	ret = read_reg(REG_CRATE, &crate);
	if (ret < 0) {
		return ret;
	}

	out->uptime_s = (uint32_t)(k_uptime_get() / 1000);
	out->soc_q8 = soc;
	/* 78.125 uV per LSB, which is exactly 5/64 mV. */
	out->millivolts = (uint16_t)(((uint32_t)vcell * 5U) / 64U);
	out->crate_raw = (int16_t)crate;
	out->screen_on_s = 0U;

	return 0;
}

static int64_t soc_delta_to_uah(int32_t delta_q8)
{
	return ((int64_t)delta_q8 * BATTERY_CAPACITY_MAH * 1000) / SOC_FULL_Q8;
}

static int32_t average_ua(int32_t delta_q8, uint64_t ms)
{
	if (ms == 0U) {
		return 0;
	}

	return (int32_t)((soc_delta_to_uah(delta_q8) * 3600000) / (int64_t)ms);
}

static void accumulate(const struct battery_sample *prev, const struct battery_sample *now)
{
	const uint64_t interval_ms = (uint64_t)(now->uptime_s - prev->uptime_s) * 1000ULL;
	const int32_t drop = (int32_t)prev->soc_q8 - (int32_t)now->soc_q8;

	if (interval_ms == 0U) {
		return;
	}

	/* Charging, or a gauge re-estimate that pushed the reading back up,
	 * makes the interval useless for a discharge figure. */
	if (drop < 0) {
		charging_skips++;
		return;
	}

	total_ms += interval_ms;
	total_drop_q8 += drop;
	screen_ms += (uint64_t)now->screen_on_s * 1000ULL;

	if (now->screen_on_s == 0U) {
		idle_ms += interval_ms;
		idle_drop_q8 += drop;
	}
}

static void store(const struct battery_sample *sample)
{
	k_mutex_lock(&log_mutex, K_FOREVER);

	if (!anchor_valid) {
		anchor = *sample;
		anchor_valid = true;
	} else {
		accumulate(&latest, sample);
	}

	latest = *sample;

	ring[ring_head] = *sample;
	ring_head = (ring_head + 1U) % SAMPLE_SLOTS;
	if (ring_count < SAMPLE_SLOTS) {
		ring_count++;
	}

	k_mutex_unlock(&log_mutex);
}

/* Index 0 is the oldest entry still held. Call with the mutex taken. */
static const struct battery_sample *ring_at(uint32_t index)
{
	const uint32_t start = (ring_count == SAMPLE_SLOTS) ? ring_head : 0U;

	return &ring[(start + index) % SAMPLE_SLOTS];
}

static void log_thread_entry(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/*
	 * Nothing in this thread may print. It keeps running with USB unplugged,
	 * which is the whole point of the log, and the console blocks when no
	 * host is draining it — a stray printk here would hang the sampler and
	 * take the measurement with it.
	 */
	while (1) {
		struct battery_sample sample;

		k_sleep(K_SECONDS(period_s));

		if (read_sample(&sample) < 0) {
			read_errors++;
			/* Leave the screen time banked; it belongs to whichever
			 * interval eventually closes. */
			continue;
		}

		sample.screen_on_s = (uint16_t)MIN(take_screen_ms() / 1000U, (uint32_t)UINT16_MAX);
		store(&sample);
	}
}

int battery_log_init(void)
{
	if (!device_is_ready(fuel_gauge.bus)) {
		return -ENODEV;
	}

	k_thread_create(&log_thread_data, log_thread_stack,
			K_THREAD_STACK_SIZEOF(log_thread_stack), log_thread_entry, NULL, NULL,
			NULL, LOG_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&log_thread_data, "batlog");

	return 0;
}

/* Shell */

static void print_soc(const struct shell *sh, const char *label, uint16_t soc_q8)
{
	shell_print(sh, "%-16s %u.%02u %%", label, soc_q8 / 256U,
		    ((soc_q8 % 256U) * 100U) / 256U);
}

static void print_duration(const struct shell *sh, const char *label, uint64_t ms)
{
	const uint64_t secs = ms / 1000ULL;

	shell_print(sh, "%-16s %lluh %02llum %02llus", label, secs / 3600ULL,
		    (secs / 60ULL) % 60ULL, secs % 60ULL);
}

static void print_current(const struct shell *sh, const char *label, int32_t ua)
{
	shell_print(sh, "%-16s %d.%03d mA", label, ua / 1000, abs(ua) % 1000);
}

static int cmd_now(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t soc;
	uint16_t vcell;
	uint16_t crate;
	int32_t ua;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = read_reg(REG_SOC, &soc);
	if (ret == 0) {
		ret = read_reg(REG_VCELL, &vcell);
	}
	if (ret == 0) {
		ret = read_reg(REG_CRATE, &crate);
	}
	if (ret < 0) {
		shell_error(sh, "fuel gauge read failed: %d", ret);
		return ret;
	}

	ua = (int32_t)(int16_t)crate * CRATE_UA_PER_LSB;

	print_soc(sh, "charge", soc);
	shell_print(sh, "%-16s %u mV", "cell", ((uint32_t)vcell * 5U) / 64U);
	shell_print(sh, "%-16s %d LSB (%d.%03d %%/hr)", "crate", (int16_t)crate,
		    ((int16_t)crate * 208) / 1000, abs((int16_t)crate * 208) % 1000);
	print_current(sh, ua < 0 ? "draw" : "charge rate", ua < 0 ? -ua : ua);
	shell_print(sh, "");
	shell_print(sh, "CRATE is a ModelGauge estimate, not a shunt reading. It needs a few");
	shell_print(sh, "minutes to settle and resolves ~0.4 mA per LSB, so treat it as a");
	shell_print(sh, "quick check and use 'batlog stat' for the number you trust.");

	return 0;
}

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	int32_t idle_ua;
	int32_t total_ua;
	uint64_t elapsed_ms;
	int32_t drop_q8;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&log_mutex, K_FOREVER);

	if (!anchor_valid || ring_count < 2U) {
		k_mutex_unlock(&log_mutex);
		shell_print(sh, "Not enough samples yet (period is %u s).", period_s);
		return 0;
	}

	elapsed_ms = (uint64_t)(latest.uptime_s - anchor.uptime_s) * 1000ULL;
	drop_q8 = (int32_t)anchor.soc_q8 - (int32_t)latest.soc_q8;
	idle_ua = average_ua(idle_drop_q8, idle_ms);
	total_ua = average_ua(total_drop_q8, total_ms);

	shell_print(sh, "samples          %u held, %u s period", ring_count, period_s);
	print_duration(sh, "span", elapsed_ms);
	print_soc(sh, "start", anchor.soc_q8);
	print_soc(sh, "now", latest.soc_q8);
	shell_print(sh, "%-16s %u mV", "cell", latest.millivolts);
	shell_print(sh, "%-16s %lld.%03llu mAh", "used",
		    soc_delta_to_uah(drop_q8) / 1000,
		    (unsigned long long)(llabs(soc_delta_to_uah(drop_q8)) % 1000));
	shell_print(sh, "");

	print_duration(sh, "screen lit", screen_ms);
	print_duration(sh, "measured", total_ms);
	print_current(sh, "average", total_ua);
	shell_print(sh, "");

	if (idle_ms == 0U) {
		shell_print(sh, "No screen-off interval yet, so idle current is unknown.");
		shell_print(sh, "Leave it alone for a few sample periods.");
	} else {
		print_duration(sh, "idle only", idle_ms);
		print_current(sh, "=> idle draw", idle_ua);
	}

	/*
	 * Screen-on current is what is left once the idle draw is charged
	 * against the dark part of the intervals that did have the screen lit.
	 * It only means anything once a fair amount of lit time has built up.
	 */
	if (idle_ms > 0U && screen_ms > 0U) {
		const uint64_t mixed_ms = total_ms - idle_ms;
		const int64_t mixed_uah = soc_delta_to_uah(total_drop_q8 - idle_drop_q8);
		const int64_t dark_uah =
			((int64_t)idle_ua * (int64_t)(mixed_ms - screen_ms)) / 3600000;
		const int64_t lit_uah = mixed_uah - dark_uah;

		if (mixed_ms >= screen_ms && screen_ms >= 60000ULL) {
			print_current(sh, "=> screen on",
				      (int32_t)((lit_uah * 3600000) / (int64_t)screen_ms));
		} else {
			shell_print(sh, "%-16s needs >1 min of lit time", "=> screen on");
		}
	}

	if (total_ua > 0) {
		const int64_t remaining_uah = soc_delta_to_uah((int32_t)latest.soc_q8);

		shell_print(sh, "");
		print_duration(sh, "left at this rate",
			       (uint64_t)((remaining_uah * 3600000) / total_ua));
	}

	if (charging_skips > 0U || read_errors > 0U) {
		shell_print(sh, "");
		shell_print(sh, "skipped %u charging intervals, %u read errors", charging_skips,
			    read_errors);
	}

	k_mutex_unlock(&log_mutex);
	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t wanted;
	uint32_t first;
	uint32_t i;

	k_mutex_lock(&log_mutex, K_FOREVER);

	wanted = ring_count;
	if (argc > 1) {
		wanted = MIN((uint32_t)atoi(argv[1]), ring_count);
	}
	first = ring_count - wanted;

	shell_print(sh, "%9s %8s %7s %7s %8s", "uptime_s", "soc_%", "mV", "lit_s", "crate");

	for (i = first; i < ring_count; i++) {
		const struct battery_sample *s = ring_at(i);

		shell_print(sh, "%9u %5u.%02u %7u %7u %8d", s->uptime_s, s->soc_q8 / 256U,
			    ((s->soc_q8 % 256U) * 100U) / 256U, s->millivolts, s->screen_on_s,
			    s->crate_raw);
	}

	k_mutex_unlock(&log_mutex);
	return 0;
}

static int cmd_csv(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t i;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&log_mutex, K_FOREVER);

	shell_print(sh, "uptime_s,soc_q8,millivolts,screen_on_s,crate_raw");
	for (i = 0; i < ring_count; i++) {
		const struct battery_sample *s = ring_at(i);

		shell_print(sh, "%u,%u,%u,%u,%d", s->uptime_s, s->soc_q8, s->millivolts,
			    s->screen_on_s, s->crate_raw);
	}

	k_mutex_unlock(&log_mutex);
	return 0;
}

static int cmd_period(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t value;

	if (argc < 2) {
		shell_print(sh, "period is %u s", period_s);
		return 0;
	}

	value = (uint32_t)atoi(argv[1]);
	if (value < PERIOD_MIN_S || value > PERIOD_MAX_S) {
		shell_error(sh, "period must be %u..%u s", PERIOD_MIN_S, PERIOD_MAX_S);
		return -EINVAL;
	}

	period_s = value;
	shell_print(sh, "period is %u s, effective after the current sleep", value);
	shell_print(sh, "%u slots covers %u h at this rate", SAMPLE_SLOTS,
		    (SAMPLE_SLOTS * value) / 3600U);

	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&log_mutex, K_FOREVER);

	ring_head = 0U;
	ring_count = 0U;
	anchor_valid = false;
	total_ms = 0U;
	total_drop_q8 = 0;
	idle_ms = 0U;
	idle_drop_q8 = 0;
	screen_ms = 0U;
	charging_skips = 0U;
	read_errors = 0U;

	k_mutex_unlock(&log_mutex);

	(void)take_screen_ms();
	shell_print(sh, "cleared");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	batlog_cmds, SHELL_CMD(now, NULL, "Instant charge, cell voltage and discharge rate",
			       cmd_now),
	SHELL_CMD(stat, NULL, "Summary: idle draw, screen-on draw, projected runtime", cmd_stat),
	SHELL_CMD_ARG(dump, NULL, "Sample table, optionally the last N: dump [n]", cmd_dump, 1, 1),
	SHELL_CMD(csv, NULL, "Samples as CSV for a spreadsheet", cmd_csv),
	SHELL_CMD_ARG(period, NULL, "Show or set the sample period in seconds", cmd_period, 1, 1),
	SHELL_CMD(clear, NULL, "Discard the log and start a fresh run", cmd_clear),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(batlog, &batlog_cmds, "Battery discharge log", NULL);
