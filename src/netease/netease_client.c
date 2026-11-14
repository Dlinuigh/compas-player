#include "netease_client.h"
#include "netease_eapi.h"

#include "../network/http/client/http_client.h"

#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define SESSION_PATH MUSIC_ROOT_DIR "/ForTune/session.txt"
#define SESSION_PATH_LEGACY MUSIC_ROOT_DIR "/NetEase/session.txt"
#define SECRETS_PATH MUSIC_ROOT_DIR "/.compas/fortune_netease.txt"

static void ensure_parent_dirs(void) {
    mkdir(MUSIC_ROOT_DIR "/.compas", 0755);
    mkdir(MUSIC_ROOT_DIR "/ForTune", 0755);
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_music_u[HTTP_LONG_HEADER_VALUE_MAX];
static char g_nickname[128];
static char g_user_id[32];
static bool g_ready;

static void clip(char * dst, size_t dst_size, const char * src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, dst_size, "%s", src);
}

static void save_secrets(void) {
    ensure_parent_dirs();
    FILE * f = fopen(SECRETS_PATH, "wb");
    if (!f) return;
    fprintf(f, "MUSIC_U=%s\nuser_id=%s\nnickname=%s\n", g_music_u, g_user_id, g_nickname);
    fclose(f);
}

static void load_secrets(void) {
    FILE * f = fopen(SECRETS_PATH, "rb");
    if (!f) return;
    char line[HTTP_LONG_HEADER_VALUE_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        char * nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "MUSIC_U=", 8) == 0) clip(g_music_u, sizeof(g_music_u), line + 8);
        else if (strncmp(line, "user_id=", 8) == 0) clip(g_user_id, sizeof(g_user_id), line + 8);
        else if (strncmp(line, "nickname=", 9) == 0) clip(g_nickname, sizeof(g_nickname), line + 9);
    }
    fclose(f);
}

static char * parse_music_u(const char * text) {
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    const char * p = strstr(text, "MUSIC_U=");
    if (p) {
        p += 8;
        size_t n = strcspn(p, "; \t\r\n");
        if (n == 0) return NULL;
        char * out = (char *) malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, p, n);
        out[n] = '\0';
        return out;
    }
    if (text[0] && !strpbrk(text, ";= \t")) {
        size_t n = strcspn(text, "\r\n");
        char * out = (char *) malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, text, n);
        out[n] = '\0';
        return out;
    }
    return NULL;
}

static bool try_import_path(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); unlink(path); return false; }
    char * buf = (char *) malloc((size_t) sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    buf[n] = '\0';
    unlink(path);
    char * u = parse_music_u(buf);
    free(buf);
    if (!u) return false;
    pthread_mutex_lock(&g_mu);
    clip(g_music_u, sizeof(g_music_u), u);
    g_nickname[0] = '\0';
    g_user_id[0] = '\0';
    save_secrets();
    pthread_mutex_unlock(&g_mu);
    free(u);
    return true;
}

static cJSON * eapi_json(const char * api, const char * json_obj) {
    char music_u[HTTP_LONG_HEADER_VALUE_MAX];
    pthread_mutex_lock(&g_mu);
    clip(music_u, sizeof(music_u), g_music_u);
    pthread_mutex_unlock(&g_mu);

    int status = 0;
    uint8_t * body = NULL;
    size_t body_len = 0;
    if (!netease_eapi_post(api, json_obj, music_u[0] ? music_u : NULL, &status, &body, &body_len))
        return NULL;
    if (status != 200 || !body) {
        free(body);
        return NULL;
    }
    cJSON * root = cJSON_ParseWithLength((char *) body, body_len);
    free(body);
    if (!root) return NULL;
    cJSON * code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code) && code->valuedouble != 200.0) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static void artists_join(cJSON * ar, char * out, size_t out_size) {
    out[0] = '\0';
    if (!cJSON_IsArray(ar)) return;
    size_t used = 0;
    cJSON * a;
    cJSON_ArrayForEach(a, ar) {
        cJSON * name = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (!cJSON_IsString(name) || !name->valuestring) continue;
        if (used > 0 && used + 3 < out_size) {
            memcpy(out + used, " / ", 3);
            used += 3;
        }
        size_t len = strlen(name->valuestring);
        if (used + len >= out_size) break;
        memcpy(out + used, name->valuestring, len);
        used += len;
        out[used] = '\0';
    }
}

