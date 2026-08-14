#include "menu.h"

#include <errno.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define ROW_HEIGHT      46
#define ROW_GAP         8
#define ROW_RADIUS      12
#define SIDE_PADDING    28
#define BACK_DIAMETER   40
#define BACK_MARGIN     6
#define ROW_BG_HEX      0x282828
#define ROW_TEXT_HEX    0xFFFFFF
#define HIT_MARGIN      28

/*
 * The application list. Add an entry here and it shows up in the menu; `open`
 * runs when the row is tapped, and may be NULL while an app is still a stub.
 */
struct menu_app {
	const char *name;
	void (*open)(void);
};

static const struct menu_app apps[] = {
	{ "Stopwatch", NULL },
	{ "Timer",     NULL },
	{ "Pasi",      NULL },
	{ "Baterie",   NULL },
	{ "Setari",    NULL },
	{ "Info",      NULL },
};

static lv_obj_t *menu_screen;
static lv_obj_t *home;
static bool menu_visible;
static lv_obj_t *probe_button;

static void probe_button_clicked(lv_event_t *event)
{
	ARG_UNUSED(event);

	printk("menu: PROBE BUTTON clicked\n");
}

/*
 * Touch calibration. Draws a target at a known place, waits for you to press
 * it, and reports what the controller said. Driven entirely from the LVGL
 * timer so the shell thread never touches LVGL state.
 */
#define CALIB_POINTS   4
#define CALIB_PHASE_MS 6000
#define CALIB_DOT_SIZE 22

static const lv_point_t calib_targets[CALIB_POINTS] = {
	{ 120, 40 }, { 200, 120 }, { 120, 200 }, { 40, 120 },
};

static volatile bool calib_request;
static bool calib_running;
static int calib_phase;
static uint32_t calib_phase_start;
static lv_obj_t *calib_dot;
static lv_obj_t *calib_shield;
static int32_t calib_sum_x, calib_sum_y;
static int calib_samples;

static void calib_show_target(void)
{
	if (calib_dot == NULL) {
		/*
		 * Full-screen catcher first, otherwise a press aimed at a dot
		 * that sits over a real control activates it — the left target
		 * lands squarely on the menu button.
		 */
		calib_shield = lv_obj_create(lv_layer_top());
		if (calib_shield == NULL) {
			return;
		}
		lv_obj_remove_style_all(calib_shield);
		lv_obj_set_size(calib_shield, LV_PCT(100), LV_PCT(100));
		lv_obj_set_style_bg_opa(calib_shield, LV_OPA_TRANSP, 0);
		lv_obj_add_flag(calib_shield, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(calib_shield, LV_OBJ_FLAG_SCROLLABLE);

		calib_dot = lv_obj_create(calib_shield);
		if (calib_dot == NULL) {
			return;
		}
		lv_obj_remove_style_all(calib_dot);
		lv_obj_set_size(calib_dot, CALIB_DOT_SIZE, CALIB_DOT_SIZE);
		lv_obj_set_style_radius(calib_dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(calib_dot, lv_color_hex(0xFF0000), 0);
		lv_obj_set_style_bg_opa(calib_dot, LV_OPA_COVER, 0);
	}

	lv_obj_set_pos(calib_dot,
		       calib_targets[calib_phase].x - (CALIB_DOT_SIZE / 2),
		       calib_targets[calib_phase].y - (CALIB_DOT_SIZE / 2));
	calib_sum_x = 0;
	calib_sum_y = 0;
	calib_samples = 0;
	calib_phase_start = lv_tick_get();

	printk("calib: press the dot (target %d,%d)\n",
	       (int)calib_targets[calib_phase].x, (int)calib_targets[calib_phase].y);
}

static void calib_finish_phase(void)
{
	if (calib_samples == 0) {
		printk("calib: target %d,%d -> nothing registered\n",
		       (int)calib_targets[calib_phase].x, (int)calib_targets[calib_phase].y);
	} else {
		printk("calib: target %d,%d -> reported %d,%d (%d samples)\n",
		       (int)calib_targets[calib_phase].x, (int)calib_targets[calib_phase].y,
		       (int)(calib_sum_x / calib_samples), (int)(calib_sum_y / calib_samples),
		       calib_samples);
	}

	calib_phase++;

	if (calib_phase >= CALIB_POINTS) {
		calib_running = false;
		if (calib_shield != NULL) {
			lv_obj_delete(calib_shield);
			calib_shield = NULL;
			calib_dot = NULL;
		}
		printk("calib: done\n");
		return;
	}

	calib_show_target();
}

static int cmd_calib(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	calib_request = true;
	shell_print(sh, "calibration queued; press each dot as it appears");

	return 0;
}
SHELL_CMD_REGISTER(calib, NULL, "Run touch calibration", cmd_calib);

static void row_clicked(lv_event_t *event)
{
	const struct menu_app *app = lv_event_get_user_data(event);

	if (app == NULL) {
		return;
	}

	if (app->open == NULL) {
		printk("menu: '%s' not implemented yet\n", app->name);
		return;
	}

	app->open();
}

/*
 * Screens must not be swapped from inside an event handler: the input device
 * is still holding the object that was pressed, and once that object is off
 * the active screen the indev stays latched to it and ignores everything
 * afterwards. Defer the swap until LVGL has finished processing the press.
 */
static void close_async(void *unused)
{
	ARG_UNUSED(unused);

	menu_close();
}

static void back_clicked(lv_event_t *event)
{
	ARG_UNUSED(event);

	lv_async_call(close_async, NULL);
}

static lv_obj_t *create_row(lv_obj_t *parent, const struct menu_app *app)
{
	lv_obj_t *row;
	lv_obj_t *label;

	row = lv_obj_create(parent);
	if (row == NULL) {
		return NULL;
	}

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
	lv_obj_set_style_bg_color(row, lv_color_hex(ROW_BG_HEX), 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	/* Visible feedback, so a tap that registers can be told from one that does not. */
	lv_obj_set_style_bg_color(row, lv_color_hex(0x0080FF), LV_STATE_PRESSED);
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)app);

	label = lv_label_create(row);
	if (label == NULL) {
		return NULL;
	}

	lv_label_set_text(label, app->name);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(ROW_TEXT_HEX), 0);
	lv_obj_center(label);

	return row;
}

