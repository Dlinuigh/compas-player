#include "led_control.h"
#include "../power/battery.h"
#include "../power/charge_limiter.h"
#include "../../core/system/logging/debug_log.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define LED_CLASS_DIR "/sys/class/leds"
#define SGM_LED_NAME "sgm31324-leds"
#define SGM_PATTERN_COUNT 11
/* Default aliases when plugins still pass "red"/"blue" on a pattern board. */
#define SGM_ALIAS_RED_PATTERN 0
#define SGM_ALIAS_BLUE_PATTERN 1
#define SGM_STATUS_CHARGE_PATTERN SGM_ALIAS_RED_PATTERN
#define SGM_STATUS_FULL_PATTERN SGM_ALIAS_BLUE_PATTERN

/* Keep the charge-status indicator at a mid raw brightness. */
#define LED_STATUS_RAW_BRIGHTNESS 50
#define LED_RED_RAW_BRIGHTNESS_MAX 100
/* Verified on R1: blue dims or turns off above raw brightness 50. */
#define LED_BLUE_RAW_BRIGHTNESS_MAX 50
#define SGM_RAW_BRIGHTNESS_MAX 255
#define LED_EFFECT_TIMER_MS 40
#define LED_BLINK_MIN_MS 50
#define LED_BLINK_MAX_MS 60000
#define LED_BREATHE_MIN_MS 100
#define LED_BREATHE_MAX_MS 120000

typedef enum {
    LED_BACKEND_NONE = 0,
    LED_BACKEND_PAIR,
    LED_BACKEND_SGM,
} led_backend_t;

static const char * const pair_names[] = { "red", "blue" };
static const int pair_raw_max[] = {
    LED_RED_RAW_BRIGHTNESS_MAX,
    LED_BLUE_RAW_BRIGHTNESS_MAX,
};

typedef struct {
    led_control_mode_t mode;
    int level;
    int current_level;
    int pattern;
    int on_ms;
    int off_ms;
    int period_ms;
    int breaths_per_min;
    bool blink_on;
    bool kernel_breathe;
    lv_timer_t * timer;
    uint32_t effect_start_tick;
} led_state_t;

static led_state_t led_states[2] = {
    { .mode = LED_CONTROL_MODE_STATUS, .pattern = SGM_ALIAS_RED_PATTERN },
    { .mode = LED_CONTROL_MODE_STATUS, .pattern = SGM_ALIAS_BLUE_PATTERN },
};
static int brightness_cache[2] = { -1, -1 };
static char trigger_cache[2][16];
static int sgm_pattern_cache = -1;
static bool override_active = false;
static bool effects_suspended = false;
static bool status_enabled = false;
static led_backend_t backend = LED_BACKEND_NONE;
static bool backend_resolved = false;

static void resolve_backend(void) {
    if (backend_resolved) return;
    backend_resolved = true;
    char sgm_path[128];
    snprintf(sgm_path, sizeof(sgm_path), "%s/%s/brightness", LED_CLASS_DIR, SGM_LED_NAME);
    if (access(sgm_path, F_OK) == 0) {
        backend = LED_BACKEND_SGM;
        return;
    }
    char red_path[128];
    char blue_path[128];
    snprintf(red_path, sizeof(red_path), "%s/red/brightness", LED_CLASS_DIR);
    snprintf(blue_path, sizeof(blue_path), "%s/blue/brightness", LED_CLASS_DIR);
    if (access(red_path, F_OK) == 0 && access(blue_path, F_OK) == 0)
        backend = LED_BACKEND_PAIR;
    else
        backend = LED_BACKEND_NONE;
}

static int led_index(led_control_color_t color) {
    if (color == LED_CONTROL_RED) return 0;
    if (color == LED_CONTROL_BLUE) return 1;
    return -1;
}

static int raw_max_for(int index) {
    resolve_backend();
    if (backend == LED_BACKEND_SGM) return SGM_RAW_BRIGHTNESS_MAX;
    if (index < 0 || index > 1) return 0;
    return pair_raw_max[index];
}

