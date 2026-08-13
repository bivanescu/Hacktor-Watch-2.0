#include "menu.h"

#include <errno.h>
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

static void screen_pressed(lv_event_t *event)
{
	printk("menu: screen got press, target=%p\n", (void *)lv_event_get_target(event));
}

/* Temporary: reports what LVGL believes the pointer is doing, a few times a
 * second, so we can tell "no touches arrive" apart from "touches arrive but
 * land somewhere unexpected".
 */
static void probe_timer_cb(lv_timer_t *timer)
{
	lv_indev_t *indev = lv_indev_get_next(NULL);
	lv_point_t point = { -1, -1 };
	lv_indev_state_t state;
	lv_obj_t *act_obj;

	ARG_UNUSED(timer);

	if (indev == NULL) {
		printk("probe: no input device registered\n");
		return;
	}

	state = lv_indev_get_state(indev);
	lv_indev_get_point(indev, &point);
	act_obj = lv_indev_get_active_obj();

	printk("probe: state=%s xy=%d,%d screen=%p menu=%p act_obj=%p menu_open=%d\n",
	       state == LV_INDEV_STATE_PRESSED ? "PRESSED" : "released",
	       (int)point.x, (int)point.y,
	       (void *)lv_screen_active(), (void *)menu_screen,
	       (void *)act_obj, (int)menu_visible);
}

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

	lv_obj_add_event_cb(menu_screen, screen_pressed, LV_EVENT_PRESSED, NULL);
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

	lv_timer_create(probe_timer_cb, 400, NULL);

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
