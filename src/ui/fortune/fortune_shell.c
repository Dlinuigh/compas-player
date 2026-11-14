#include "fortune_shell.h"

#include "../../core/persistence/settings.h"
#include "../../core/process/worker_thread.h"
#include "../../core/system/logging/db_log.h"
#include "../../core/system/runtime/board_config.h"
#include "../../hardware/usb/usb_mode_control.h"
#include "../../library/remote/remote_track.h"
#include "../../netease/netease_client.h"
#include "../appearance/gui_theme.h"
#include "../library/browse/gui_library.h"
#include "../application/gui.h"
#include "../network/gui_network.h"
#include "../player/screen/gui_player.h"
#include "../shell/gui_shell.h"
#include "../shell/gui_navigation.h"
#include "../shell/gui_notifications.h"
#include "../text/rendering/gui_text_input.h"
#include "../updates/gui_updates.h"
#include "../widgets/builders/screen_builders.h"
#include "../widgets/lists/compact_list.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAB_HOME = 0, TAB_LIBRARY = 1, TAB_SEARCH = 2, TAB_COUNT = 3 };
enum {
    JOB_DAILY = 1,
    JOB_PLAYLISTS,
    JOB_PLAYLIST_TRACKS,
    JOB_LIKED,
    JOB_SEARCH,
    JOB_PLAY
};

typedef struct {
    int kind;
    char arg[128];
    int start_index;
    netease_track_t tracks[NETEASE_LIST_MAX];
    netease_playlist_t playlists[NETEASE_LIST_MAX];
    int track_count;
    int playlist_count;
    remote_track_meta_t * play_metas;
    int play_count;
    int play_start;
    char err[64];
    bool ok;
} fortune_job_t;

static lv_obj_t * g_home;
static lv_obj_t * g_content;
static lv_obj_t * g_title;
static lv_obj_t * g_tab_btns[TAB_COUNT];
static int g_tab;
static bool g_busy;

static compact_list_item_t g_items[NETEASE_LIST_MAX + 16];
static char g_labels[NETEASE_LIST_MAX + 16][NETEASE_TITLE_MAX + NETEASE_ARTIST_MAX + 8];
static char g_subtitles[NETEASE_LIST_MAX + 16][NETEASE_ARTIST_MAX];
static int g_item_count;
static int g_item_kind; /* 0=menu 1=tracks 2=playlists */
static netease_track_t g_tracks[NETEASE_LIST_MAX];
static int g_track_count;
static netease_playlist_t g_playlists[NETEASE_LIST_MAX];
static char g_search_q[128];

static const char * const TAB_NAMES[TAB_COUNT] = { "首页", "资料库", "搜索" };

static void rebuild_content(void);
static void home_menu(void);
static void library_menu(void);
static void search_menu(void);