static bool write_sysfs(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s", value) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool write_led_attr(int index, const char * attr, const char * value) {
    char path[160];
    resolve_backend();
    if (backend == LED_BACKEND_SGM) {
        snprintf(path, sizeof(path), "%s/%s/%s", LED_CLASS_DIR, SGM_LED_NAME, attr);
    } else if (backend == LED_BACKEND_PAIR) {
        if (index < 0 || index > 1) return false;
        snprintf(path, sizeof(path), "%s/%s/%s", LED_CLASS_DIR, pair_names[index], attr);
    } else {
        return false;
    }
    return write_sysfs(path, value);
}

static bool set_trigger(int index, const char * trigger, bool force) {
    int slot = index;
    if (backend == LED_BACKEND_SGM) slot = 0;
    if (!force && strcmp(trigger_cache[slot], trigger) == 0) return true;
    if (!write_led_attr(slot, "trigger", trigger)) return false;
    snprintf(trigger_cache[slot], sizeof(trigger_cache[slot]), "%s", trigger);
    brightness_cache[slot] = -1;
    return true;
}

static bool set_sgm_pattern(int pattern) {
    if (pattern < 0) pattern = 0;
    if (pattern >= SGM_PATTERN_COUNT) pattern = SGM_PATTERN_COUNT - 1;
    if (sgm_pattern_cache == pattern) return true;
    char value[8];
    snprintf(value, sizeof(value), "%d", pattern);
    if (!write_led_attr(0, "led_pattern", value)) return false;
    sgm_pattern_cache = pattern;
    brightness_cache[0] = -1;
    return true;
}

static int user_level_to_raw(int index, int level) {
    int max = raw_max_for(index);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    return (level * max + 50) / 100;
}

static int raw_to_user_level(int index, int raw) {
    int max = raw_max_for(index);
    if (max <= 0) return 0;
    if (raw < 0) return 0;
    if (raw > max) raw = max;
    return (raw * 100 + max / 2) / max;
}

static bool set_raw_brightness(int index, int raw) {
    int slot = (backend == LED_BACKEND_SGM) ? 0 : index;
    int max = raw_max_for(slot);
    char value[16];
    if (raw < 0) raw = 0;
    if (raw > max) raw = max;
    if (brightness_cache[slot] == raw) return true;
    snprintf(value, sizeof(value), "%d", raw);
    if (!write_led_attr(slot, "brightness", value)) return false;
    brightness_cache[slot] = raw;
    return true;
}

static bool set_user_brightness(int index, int level) {
    int slot = (backend == LED_BACKEND_SGM) ? 0 : index;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    led_states[slot].current_level = level;
    if (backend == LED_BACKEND_SGM) set_sgm_pattern(led_states[slot].pattern);
    return set_raw_brightness(slot, user_level_to_raw(slot, level));
}

static void delete_effect_timer(int index) {
    if (led_states[index].timer) {
        lv_timer_delete(led_states[index].timer);
        led_states[index].timer = NULL;
    }
}

static void cancel_color_effect(int index) {
    delete_effect_timer(index);
    set_trigger(index, "none", false);
    led_states[index].kernel_breathe = false;
    led_states[index].blink_on = false;
}

static void begin_override(void) {
    if (override_active) return;
    override_active = true;
    resolve_backend();
    int n = backend == LED_BACKEND_SGM ? 1 : 2;
    for (int i = 0; i < n; i++) {
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        set_trigger(i, "none", false);
    }
}

