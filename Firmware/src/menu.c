#include "menu.h"

#include <errno.h>
#include <zephyr/sys/util.h>

#define ROW_WIDTH    140
#define ROW_HEIGHT   38
#define ROW_PITCH    44
#define ROW_X_OFFSET (-18)
#define ROW_RADIUS   10
#define BAND_TOP     45
#define BAND_HEIGHT  140
#define RAIL_WIDTH   14
#define RAIL_X       80
#define RAIL_TOP     55
#define RAIL_HEIGHT  130
#define THUMB_MIN    30
#define BACK_SIZE    36
#define BACK_BOTTOM  4
#define BACK_HIT_MARGIN 20
#define RAIL_HIT_MARGIN 24
#define BG_HEX       0x282828
#define ON_HEX       0x0080FF
#define RAIL_BG_HEX  0x1A1A1A
#define THUMB_HEX    0x5A5A5A
#define TEXT_HEX     0xFFFFFF

/*
 * Grown one step at a time, each step confirmed on hardware before the next.
 * An earlier attempt built the whole thing at once — scrollable list, flex
 * layout, six rows — never registered a touch, and gave no way to tell which
 * layer was at fault.
 *
 * Rows sit at fixed positions straight on the screen and are moved by hand.
 * LVGL's own scrollable container swallows every press on this hardware: the
 * panel reports contact and release with little movement between, so a press
 * is held as the start of a drag that never arrives and never reaches the row.
 *
 * Scrolling therefore lives on a dedicated rail down the side, which reads an
 * absolute position rather than a gesture. Touch it anywhere and the list
 * jumps there; drag along it and the list follows. Both work with press-only
 * input, and taps on rows stay unambiguous because the rail is the only thing
 * that scrolls.
 *
 * Nothing here may print from an LVGL callback. The USB console blocks when
 * the host is not draining it, and these run on the UI thread while it holds
 * the LVGL lock, so a stray printk freezes the interface.
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

#define APP_COUNT      ARRAY_SIZE(apps)
#define CONTENT_HEIGHT ((int32_t)APP_COUNT * ROW_PITCH - (ROW_PITCH - ROW_HEIGHT))
#define MAX_SCROLL     (CONTENT_HEIGHT - BAND_HEIGHT)

static lv_obj_t *menu_screen;
static lv_obj_t *home;
static lv_obj_t *rows[APP_COUNT];
static lv_obj_t *rail;
static lv_obj_t *thumb;
static bool menu_visible;
static int32_t scroll_px;

/*
 * Screens must not be swapped from inside an event handler: the input device
 * is still holding the pressed object, and once that object is off the active
 * screen the indev stays latched to it. Defer until LVGL is done with the press.
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

static void row_clicked(lv_event_t *event)
{
	const struct menu_app *app = lv_event_get_user_data(event);

	if (app != NULL && app->open != NULL) {
		app->open();
	}
}

static void apply_scroll(void)
{
	int32_t thumb_height = MAX(THUMB_MIN, (RAIL_HEIGHT * BAND_HEIGHT) / CONTENT_HEIGHT);
	int32_t travel = RAIL_HEIGHT - thumb_height;
	size_t i;

	for (i = 0; i < APP_COUNT; i++) {
		int32_t y = BAND_TOP + (int32_t)i * ROW_PITCH - scroll_px;
		bool visible = (y + ROW_HEIGHT > BAND_TOP) && (y < BAND_TOP + BAND_HEIGHT);

		if (visible) {
			lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_align(rows[i], LV_ALIGN_TOP_MID, ROW_X_OFFSET, y);
		} else {
			lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	lv_obj_set_height(thumb, thumb_height);
	lv_obj_align(thumb, LV_ALIGN_TOP_LEFT, 0,
		     MAX_SCROLL > 0 ? (scroll_px * travel) / MAX_SCROLL : 0);
}

/*
 * Position, not gesture: wherever the finger sits on the rail is where the
 * list goes. Behaves the same whether it was a tap or a drag.
 */
