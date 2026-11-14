#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../library/remote/remote_track.h"

#define NETEASE_TITLE_MAX 128
#define NETEASE_ARTIST_MAX 128
#define NETEASE_ID_MAX 32
#define NETEASE_COVER_MAX 512
#define NETEASE_LIST_MAX 80

typedef struct {
    char id[NETEASE_ID_MAX];
    char title[NETEASE_TITLE_MAX];
    char artist[NETEASE_ARTIST_MAX];
    char album[NETEASE_TITLE_MAX];
    char cover[NETEASE_COVER_MAX];
    uint32_t duration_ms;
} netease_track_t;

typedef struct {
    char id[NETEASE_ID_MAX];
    char name[NETEASE_TITLE_MAX];
    char cover[NETEASE_COVER_MAX];
    int track_count;
} netease_playlist_t;

void netease_client_init(void);
/* UI-safe: only reads/moves session files; no network. */
void netease_client_poll_session_file(void);
/* Worker only: refresh nickname/uid after a new MUSIC_U import. */
bool netease_client_refresh_account(void);

bool netease_client_logged_in(void);
const char * netease_client_nickname(void);
void netease_client_logout(void);

/* Blocking network calls — worker thread only. */
bool netease_client_search_songs(const char * query, netease_track_t * out, int cap, int * out_count);
bool netease_client_daily_songs(netease_track_t * out, int cap, int * out_count);
bool netease_client_user_playlists(netease_playlist_t * out, int cap, int * out_count);
bool netease_client_playlist_tracks(const char * playlist_id, netease_track_t * out, int cap, int * out_count);
bool netease_client_liked_songs(netease_track_t * out, int cap, int * out_count);
bool netease_client_resolve_url(const char * track_id, char * url_out, size_t url_size, char * codec_out, size_t codec_size);

/* Worker: resolve a short window of stream URLs. Caller frees *out_metas.
 * Do NOT call gui_plugin_play_remote_tracks from the worker — UI thread only. */
bool netease_client_build_remote_queue(const netease_track_t * tracks, int count, int start_index,
                                       remote_track_meta_t ** out_metas, int * out_count,
                                       int * out_start_index);