static void apply_status_colors(bool enabled) {
    resolve_backend();
    if (backend == LED_BACKEND_SGM) {
        if (led_states[0].mode != LED_CONTROL_MODE_STATUS) return;
        int raw = 0;
        int pattern = led_states[0].pattern;
        if (enabled) {
            bool capped = charge_limiter_is_confirmed_off();
            battery_external_power_state_t power_state = battery_get_external_power_state();
            bool power_disconnected = power_state == BATTERY_EXTERNAL_POWER_DISCONNECTED;
            bool full = !power_disconnected && (battery_is_full() || capped);
            bool charging = !full && !power_disconnected && battery_is_charging();
            if (charging) {
                pattern = SGM_STATUS_CHARGE_PATTERN;
                raw = LED_STATUS_RAW_BRIGHTNESS * SGM_RAW_BRIGHTNESS_MAX / 100;
            } else if (full) {
                pattern = SGM_STATUS_FULL_PATTERN;
                raw = LED_STATUS_RAW_BRIGHTNESS * SGM_RAW_BRIGHTNESS_MAX / 100;
            }
            DBG_LOG("led_control sgm: capped=%d power=%d full=%d charging=%d pattern=%d raw=%d\n",
                    capped, (int) power_state, full, charging, pattern, raw);
        }
        led_states[0].pattern = pattern;
        led_states[0].current_level = raw_to_user_level(0, raw);
        set_trigger(0, "none", false);
        set_sgm_pattern(pattern);
        set_raw_brightness(0, raw);
        return;
    }

    if (backend != LED_BACKEND_PAIR) return;
    bool wants_status[2] = {
        led_states[0].mode == LED_CONTROL_MODE_STATUS,
        led_states[1].mode == LED_CONTROL_MODE_STATUS,
    };
    if (!wants_status[0] && !wants_status[1]) return;

    int red_raw = 0;
    int blue_raw = 0;
    if (enabled) {
        bool capped = charge_limiter_is_confirmed_off();
        battery_external_power_state_t power_state = battery_get_external_power_state();
        bool power_disconnected = power_state == BATTERY_EXTERNAL_POWER_DISCONNECTED;
        bool full = !power_disconnected && (battery_is_full() || capped);
        bool charging = !full && !power_disconnected && battery_is_charging();
        red_raw = charging ? LED_STATUS_RAW_BRIGHTNESS : 0;
        blue_raw = full ? LED_STATUS_RAW_BRIGHTNESS : 0;
        DBG_LOG("led_control: capped=%d power_state=%d full=%d charging=%d red_raw=%d blue_raw=%d\n",
                capped, (int) power_state, full, charging, red_raw, blue_raw);
    }

    if (wants_status[0]) {
        led_states[0].current_level = raw_to_user_level(0, red_raw);
        set_raw_brightness(0, red_raw);
    }
    if (wants_status[1]) {
        led_states[1].current_level = raw_to_user_level(1, blue_raw);
        set_raw_brightness(1, blue_raw);
    }
}

static void led_effect_timer_cb(lv_timer_t * timer) {
    led_state_t * state = (led_state_t *) lv_timer_get_user_data(timer);
    if (!state || !override_active || effects_suspended || state->timer != timer) return;
    int index = (int) (state - led_states);
    if (index < 0 || index > 1) return;
    int slot = (backend == LED_BACKEND_SGM) ? 0 : index;

    if (state->mode == LED_CONTROL_MODE_BLINK) {
        state->blink_on = !state->blink_on;
        int level = state->blink_on ? state->level : 0;
        set_user_brightness(slot, level);
        lv_timer_set_period(timer, state->blink_on ? state->on_ms : state->off_ms);
    } else if (state->mode == LED_CONTROL_MODE_BREATHE && !state->kernel_breathe) {
        uint32_t elapsed = lv_tick_elaps(state->effect_start_tick);
        uint32_t phase = elapsed % (uint32_t) state->period_ms;
        uint32_t half = (uint32_t) state->period_ms / 2;
        int level;
        if (phase < half) {
            level = half ? (int) ((uint64_t) state->level * phase / half) : state->level;
        } else {
            uint32_t falling = (uint32_t) state->period_ms - phase;
            level = half ? (int) ((uint64_t) state->level * falling / half) : 0;
        }
        if (level < 0) level = 0;
        if (level > state->level) level = state->level;
        set_user_brightness(slot, level);
    }
}

