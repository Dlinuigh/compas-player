#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdbool.h>

typedef enum {
    LED_CONTROL_RED = 0,
    LED_CONTROL_BLUE
} led_control_color_t;

typedef enum {
    LED_CONTROL_MODE_OFF = 0,
    LED_CONTROL_MODE_ON,
    LED_CONTROL_MODE_BLINK,
    LED_CONTROL_MODE_BREATHE,
    LED_CONTROL_MODE_STATUS
} led_control_mode_t;

/* Charge-status LEDs. R1 uses /sys/class/leds/{red,blue}. R3 II / R3 Pro II
 * use the SGM31324 RGB strip (/sys/class/leds/sgm31324-leds) with predefined
 * patterns 0..10. Normal charge indication only lights while connected to
 * external power. */
void led_control_apply(bool enabled);
bool led_control_available(void);

/* 11 on SGM31324 boards, else 0. Plugins use pattern ids 0..(count-1) as the
 * first argument to led_set/blink/breathe instead of "red"/"blue". */
int led_control_pattern_count(void);

/* Plugin override operations. User levels are 0..100. Effects use LVGL
 * timers, so these and their callbacks must run on the UI thread.
 * On pattern boards, color aliases map to stock-ish patterns (red≈charge,
 * blue≈full); prefer led_control_*_pattern() when the caller knows an id. */
void led_control_set_override(led_control_color_t color, int level);
bool led_control_blink(led_control_color_t color, int on_ms, int off_ms, int level);
bool led_control_breathe(led_control_color_t color, int period_ms, int level);
void led_control_set_override_pattern(int pattern, int level);
bool led_control_blink_pattern(int pattern, int on_ms, int off_ms, int level);
bool led_control_breathe_pattern(int pattern, int period_ms, int level);
void led_control_set_status(led_control_color_t color);
void led_control_set_all_status(void);
void led_control_clear_override(void);
void led_control_suspend(void);
void led_control_resume(bool enabled);
led_control_mode_t led_control_get_mode(led_control_color_t color);
int led_control_get_level(led_control_color_t color);

/* Call on every control tick. Updates normal status colors and colors that a
 * plugin explicitly returned to status mode, while leaving other plugin
 * effects alone. */
void led_control_poll(bool enabled);

#endif /* LED_CONTROL_H */
