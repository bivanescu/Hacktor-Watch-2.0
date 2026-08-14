#include "menu.h"

#include <errno.h>
#include <zephyr/sys/printk.h>

#define BACK_SIZE   150
#define BACK_RADIUS 20
#define BACK_BG_HEX 0x282828
#define BACK_ON_HEX 0x0080FF
#define TEXT_HEX    0xFFFFFF

/*
 * Deliberately the smallest thing that can work: one screen, one button. An
 * earlier attempt built a scrollable list straight away, never got a single
 * touch through, and left no way to tell which layer was at fault. Grow this
 * only once each step is confirmed on hardware.
 *
 * Nothing here may print from an LVGL callback. The USB console blocks when
 * the host is not draining it, and these callbacks run on the UI thread while
 * it holds the LVGL lock, so a stray printk freezes the whole interface.
 */

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

int menu_init(lv_obj_t *home_screen)
{
	lv_obj_t *back;
	lv_obj_t *label;

	home = home_screen;

	menu_screen = lv_obj_create(NULL);
	if (menu_screen == NULL) {
		return -ENOMEM;
	}

	lv_obj_set_style_bg_color(menu_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, 0);
	lv_obj_remove_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);

	back = lv_obj_create(menu_screen);
	if (back == NULL) {
		return -ENOMEM;
	}

	lv_obj_remove_style_all(back);
	lv_obj_set_size(back, BACK_SIZE, BACK_SIZE);
	lv_obj_center(back);
	lv_obj_set_style_radius(back, BACK_RADIUS, 0);
	lv_obj_set_style_bg_color(back, lv_color_hex(BACK_BG_HEX), 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
	/* Lights up while held, so a tap that registers is visible even if the
	 * click never fires. */
	lv_obj_set_style_bg_color(back, lv_color_hex(BACK_ON_HEX), LV_STATE_PRESSED);
	lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

	label = lv_label_create(back);
	if (label == NULL) {
		return -ENOMEM;
	}

	lv_label_set_text(label, "Inapoi");
	lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(TEXT_HEX), 0);
	lv_obj_center(label);

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