static bool fill_track(cJSON * song, netease_track_t * t) {
    if (!cJSON_IsObject(song) || !t) return false;
    memset(t, 0, sizeof(*t));
    cJSON * id = cJSON_GetObjectItemCaseSensitive(song, "id");
    if (cJSON_IsNumber(id))
        snprintf(t->id, sizeof(t->id), "%.0f", id->valuedouble);
    else if (cJSON_IsString(id) && id->valuestring)
        clip(t->id, sizeof(t->id), id->valuestring);
    else
        return false;

    cJSON * name = cJSON_GetObjectItemCaseSensitive(song, "name");
    if (cJSON_IsString(name)) clip(t->title, sizeof(t->title), name->valuestring);

    cJSON * ar = cJSON_GetObjectItemCaseSensitive(song, "ar");
    if (!ar) ar = cJSON_GetObjectItemCaseSensitive(song, "artists");
    artists_join(ar, t->artist, sizeof(t->artist));

    cJSON * al = cJSON_GetObjectItemCaseSensitive(song, "al");
    if (!al) al = cJSON_GetObjectItemCaseSensitive(song, "album");
    if (cJSON_IsObject(al)) {
        cJSON * an = cJSON_GetObjectItemCaseSensitive(al, "name");
        if (cJSON_IsString(an)) clip(t->album, sizeof(t->album), an->valuestring);
        cJSON * pic = cJSON_GetObjectItemCaseSensitive(al, "picUrl");
        if (cJSON_IsString(pic) && pic->valuestring) {
            if (strstr(pic->valuestring, "param="))
                clip(t->cover, sizeof(t->cover), pic->valuestring);
            else
                snprintf(t->cover, sizeof(t->cover), "%s%cparam=320y320",
                         pic->valuestring, strchr(pic->valuestring, '?') ? '&' : '?');
        }
    }

    cJSON * dt = cJSON_GetObjectItemCaseSensitive(song, "dt");
    if (cJSON_IsNumber(dt) && dt->valuedouble > 0)
        t->duration_ms = (uint32_t) dt->valuedouble;
    return true;
}

static int fill_track_array(cJSON * arr, netease_track_t * out, int cap) {
    if (!cJSON_IsArray(arr) || !out || cap <= 0) return 0;
    int n = 0;
    cJSON * song;
    cJSON_ArrayForEach(song, arr) {
        if (n >= cap) break;
        if (fill_track(song, &out[n])) n++;
    }
    return n;
}

void netease_client_init(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_ready) {
        g_music_u[0] = g_nickname[0] = g_user_id[0] = '\0';
        load_secrets();
        g_ready = true;
    }
    pthread_mutex_unlock(&g_mu);
    netease_client_poll_session_file();
}

void netease_client_poll_session_file(void) {
    (void) try_import_path(SESSION_PATH);
    (void) try_import_path(SESSION_PATH_LEGACY);
}

