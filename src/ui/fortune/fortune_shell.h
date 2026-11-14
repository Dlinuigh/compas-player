#pragma once

#include <lvgl/lvgl.h>
#include <stdbool.h>

/* Lean ForTune home: 首页 / 资料库 / 搜索. Built only under FORTUNE_SHELL. */
lv_obj_t * fortune_shell_build_home(void);
void fortune_shell_init(void);
void fortune_shell_poll(void);
void fortune_shell_teardown(void);
lv_obj_t * fortune_shell_get_home(void);

void fortune_shell_open_player(void);
void fortune_shell_open_wifi_settings(void);
void fortune_shell_open_settings_minimal(void);