static bool start_software_breathe(int index) {
    led_state_t * state = &led_states[index];
    state->kernel_breathe = false;
    state->current_level = 0;
    state->effect_start_tick = lv_tick_get();
    set_user_brightness(index, 0);
    state->timer = lv_timer_create(led_effect_timer_cb, LED_EFFECT_TIMER_MS, state);
    return state->timer != NULL;
}

bool led_control_available(void) {
    resolve_backend();
    return backend != LED_BACKEND_NONE;
}

int led_control_pattern_count(void) {
    resolve_backend();
    return backend == LED_BACKEND_SGM ? SGM_PATTERN_COUNT : 0;
}

static int pattern_for_color(led_control_color_t color) {
    return color == LED_CONTROL_BLUE ? SGM_ALIAS_BLUE_PATTERN : SGM_ALIAS_RED_PATTERN;
}

void led_control_apply(bool enabled) {
    status_enabled = enabled;
    resolve_backend();
    int n = backend == LED_BACKEND_SGM ? 1 : 2;
    if (override_active) {
        for (int i = 0; i < n; i++) {
            if (led_states[i].mode == LED_CONTROL_MODE_STATUS) set_trigger(i, "none", true);
        }
        apply_status_colors(enabled);
        return;
    }

    for (int i = 0; i < n; i++) {
        delete_effect_timer(i);
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        led_states[i].level = 0;
        led_states[i].kernel_breathe = false;
        set_trigger(i, "none", true);
    }
    apply_status_colors(enabled);
}

static void apply_override_on(int slot, int pattern, int level, led_control_mode_t mode) {
    resolve_backend();
    if (backend == LED_BACKEND_NONE) return;
    if (backend == LED_BACKEND_SGM) slot = 0;
    begin_override();
    cancel_color_effect(slot);
    if (backend == LED_BACKEND_SGM) {
        if (pattern < 0) pattern = 0;
        if (pattern >= SGM_PATTERN_COUNT) pattern = SGM_PATTERN_COUNT - 1;
        led_states[slot].pattern = pattern;
    }
    led_states[slot].level = level;
    led_states[slot].mode = level == 0 ? LED_CONTROL_MODE_OFF : mode;
    set_user_brightness(slot, level);
}

void led_control_set_override(led_control_color_t color, int level) {
    int index = led_index(color);
    if (index < 0) return;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    int pattern = pattern_for_color(color);
    apply_override_on(index, pattern, level, LED_CONTROL_MODE_ON);
}

void led_control_set_override_pattern(int pattern, int level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    apply_override_on(0, pattern, level, LED_CONTROL_MODE_ON);
}

bool led_control_blink(led_control_color_t color, int on_ms, int off_ms, int level) {
    return led_control_blink_pattern(pattern_for_color(color), on_ms, off_ms, level);
}

bool led_control_blink_pattern(int pattern, int on_ms, int off_ms, int level) {
    resolve_backend();
    if (backend == LED_BACKEND_NONE) return false;
    int slot = backend == LED_BACKEND_SGM ? 0 : 0;
    if (backend == LED_BACKEND_PAIR) {
        /* Pattern boards only; on the pair board treat pattern 0/1 as red/blue. */
        slot = (pattern == SGM_ALIAS_BLUE_PATTERN) ? 1 : 0;
    }
    if (on_ms < LED_BLINK_MIN_MS) on_ms = LED_BLINK_MIN_MS;
    if (on_ms > LED_BLINK_MAX_MS) on_ms = LED_BLINK_MAX_MS;
    if (off_ms < LED_BLINK_MIN_MS) off_ms = LED_BLINK_MIN_MS;
    if (off_ms > LED_BLINK_MAX_MS) off_ms = LED_BLINK_MAX_MS;
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    begin_override();
    cancel_color_effect(slot);
    if (backend == LED_BACKEND_SGM) {
        if (pattern < 0) pattern = 0;
        if (pattern >= SGM_PATTERN_COUNT) pattern = SGM_PATTERN_COUNT - 1;
        led_states[slot].pattern = pattern;
    }
    led_states[slot].mode = level == 0 ? LED_CONTROL_MODE_OFF : LED_CONTROL_MODE_BLINK;
    led_states[slot].level = level;
    led_states[slot].on_ms = on_ms;
    led_states[slot].off_ms = off_ms;
    led_states[slot].blink_on = true;
    if (level == 0) {
        set_user_brightness(slot, 0);
        return true;
    }
    set_user_brightness(slot, level);
    if (effects_suspended) return true;
    led_states[slot].timer = lv_timer_create(led_effect_timer_cb, on_ms, &led_states[slot]);
    return led_states[slot].timer != NULL;
}