static void style_screen(lv_obj_t * scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(GUI_COLOR_SCREEN), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

static void clear_items(void) {
    g_item_count = 0;
    g_item_kind = 0;
    memset(g_items, 0, sizeof(g_items));
}

static void add_item(const char * label, const char * subtitle) {
    if (g_item_count >= (int) (sizeof(g_items) / sizeof(g_items[0]))) return;
    int i = g_item_count++;
    snprintf(g_labels[i], sizeof(g_labels[i]), "%s", label ? label : "");
    snprintf(g_subtitles[i], sizeof(g_subtitles[i]), "%s", subtitle ? subtitle : "");
    g_items[i].label = g_labels[i];
    g_items[i].subtitle = g_subtitles[i][0] ? g_subtitles[i] : NULL;
    g_items[i].is_action = true;
}

static void set_title(const char * text) {
    if (g_title) lv_label_set_text(g_title, text ? text : "");
}

static void set_busy(bool busy) {
    g_busy = busy;
    if (busy) set_title("…");
}

static void fill_tracks_as_items(const netease_track_t * tracks, int count) {
    clear_items();
    g_item_kind = 1;
    g_track_count = count > NETEASE_LIST_MAX ? NETEASE_LIST_MAX : count;
    memcpy(g_tracks, tracks, (size_t) g_track_count * sizeof(netease_track_t));
    for (int i = 0; i < g_track_count; i++) {
        add_item(g_tracks[i].title[0] ? g_tracks[i].title : g_tracks[i].id, g_tracks[i].artist);
        g_items[i].is_action = false;
    }
}

static void fill_playlists_as_items(const netease_playlist_t * pls, int count) {
    clear_items();
    g_item_kind = 2;
    int n = count > NETEASE_LIST_MAX ? NETEASE_LIST_MAX : count;
    memcpy(g_playlists, pls, (size_t) n * sizeof(netease_playlist_t));
    for (int i = 0; i < n; i++) {
        char sub[64];
        snprintf(sub, sizeof(sub), "%d 首", g_playlists[i].track_count);
        add_item(g_playlists[i].name[0] ? g_playlists[i].name : g_playlists[i].id, sub);
        g_items[i].is_action = false;
    }
}

static void job_done_ui(void * p) {
    fortune_job_t * job = (fortune_job_t *) p;
    set_busy(false);
    if (!job) return;
    if (!job->ok) {
        show_info_toast(job->err[0] ? job->err : "失败");
        free(job->play_metas);
        free(job);
        return;
    }
    switch (job->kind) {
    case JOB_DAILY:
    case JOB_LIKED:
    case JOB_SEARCH:
        fill_tracks_as_items(job->tracks, job->track_count);
        set_title(job->kind == JOB_SEARCH ? "搜索" : (job->kind == JOB_LIKED ? "喜欢" : "每日推荐"));
        rebuild_content();
        break;
    case JOB_PLAYLISTS:
        fill_playlists_as_items(job->playlists, job->playlist_count);
        set_title("歌单");
        rebuild_content();
        break;
    case JOB_PLAYLIST_TRACKS:
        fill_tracks_as_items(job->tracks, job->track_count);
        set_title("歌单");
        rebuild_content();
        break;
    case JOB_PLAY:
        if (job->play_metas && job->play_count > 0) {
            gui_plugin_play_remote_tracks(job->play_metas, job->play_count, job->play_start);
            lv_obj_t * player = gui_player_get_screen();
            if (player) nav_push(player);
        }
        break;
    default:
        break;
    }
    free(job->play_metas);
    free(job);
}

static void * job_worker(void * p) {
    fortune_job_t * job = (fortune_job_t *) p;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    switch (job->kind) {
    case JOB_DAILY:
        job->ok = netease_client_daily_songs(job->tracks, NETEASE_LIST_MAX, &job->track_count);
        break;
    case JOB_PLAYLISTS:
        job->ok = netease_client_user_playlists(job->playlists, NETEASE_LIST_MAX, &job->playlist_count);
        break;
    case JOB_PLAYLIST_TRACKS:
        job->ok = netease_client_playlist_tracks(job->arg, job->tracks, NETEASE_LIST_MAX, &job->track_count);
        break;
    case JOB_LIKED:
        job->ok = netease_client_liked_songs(job->tracks, NETEASE_LIST_MAX, &job->track_count);
        break;
    case JOB_SEARCH:
        job->ok = netease_client_search_songs(job->arg, job->tracks, NETEASE_LIST_MAX, &job->track_count);
        break;
    case JOB_PLAY:
        job->ok = netease_client_build_remote_queue(job->tracks, job->track_count, job->start_index,
                                                    &job->play_metas, &job->play_count, &job->play_start);
        break;
    default:
        job->ok = false;
        break;
    }
    if (!job->ok && !job->err[0]) snprintf(job->err, sizeof(job->err), "失败");
    lv_async_call(job_done_ui, job);
    return NULL;
}

static bool require_online(const char * need_login_msg) {
    if (!gui_shell_wifi_connected()) {
        show_info_toast("未联网");
        return false;
    }
    if (!netease_client_logged_in()) {
        show_info_toast(need_login_msg ? need_login_msg : "未登录");
        return false;
    }
    return true;
}

static void start_job(fortune_job_t * job) {
    if (g_busy) {
        free(job->play_metas);
        free(job);
        return;
    }
    set_busy(true);
    pthread_t th;
    /* eapi + http_request need headroom; 96KB overflowed and restarted the player. */
    if (worker_thread_create(&th, 512 * 1024, job_worker, job) != 0) {
        set_busy(false);
        free(job->play_metas);
        free(job);
        show_info_toast("线程失败");
        return;
    }
    pthread_detach(th);
}

static void start_simple_job(int kind, const char * arg) {
    if (!require_online("未登录")) return;
    fortune_job_t * job = (fortune_job_t *) calloc(1, sizeof(*job));
    if (!job) return;
    job->kind = kind;
    if (arg) snprintf(job->arg, sizeof(job->arg), "%s", arg);
    start_job(job);
}

static void start_play_job(int start_index) {
    if (g_track_count <= 0) return;
    if (!require_online("未登录")) return;
    fortune_job_t * job = (fortune_job_t *) calloc(1, sizeof(*job));
    if (!job) return;
    job->kind = JOB_PLAY;
    job->track_count = g_track_count;
    job->start_index = start_index;
    memcpy(job->tracks, g_tracks, (size_t) g_track_count * sizeof(netease_track_t));
    start_job(job);
}

static void home_menu(void) {
    clear_items();
    netease_client_poll_session_file();
    if (netease_client_logged_in()) {
        char line[160];
        snprintf(line, sizeof(line), "账号 · %s", netease_client_nickname());
        add_item(line, "点按退出登录");
    } else {
        add_item("未登录", "SD/ForTune/session.txt");
    }
    add_item("每日推荐", NULL);
    add_item("我的歌单", NULL);
    add_item("我喜欢的音乐", NULL);
    add_item("正在播放", NULL);
    add_item("Wi-Fi", NULL);
    add_item(gui_network_adb_active() ? "ADB · 开" : "ADB · 关", "调试推送/拉日志");
    add_item(current_settings.db_logging_enabled ? "调试日志 · 开" : "调试日志 · 关",
             "SD/.logs");
    add_item("更新播放器", "SD/.compas 或在线");
    set_title("首页");
}

static void library_menu(void) {
    clear_items();
    add_item("文件夹", NULL);
    add_item("全部歌曲", NULL);
    add_item("艺人", NULL);
    add_item("专辑", NULL);
    add_item("播放列表", NULL);
    add_item("重新扫描", NULL);
    add_item("网易喜欢", NULL);
    set_title("资料库");
}

static void search_menu(void) {
    clear_items();
    if (g_search_q[0]) {
        char line[160];
        snprintf(line, sizeof(line), "搜索：%s", g_search_q);
        add_item(line, "点按改关键字");
    } else {
        add_item("输入关键字", NULL);
    }
    set_title("搜索");
}

static void search_done_cb(const char * text, void * user_data) {
    (void) user_data;
    snprintf(g_search_q, sizeof(g_search_q), "%s", text ? text : "");
    if (g_search_q[0]) start_simple_job(JOB_SEARCH, g_search_q);
    else {
        search_menu();
        rebuild_content();
    }
}

static void on_row(int index) {
    if (g_busy || index < 0 || index >= g_item_count) return;

    if (g_item_kind == 1) { start_play_job(index); return; }
    if (g_item_kind == 2) {
        start_simple_job(JOB_PLAYLIST_TRACKS, g_playlists[index].id);
        return;
    }

    if (g_tab == TAB_HOME) {
        switch (index) {
        case 0:
            if (netease_client_logged_in()) {
                netease_client_logout();
                show_info_toast("已退出");
            } else {
                netease_client_poll_session_file();
                if (!netease_client_logged_in()) show_info_toast("无 session");
            }
            home_menu();
            rebuild_content();
            break;
        case 1: start_simple_job(JOB_DAILY, NULL); break;
        case 2: start_simple_job(JOB_PLAYLISTS, NULL); break;
        case 3: start_simple_job(JOB_LIKED, NULL); break;
        case 4: fortune_shell_open_player(); break;
        case 5: fortune_shell_open_wifi_settings(); break;
        case 6: {
            bool want_adb = !gui_network_adb_active();
            if (!start_usb_mode_switch(want_adb ? USB_MODE_ADB : USB_MODE_STORAGE)) {
                show_info_toast("USB 忙");
            } else {
                current_settings.usb_mode = (int) (want_adb ? USB_MODE_ADB : USB_MODE_STORAGE);
                settings_save(&current_settings);
                show_info_toast(want_adb ? "ADB 开启中" : "ADB 关闭中");
            }
            home_menu();
            rebuild_content();
            break;
        }
        case 7:
            current_settings.db_logging_enabled = !current_settings.db_logging_enabled;
            db_log_set_enabled(current_settings.db_logging_enabled);
            settings_save(&current_settings);
            home_menu();
            rebuild_content();
            break;
        case 8:
            gui_updates_open_player_menu();
            break;
        default: break;
        }
        return;
    }

    if (g_tab == TAB_LIBRARY) {
        switch (index) {
        case 0: nav_push(gui_library_get_files_screen()); break;
        case 1: nav_push(gui_library_get_all_songs_screen()); break;
        case 2: nav_push(gui_library_get_artists_screen()); break;
        case 3: nav_push(gui_library_get_albums_screen()); break;
        case 4: nav_push(gui_library_get_playlists_screen()); break;
        case 5:
            start_library_rescan();
            show_info_toast("扫描中");
            break;
        case 6: start_simple_job(JOB_LIKED, NULL); break;
        default: break;
        }
        return;
    }

    if (g_tab == TAB_SEARCH)
        show_text_entry("搜索", g_search_q, false, false, search_done_cb, NULL);
}

static void rebuild_content(void) {
    if (!g_content) return;
    lv_obj_clean(g_content);
    if (g_item_count <= 0) {
        if (g_tab == TAB_HOME) home_menu();
        else if (g_tab == TAB_LIBRARY) library_menu();
        else search_menu();
    }
    lv_obj_t * list = build_compact_list_widget(g_content, g_items, g_item_count, on_row, NULL,
                                                LIST_ROW_WIDTH, false, NULL);
    if (!list) return;
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
}

static void tab_click(lv_event_t * e) {
    int tab = (int) (intptr_t) lv_event_get_user_data(e);
    if (tab < 0 || tab >= TAB_COUNT || tab == g_tab) return;
    g_tab = tab;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (!g_tab_btns[i]) continue;
        lv_obj_t * lab = lv_obj_get_child(g_tab_btns[i], 0);
        if (lab)
            lv_obj_set_style_text_color(lab,
                lv_color_hex(i == g_tab ? GUI_COLOR_PRIMARY : GUI_COLOR_SECONDARY), 0);
    }
    clear_items();
    if (g_tab == TAB_HOME) home_menu();
    else if (g_tab == TAB_LIBRARY) library_menu();
    else search_menu();
    rebuild_content();
}

