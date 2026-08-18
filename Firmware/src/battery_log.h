#ifndef BATTERY_LOG_H_
#define BATTERY_LOG_H_

#include <stdbool.h>

/*
 * Samples the fuel gauge on a slow thread and keeps the history in RAM, so a
 * discharge test can run with USB unplugged and be read back afterwards.
 */
int battery_log_init(void);

/*
 * Called on every display power transition, so each sample knows how much of
 * the interval it covers was spent with the screen lit. That split is what
 * separates the idle current from the screen-on current.
 */
void battery_log_display_changed(bool active);

#endif /* BATTERY_LOG_H_ */