bool led_control_breathe(led_control_color_t color, int period_ms, int level) {
    return led_control_breathe_pattern(pattern_for_color(color), period_ms, level);
}

bool led_control_breathe_pattern(int pattern, int period_ms, int level) {
    resolve_backend();
    if (backend == LED_BACKEND_NONE) return false;
    int slot = 0;
    if (backend == LED_BACKEND_PAIR)
        slot = (pattern == SGM_ALIAS_BLUE_PATTERN) ? 1 : 0;
    if (period_ms < LED_BREATHE_MIN_MS) period_ms = LED_BREATHE_MIN_MS;
    if (period_ms > LED_BREATHE_MAX_MS) period_ms = LED_BREATHE_MAX_MS;
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    begin_override();
    cancel_color_effect(slot);
    if (backend == LED_BACKEND_SGM) {
        if (pattern < 0) pattern = 0;
        if (pattern >= SGM_PATTERN_COUNT) pattern = SGM_PATTERN_COUNT - 1;
        led_states[slot].pattern = pattern;
    }
    led_states[slot].mode = level == 0 ? LED_CONTROL_MODE_OFF : LED_CONTROL_MODE_BREATHE;
    led_states[slot].level = level;
    led_states[slot].period_ms = period_ms;
    led_states[slot].breaths_per_min = (60000 + period_ms / 2) / period_ms;
    if (led_states[slot].breaths_per_min < 1) led_states[slot].breaths_per_min = 1;
    if (led_states[slot].breaths_per_min > 600) led_states[slot].breaths_per_min = 600;
    if (level == 0) {
        set_user_brightness(slot, 0);
        return true;
    }

    /* R1 red can use the kernel breathing trigger at full peak. Pattern boards
     * and blue always step in software so the selected pattern stays put. */
    if (backend == LED_BACKEND_PAIR && slot == 0 && level == 100 && !effects_suspended) {
        set_user_brightness(slot, 100);
        char bpm[8];
        snprintf(bpm, sizeof(bpm), "%d", led_states[slot].breaths_per_min);
        if (set_trigger(slot, "breathing", false) &&
            write_led_attr(slot, "breaths_per_min", bpm)) {
            led_states[slot].kernel_breathe = true;
            led_states[slot].current_level = 100;
            return true;
        }
        set_trigger(slot, "none", true);
    }
    return start_software_breathe(slot);
}

void led_control_set_status(led_control_color_t color) {
    int index = led_index(color);
    if (index < 0) return;
    resolve_backend();
    int slot = backend == LED_BACKEND_SGM ? 0 : index;
    begin_override();
    cancel_color_effect(slot);
    led_states[slot].mode = LED_CONTROL_MODE_STATUS;
    led_states[slot].level = 0;
    if (backend == LED_BACKEND_SGM) led_states[slot].pattern = pattern_for_color(color);
    apply_status_colors(status_enabled);
}

void led_control_set_all_status(void) {
    resolve_backend();
    begin_override();
    int n = backend == LED_BACKEND_SGM ? 1 : 2;
    for (int i = 0; i < n; i++) {
        cancel_color_effect(i);
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        led_states[i].level = 0;
    }
    apply_status_colors(status_enabled);
}