lv_obj_t * fortune_shell_build_home(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    style_screen(scr);
    lv_obj_set_size(scr, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t tab_h = BOARD_SCALE_PX(56);
    int32_t top = STATUS_BAR_CLEARANCE;
    int32_t title_h = TITLE_ROW_HEIGHT;

    g_title = lv_label_create(scr);
    lv_obj_set_style_text_color(g_title, lv_color_hex(GUI_COLOR_PRIMARY), 0);
    lv_obj_set_style_text_font(g_title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_label_set_text(g_title, "首页");
    lv_obj_align(g_title, LV_ALIGN_TOP_LEFT, GUI_TEXT_INSET, top + BOARD_SCALE_PX(12));

    g_content = lv_obj_create(scr);
    style_screen(g_content);
    lv_obj_set_style_border_width(g_content, 0, 0);
    lv_obj_set_style_pad_all(g_content, 0, 0);
    lv_obj_set_size(g_content, BOARD_SCREEN_WIDTH,
                    BOARD_SCREEN_HEIGHT - top - title_h - tab_h - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(g_content, LV_ALIGN_TOP_LEFT, 0, top + title_h);
    lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * bar = lv_obj_create(scr);
    lv_obj_set_style_bg_color(bar, lv_color_hex(GUI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_size(bar, BOARD_SCREEN_WIDTH, tab_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_BAND_HEIGHT);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t * btn = lv_button_create(bar);
        g_tab_btns[i] = btn;
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_size(btn, BOARD_SCREEN_WIDTH / TAB_COUNT, tab_h);
        lv_obj_t * lab = lv_label_create(btn);
        lv_label_set_text(lab, TAB_NAMES[i]);
        lv_obj_set_style_text_font(lab, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(i == 0 ? GUI_COLOR_PRIMARY : GUI_COLOR_SECONDARY), 0);
        lv_obj_center(lab);
        lv_obj_add_event_cb(btn, tab_click, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }

    g_home = scr;
    g_tab = TAB_HOME;
    home_menu();
    rebuild_content();
    finalize_screen_navigation(scr);
    return scr;
}

void fortune_shell_init(void) {
    netease_client_init();
    g_home = NULL;
    g_content = NULL;
    g_title = NULL;
    g_tab = TAB_HOME;
    g_busy = false;
    g_search_q[0] = '\0';
    clear_items();
}

void fortune_shell_poll(void) {
    static uint32_t last;
    uint32_t now = lv_tick_get();
    if (now - last < 2000) return;
    last = now;
    if (g_tab == TAB_HOME && g_item_kind == 0 && !g_busy) {
        bool before = netease_client_logged_in();
        netease_client_poll_session_file();
        if (before != netease_client_logged_in()) {
            home_menu();
            rebuild_content();
        }
    }
}

void fortune_shell_teardown(void) {
    g_home = NULL;
    g_content = NULL;
    g_title = NULL;
}

lv_obj_t * fortune_shell_get_home(void) { return g_home; }

void fortune_shell_open_player(void) {
    lv_obj_t * player = gui_player_get_screen();
    if (player) nav_push(player);
}

void fortune_shell_open_wifi_settings(void) {
    lv_obj_t * wifi = gui_network_get_wifi_screen();
    if (wifi) nav_push(wifi);
}

void fortune_shell_open_settings_minimal(void) {
    fortune_shell_open_wifi_settings();
}
