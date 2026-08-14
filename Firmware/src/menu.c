#include "menu.h"

#include <errno.h>
#include <zephyr/sys/util.h>

#define ROW_WIDTH    170
#define ROW_HEIGHT   44
#define ROW_FIRST_Y  38
#define ROW_PITCH    52
#define ROW_RADIUS   12
#define BACK_SIZE    40
#define BACK_BOTTOM  6
#define BACK_HIT_MARGIN 20
#define BG_HEX       0x282828
#define ON_HEX       0x0080FF
#define TEXT_HEX     0xFFFFFF

/*
 * Grown one step at a time, each step confirmed on hardware before the next.
 * An earlier attempt built the whole thing at once — scrollable list, flex
 * layout, six rows — never registered a touch, and gave no way to tell which
 * layer was at fault.
 *
 * Rows sit at fixed positions straight on the screen. Putting them inside a
 * scrollable container kills every press, whether its styles are wiped or set
 * explicitly, so that route needs solving on its own before more entries than
 * fit on one screen are worth attempting.
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
	{ "Setari",    NULL },
};

static lv_obj_t *menu_screen;
static lv_obj_t *home;
static bool menu_visible;

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

int menu_init(lv_obj_t *home_screen)
{
	lv_obj_t *back;
	size_t i;

	home = home_screen;

	menu_screen = lv_obj_create(NULL);
	if (menu_screen == NULL) {
		return -ENOMEM;
	}

	lv_obj_set_style_bg_color(menu_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, 0);
	lv_obj_remove_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);

	for (i = 0; i < ARRAY_SIZE(apps); i++) {
		lv_obj_t *row = styled_box(menu_screen, ROW_WIDTH, ROW_HEIGHT, ROW_RADIUS);

		if (row == NULL) {
			return -ENOMEM;
		}

		lv_obj_align(row, LV_ALIGN_TOP_MID, 0, ROW_FIRST_Y + (int32_t)i * ROW_PITCH);
		lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)&apps[i]);

		if (add_label(row, apps[i].name, &lv_font_montserrat_14) == NULL) {
			return -ENOMEM;
		}
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