void led_control_clear_override(void) {
    resolve_backend();
    int n = backend == LED_BACKEND_SGM ? 1 : 2;
    for (int i = 0; i < n; i++) {
        cancel_color_effect(i);
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        led_states[i].level = 0;
    }
    override_active = false;
}

void led_control_suspend(void) {
    if (effects_suspended) return;
    effects_suspended = true;
    resolve_backend();
    int n = backend == LED_BACKEND_SGM ? 1 : 2;
    for (int i = 0; i < n; i++) {
        if (led_states[i].timer) lv_timer_pause(led_states[i].timer);
        if (led_states[i].kernel_breathe) set_trigger(i, "none", false);
    }
}

static void resume_color_effect(int index) {
    led_state_t * state = &led_states[index];
    set_trigger(index, "none", true);
    if (state->mode == LED_CONTROL_MODE_OFF) {
        state->current_level = 0;
        set_user_brightness(index, 0);
    } else if (state->mode == LED_CONTROL_MODE_ON) {
        set_user_brightness(index, state->level);
    } else if (state->mode == LED_CONTROL_MODE_BLINK) {
        state->blink_on = true;
        set_user_brightness(index, state->level);
        if (!state->timer) state->timer = lv_timer_create(led_effect_timer_cb, state->on_ms, state);
        if (state->timer) {
            lv_timer_set_period(state->timer, state->on_ms);
            lv_timer_reset(state->timer);
            lv_timer_resume(state->timer);
        }
    } else if (state->mode == LED_CONTROL_MODE_BREATHE) {
        if (state->kernel_breathe && backend == LED_BACKEND_PAIR && index == 0 && state->level == 100) {
            set_user_brightness(index, 100);
            char bpm[8];
            snprintf(bpm, sizeof(bpm), "%d", state->breaths_per_min);
            if (set_trigger(index, "breathing", false) &&
                write_led_attr(index, "breaths_per_min", bpm)) {
                state->current_level = 100;
                return;
            }
            set_trigger(index, "none", true);
            state->kernel_breathe = false;
        }
        state->effect_start_tick = lv_tick_get();
        state->current_level = 0;
        set_user_brightness(index, 0);
        if (!state->timer) state->timer = lv_timer_create(led_effect_timer_cb, LED_EFFECT_TIMER_MS, state);
        if (state->timer) {
            lv_timer_set_period(state->timer, LED_EFFECT_TIMER_MS);
            lv_timer_reset(state->timer);
            lv_timer_resume(state->timer);
        }
    } else if (state->mode == LED_CONTROL_MODE_STATUS) {
        state->current_level = 0;
    }
}

void led_control_resume(bool enabled) {
    if (!effects_suspended) return;
    effects_suspended = false;
    resolve_backend();
    if (!override_active) {
        led_control_apply(enabled);
        return;
    }
    int n = backend == LED_BACKEND_SGM ? 1 : 2;
    for (int i = 0; i < n; i++) {
        if (led_states[i].mode != LED_CONTROL_MODE_STATUS) resume_color_effect(i);
        else set_trigger(i, "none", true);
    }
    apply_status_colors(enabled);
}

void led_control_poll(bool enabled) {
    status_enabled = enabled;
    if (!override_active && !enabled) return;
    apply_status_colors(enabled);
}

led_control_mode_t led_control_get_mode(led_control_color_t color) {
    resolve_backend();
    int index = led_index(color);
    if (backend == LED_BACKEND_SGM) index = 0;
    return index < 0 ? LED_CONTROL_MODE_OFF : led_states[index].mode;
}

int led_control_get_level(led_control_color_t color) {
    resolve_backend();
    int index = led_index(color);
    if (backend == LED_BACKEND_SGM) index = 0;
    if (index < 0) return 0;
    return led_states[index].mode == LED_CONTROL_MODE_STATUS
               ? led_states[index].current_level
               : led_states[index].level;
}