static void rail_pressed(lv_event_t *event)
{
	lv_indev_t *indev = lv_indev_active();
	lv_area_t area;
	lv_point_t point;
	int32_t usable;
	int32_t rel;

	ARG_UNUSED(event);

	if (indev == NULL || MAX_SCROLL <= 0) {
		return;
	}

	lv_indev_get_point(indev, &point);
	lv_obj_get_coords(rail, &area);

	usable = lv_area_get_height(&area);
	if (usable <= 0) {
		return;
	}

	rel = CLAMP(point.y - area.y1, 0, usable);
	scroll_px = (rel * MAX_SCROLL) / usable;
	apply_scroll();
}

static lv_obj_t *styled_box(lv_obj_t *parent, int32_t w, int32_t h, int32_t radius)
{
	lv_obj_t *box = lv_obj_create(parent);

	if (box == NULL) {
		return NULL;
	}

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, w, h);
	lv_obj_set_style_radius(box, radius, 0);
	lv_obj_set_style_bg_color(box, lv_color_hex(BG_HEX), 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	/* Lights up while held, so a tap that registers stays visible even if
	 * the click itself never fires. */
	lv_obj_set_style_bg_color(box, lv_color_hex(ON_HEX), LV_STATE_PRESSED);
	lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

	return box;
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);

	if (label == NULL) {
		return NULL;
	}

	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(TEXT_HEX), 0);
	lv_obj_center(label);

	return label;
}

static int create_rail(void)
{
	rail = lv_obj_create(menu_screen);
	if (rail == NULL) {
		return -ENOMEM;
	}

	lv_obj_remove_style_all(rail);
	lv_obj_set_size(rail, RAIL_WIDTH, RAIL_HEIGHT);
	lv_obj_align(rail, LV_ALIGN_TOP_MID, RAIL_X, RAIL_TOP);
	lv_obj_set_style_radius(rail, RAIL_WIDTH / 2, 0);
	lv_obj_set_style_bg_color(rail, lv_color_hex(RAIL_BG_HEX), 0);
	lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
	lv_obj_add_flag(rail, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
	/* Generous, since the rail is narrow and touches read left of the finger. */
	lv_obj_set_ext_click_area(rail, RAIL_HIT_MARGIN);
	lv_obj_add_event_cb(rail, rail_pressed, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(rail, rail_pressed, LV_EVENT_PRESSING, NULL);

	thumb = lv_obj_create(rail);
	if (thumb == NULL) {
		return -ENOMEM;
	}

	lv_obj_remove_style_all(thumb);
	lv_obj_set_width(thumb, RAIL_WIDTH);
	lv_obj_set_style_radius(thumb, RAIL_WIDTH / 2, 0);
	lv_obj_set_style_bg_color(thumb, lv_color_hex(THUMB_HEX), 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	/* The rail takes the touch; the thumb is decoration. */
	lv_obj_remove_flag(thumb, LV_OBJ_FLAG_CLICKABLE);

	return 0;
}

int menu_init(lv_obj_t *home_screen)
{
	lv_obj_t *back;
	size_t i;
	int ret;

	home = home_screen;

	menu_screen = lv_obj_create(NULL);
	if (menu_screen == NULL) {
		return -ENOMEM;
	}

	lv_obj_set_style_bg_color(menu_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, 0);
	lv_obj_remove_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);

	for (i = 0; i < APP_COUNT; i++) {
		rows[i] = styled_box(menu_screen, ROW_WIDTH, ROW_HEIGHT, ROW_RADIUS);
		if (rows[i] == NULL) {
			return -ENOMEM;
		}

		lv_obj_add_event_cb(rows[i], row_clicked, LV_EVENT_CLICKED, (void *)&apps[i]);

		if (add_label(rows[i], apps[i].name, &lv_font_montserrat_14) == NULL) {
			return -ENOMEM;
		}
	}

	ret = create_rail();
	if (ret < 0) {
		return ret;
	}

	back = styled_box(menu_screen, BACK_SIZE, BACK_SIZE, LV_RADIUS_CIRCLE);
	if (back == NULL) {
		return -ENOMEM;
	}

	lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -BACK_BOTTOM);
	lv_obj_set_ext_click_area(back, BACK_HIT_MARGIN);
	lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

	if (add_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_14) == NULL) {
		return -ENOMEM;
	}

	apply_scroll();

	return 0;
}

void menu_open(void)
{
	if (menu_screen == NULL || menu_visible) {
		return;
	}

	scroll_px = 0;
	apply_scroll();
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