static int create_back_button(lv_obj_t *parent)
{
	lv_obj_t *back;
	lv_obj_t *label;

	back = lv_obj_create(parent);
	if (back == NULL) {
		return -ENOMEM;
	}

	lv_obj_remove_style_all(back);
	lv_obj_set_size(back, BACK_DIAMETER, BACK_DIAMETER);
	lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(back, lv_color_hex(ROW_BG_HEX), 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
	lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -BACK_MARGIN);
	lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
	/* Same slack as the menu button: touches read low on x. */
	lv_obj_set_ext_click_area(back, HIT_MARGIN);
	lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

	label = lv_label_create(back);
	if (label == NULL) {
		return -ENOMEM;
	}

	lv_label_set_text(label, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_color(label, lv_color_hex(ROW_TEXT_HEX), 0);
	lv_obj_center(label);

	return 0;
}

int menu_init(lv_obj_t *home_screen)
{
	lv_obj_t *list;
	size_t i;
	int ret;

	home = home_screen;

	menu_screen = lv_obj_create(NULL);
	if (menu_screen == NULL) {
		printk("menu: screen allocation failed\n");
		return -ENOMEM;
	}

	lv_obj_set_style_bg_color(menu_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, 0);
	lv_obj_remove_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);

	list = lv_obj_create(menu_screen);
	if (list == NULL) {
		printk("menu: list allocation failed\n");
		return -ENOMEM;
	}

	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_pad_left(list, SIDE_PADDING, 0);
	lv_obj_set_style_pad_right(list, SIDE_PADDING, 0);
	lv_obj_set_style_pad_top(list, ROW_HEIGHT, 0);
	/* Leave room for the back button so the last row is not stuck under it. */
	lv_obj_set_style_pad_bottom(list, BACK_DIAMETER + (BACK_MARGIN * 2), 0);
	lv_obj_set_style_pad_row(list, ROW_GAP, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

	for (i = 0; i < ARRAY_SIZE(apps); i++) {
		if (create_row(list, &apps[i]) == NULL) {
			printk("menu: row allocation failed at %u\n", (unsigned int)i);
			return -ENOMEM;
		}
	}

	ret = create_back_button(menu_screen);
	if (ret < 0) {
		printk("menu: back button allocation failed\n");
		return ret;
	}

	/*
	 * Bisect aid: a plain target sitting straight on the menu screen rather
	 * than inside the scrollable list. If this reacts and the rows do not,
	 * the list is eating the presses; if neither reacts, the screen itself
	 * never sees input.
	 */
	probe_button = lv_obj_create(menu_screen);
	if (probe_button != NULL) {
		lv_obj_remove_style_all(probe_button);
		lv_obj_set_size(probe_button, 70, 70);
		lv_obj_center(probe_button);
		lv_obj_set_style_bg_color(probe_button, lv_color_hex(0x00A000), 0);
		lv_obj_set_style_bg_opa(probe_button, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(probe_button, lv_color_hex(0x00FF00), LV_STATE_PRESSED);
		lv_obj_add_flag(probe_button, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(probe_button, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(probe_button, probe_button_clicked, LV_EVENT_CLICKED, NULL);
	}

	return 0;
}

void menu_open(void)
{
	if (menu_screen == NULL || menu_visible) {
		return;
	}

	lv_screen_load(menu_screen);
	menu_visible = true;
}

void menu_close(void)
{
	if (home == NULL || !menu_visible) {
		return;
	}

	lv_screen_load(home);
	menu_visible = false;
}

bool menu_is_open(void)
{
	return menu_visible;
}