bool netease_client_refresh_account(void) {
    if (!netease_client_logged_in()) return false;
    cJSON * root = eapi_json("/api/nuser/account/get", "{}");
    if (!root) return false;
    cJSON * profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    bool ok = false;
    if (cJSON_IsObject(profile)) {
        cJSON * nick = cJSON_GetObjectItemCaseSensitive(profile, "nickname");
        cJSON * uid = cJSON_GetObjectItemCaseSensitive(profile, "userId");
        pthread_mutex_lock(&g_mu);
        if (cJSON_IsString(nick)) clip(g_nickname, sizeof(g_nickname), nick->valuestring);
        if (cJSON_IsNumber(uid)) snprintf(g_user_id, sizeof(g_user_id), "%.0f", uid->valuedouble);
        save_secrets();
        pthread_mutex_unlock(&g_mu);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool netease_client_logged_in(void) {
    pthread_mutex_lock(&g_mu);
    bool ok = g_music_u[0] != '\0';
    pthread_mutex_unlock(&g_mu);
    return ok;
}

const char * netease_client_nickname(void) {
    return g_nickname[0] ? g_nickname : (g_music_u[0] ? "已登录" : "");
}

void netease_client_logout(void) {
    pthread_mutex_lock(&g_mu);
    g_music_u[0] = g_nickname[0] = g_user_id[0] = '\0';
    unlink(SECRETS_PATH);
    pthread_mutex_unlock(&g_mu);
}

bool netease_client_search_songs(const char * query, netease_track_t * out, int cap, int * out_count) {
    if (out_count) *out_count = 0;
    if (!query || !query[0] || !out || cap <= 0) return false;
    char escaped[256];
    /* Minimal JSON string escape for search keyword. */
    size_t o = 0;
    escaped[o++] = '"';
    for (const char * p = query; *p && o + 6 < sizeof(escaped); p++) {
        if (*p == '"' || *p == '\\') { escaped[o++] = '\\'; escaped[o++] = *p; }
        else if ((unsigned char) *p < 0x20) continue;
        else escaped[o++] = *p;
    }
    escaped[o++] = '"';
    escaped[o] = '\0';

    char json[384];
    snprintf(json, sizeof(json),
             "{\"s\":%s,\"type\":\"1\",\"limit\":\"30\",\"offset\":\"0\",\"total\":\"true\"}",
             escaped);
    cJSON * root = eapi_json("/api/cloudsearch/pc", json);
    if (!root) return false;
    cJSON * result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON * songs = result ? cJSON_GetObjectItemCaseSensitive(result, "songs") : NULL;
    int n = fill_track_array(songs, out, cap);
    cJSON_Delete(root);
    if (out_count) *out_count = n;
    return n > 0;
}

bool netease_client_daily_songs(netease_track_t * out, int cap, int * out_count) {
    if (out_count) *out_count = 0;
    if (!out || cap <= 0) return false;
    cJSON * root = eapi_json("/api/v3/discovery/recommend/songs", "{}");
    if (!root) return false;
    cJSON * data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON * daily = data ? cJSON_GetObjectItemCaseSensitive(data, "dailySongs") : NULL;
    int n = fill_track_array(daily, out, cap);
    cJSON_Delete(root);
    if (out_count) *out_count = n;
    return n > 0;
}

bool netease_client_user_playlists(netease_playlist_t * out, int cap, int * out_count) {
    if (out_count) *out_count = 0;
    if (!out || cap <= 0) return false;
    char uid[32];
    pthread_mutex_lock(&g_mu);
    clip(uid, sizeof(uid), g_user_id);
    pthread_mutex_unlock(&g_mu);
    if (!uid[0]) {
        cJSON * acc = eapi_json("/api/nuser/account/get", "{}");
        if (!acc) return false;
        cJSON * profile = cJSON_GetObjectItemCaseSensitive(acc, "profile");
        cJSON * id = profile ? cJSON_GetObjectItemCaseSensitive(profile, "userId") : NULL;
        if (cJSON_IsNumber(id)) {
            snprintf(uid, sizeof(uid), "%.0f", id->valuedouble);
            pthread_mutex_lock(&g_mu);
            clip(g_user_id, sizeof(g_user_id), uid);
            cJSON * nick = cJSON_GetObjectItemCaseSensitive(profile, "nickname");
            if (cJSON_IsString(nick)) clip(g_nickname, sizeof(g_nickname), nick->valuestring);
            save_secrets();
            pthread_mutex_unlock(&g_mu);
        }
        cJSON_Delete(acc);
        if (!uid[0]) return false;
    }

    char json[160];
    snprintf(json, sizeof(json),
             "{\"uid\":\"%s\",\"limit\":\"50\",\"offset\":\"0\",\"includeVideo\":\"false\"}", uid);
    cJSON * root = eapi_json("/api/user/playlist", json);
    if (!root) return false;
    cJSON * pls = cJSON_GetObjectItemCaseSensitive(root, "playlist");
    int n = 0;
    if (cJSON_IsArray(pls)) {
        cJSON * pl;
        cJSON_ArrayForEach(pl, pls) {
            if (n >= cap) break;
            memset(&out[n], 0, sizeof(out[n]));
            cJSON * id = cJSON_GetObjectItemCaseSensitive(pl, "id");
            cJSON * name = cJSON_GetObjectItemCaseSensitive(pl, "name");
            cJSON * cover = cJSON_GetObjectItemCaseSensitive(pl, "coverImgUrl");
            cJSON * count = cJSON_GetObjectItemCaseSensitive(pl, "trackCount");
            if (cJSON_IsNumber(id)) snprintf(out[n].id, sizeof(out[n].id), "%.0f", id->valuedouble);
            else continue;
            if (cJSON_IsString(name)) clip(out[n].name, sizeof(out[n].name), name->valuestring);
            if (cJSON_IsString(cover)) clip(out[n].cover, sizeof(out[n].cover), cover->valuestring);
            if (cJSON_IsNumber(count)) out[n].track_count = (int) count->valuedouble;
            n++;
        }
    }
    cJSON_Delete(root);
    if (out_count) *out_count = n;
    return n > 0;
}

bool netease_client_playlist_tracks(const char * playlist_id, netease_track_t * out, int cap, int * out_count) {
    if (out_count) *out_count = 0;
    if (!playlist_id || !out || cap <= 0) return false;
    char json[128];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"n\":\"0\",\"s\":\"0\"}", playlist_id);
    cJSON * root = eapi_json("/api/v6/playlist/detail", json);
    if (!root) return false;
    cJSON * playlist = cJSON_GetObjectItemCaseSensitive(root, "playlist");
    cJSON * track_ids = playlist ? cJSON_GetObjectItemCaseSensitive(playlist, "trackIds") : NULL;
    if (!cJSON_IsArray(track_ids)) {
        cJSON_Delete(root);
        return false;
    }

    /* Batch song detail: up to DETAIL_BATCH ids. */
    char c_buf[2048];
    size_t used = 0;
    c_buf[used++] = '[';
    int id_count = 0;
    cJSON * tid;
    cJSON_ArrayForEach(tid, track_ids) {
        if (id_count >= cap || id_count >= 40) break;
        cJSON * id = cJSON_GetObjectItemCaseSensitive(tid, "id");
        double v = cJSON_IsNumber(id) ? id->valuedouble :
                   (cJSON_IsNumber(tid) ? tid->valuedouble : 0);
        if (v <= 0) continue;
        int wr = snprintf(c_buf + used, sizeof(c_buf) - used, "%s{\"id\":%.0f}",
                          id_count ? "," : "", v);
        if (wr < 0 || used + (size_t) wr >= sizeof(c_buf)) break;
        used += (size_t) wr;
        id_count++;
    }
    if (used + 1 >= sizeof(c_buf)) { cJSON_Delete(root); return false; }
    c_buf[used++] = ']';
    c_buf[used] = '\0';
    cJSON_Delete(root);

    char detail_json[2200];
    snprintf(detail_json, sizeof(detail_json), "{\"c\":%s}", c_buf);
    cJSON * detail = eapi_json("/api/v3/song/detail", detail_json);
    if (!detail) return false;
    cJSON * songs = cJSON_GetObjectItemCaseSensitive(detail, "songs");
    int n = fill_track_array(songs, out, cap);
    cJSON_Delete(detail);
    if (out_count) *out_count = n;
    return n > 0;
}

bool netease_client_liked_songs(netease_track_t * out, int cap, int * out_count) {
    netease_playlist_t pls[8];
    int pc = 0;
    if (!netease_client_user_playlists(pls, 8, &pc) || pc <= 0) return false;
    /* First playlist is usually "liked songs". */
    return netease_client_playlist_tracks(pls[0].id, out, cap, out_count);
}

bool netease_client_resolve_url(const char * track_id, char * url_out, size_t url_size,
                                char * codec_out, size_t codec_size) {
    if (!track_id || !url_out || url_size == 0) return false;
    url_out[0] = '\0';
    if (codec_out && codec_size) codec_out[0] = '\0';
    char json[160];
    snprintf(json, sizeof(json),
             "{\"ids\":\"[%s]\",\"level\":\"exhigh\",\"encodeType\":\"mp3\"}", track_id);
    cJSON * root = eapi_json("/api/song/enhance/player/url/v1", json);
    if (!root) return false;
    cJSON * data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = false;
    if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
        cJSON * first = cJSON_GetArrayItem(data, 0);
        cJSON * url = cJSON_GetObjectItemCaseSensitive(first, "url");
        cJSON * type = cJSON_GetObjectItemCaseSensitive(first, "type");
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
            clip(url_out, url_size, url->valuestring);
            if (codec_out && codec_size) {
                if (cJSON_IsString(type) && type->valuestring)
                    clip(codec_out, codec_size, type->valuestring);
                else
                    clip(codec_out, codec_size, "mp3");
            }
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool netease_client_build_remote_queue(const netease_track_t * tracks, int count, int start_index,
                                       remote_track_meta_t ** out_metas, int * out_count,
                                       int * out_start_index) {
    if (out_metas) *out_metas = NULL;
    if (out_count) *out_count = 0;
    if (out_start_index) *out_start_index = 0;
    if (!tracks || count <= 0 || !out_metas || !out_count) return false;
    if (start_index < 0) start_index = 0;
    if (start_index >= count) start_index = 0;

    /* Resolve a short window only — full-list URL fetch on a weak link was
     * hanging/failing and inviting stack/OOM pressure. */
    enum { WINDOW = 8 };
    remote_track_meta_t * metas = (remote_track_meta_t *) calloc(WINDOW, sizeof(*metas));
    if (!metas) return false;

    int ok_n = 0;
    int play_at = 0;
    for (int n = 0; n < WINDOW && n < count; n++) {
        int i = (start_index + n) % count;
        remote_track_meta_t * m = &metas[ok_n];
        memset(m, 0, sizeof(*m));
        clip(m->provider, sizeof(m->provider), "netease");
        clip(m->track_id, sizeof(m->track_id), tracks[i].id);
        clip(m->title, sizeof(m->title), tracks[i].title);
        clip(m->artist, sizeof(m->artist), tracks[i].artist);
        clip(m->album, sizeof(m->album), tracks[i].album);
        m->duration_ms = tracks[i].duration_ms;
        clip(m->artwork_url, sizeof(m->artwork_url), tracks[i].cover);
        m->verify_tls = true;
        m->resolve_stream = false;

        char codec[16] = "mp3";
        if (!netease_client_resolve_url(tracks[i].id, m->stream_url, sizeof(m->stream_url),
                                        codec, sizeof(codec))) {
            continue;
        }
        clip(m->codec, sizeof(m->codec), codec);
        if (n == 0) play_at = ok_n;
        ok_n++;
    }

    if (ok_n <= 0) {
        free(metas);
        return false;
    }
    *out_metas = metas;
    *out_count = ok_n;
    if (out_start_index) *out_start_index = play_at;
    return true;
}
