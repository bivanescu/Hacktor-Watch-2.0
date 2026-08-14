#ifndef MENU_H_
#define MENU_H_

#include <stdbool.h>
#include <lvgl.h>

/* Builds the menu screen. Pass the watch face, which the menu returns to. */
int menu_init(lv_obj_t *home_screen);

void menu_open(void);
void menu_close(void);

#endif /* MENU_H_ */
