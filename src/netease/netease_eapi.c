#include "netease_eapi.h"

#include "../network/http/client/http_client.h"

#include "mbedtls/aes.h"
#include "mbedtls/md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EAPI_BASE "https://interface.music.163.com/eapi"
#define EAPI_KEY "e82ckenh8dichen8"
#define USER_AGENT "Mozilla/5.0 (Linux; Android 14; Mobile) AppleWebKit/537.36 Chrome/120.0.0.0 Mobile Safari/537.36"
#define COOKIE_BASE "os=android; appver=8.10.90"
#define MAX_RESPONSE (2 * 1024 * 1024)
#define PARAMS_HEX_MAX 16384
#define BODY_MAX 17000
#define PLAIN_MAX 8192
#define MD5_SRC_MAX 4096

static void md5_hex(const char * text, char out_hex[33]) {
    unsigned char dig[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const unsigned char *) text, strlen(text));
    mbedtls_md5_finish(&ctx, dig);
    mbedtls_md5_free(&ctx);
    for (int i = 0; i < 16; i++) snprintf(out_hex + i * 2, 3, "%02x", dig[i]);
    out_hex[32] = '\0';
}

static bool aes_ecb_hex(const char * plain, char * out_hex, size_t out_hex_size) {
    size_t len = strlen(plain);
    size_t pad = 16 - (len % 16);
    size_t total = len + pad;
    unsigned char * buf = (unsigned char *) malloc(total);
    if (!buf) return false;
    memcpy(buf, plain, len);
    memset(buf + len, (int) pad, pad);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, (const unsigned char *) EAPI_KEY, 128) != 0) {
        mbedtls_aes_free(&aes);
        free(buf);
        return false;
    }

    size_t need = total * 2 + 1;
    if (out_hex_size < need) {
        mbedtls_aes_free(&aes);
        free(buf);
        return false;
    }

    unsigned char block[16];
    size_t o = 0;
    for (size_t off = 0; off < total; off += 16) {
        if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, buf + off, block) != 0) {
            mbedtls_aes_free(&aes);
            free(buf);
            return false;
        }
        for (int i = 0; i < 16; i++) {
            snprintf(out_hex + o, 3, "%02X", block[i]);
            o += 2;
        }
    }
    out_hex[o] = '\0';
    mbedtls_aes_free(&aes);
    free(buf);
    return true;
}

bool netease_eapi_params(const char * api_path, const char * json_object,
                         char * out_hex, size_t out_hex_size) {
    if (!api_path || !json_object || !out_hex || out_hex_size < 33) return false;

    char digest[33];
    char * md5_src = (char *) malloc(MD5_SRC_MAX);
    char * plain = (char *) malloc(PLAIN_MAX);
    if (!md5_src || !plain) {
        free(md5_src);
        free(plain);
        return false;
    }

    int n = snprintf(md5_src, MD5_SRC_MAX, "nobody%suse%smd5forencrypt", api_path, json_object);
    if (n < 0 || (size_t) n >= MD5_SRC_MAX) {
        free(md5_src);
        free(plain);
        return false;
    }
    md5_hex(md5_src, digest);

    n = snprintf(plain, PLAIN_MAX, "%s-36cd479b6b5-%s-36cd479b6b5-%s",
                 api_path, json_object, digest);
    if (n < 0 || (size_t) n >= PLAIN_MAX) {
        free(md5_src);
        free(plain);
        return false;
    }
    bool ok = aes_ecb_hex(plain, out_hex, out_hex_size);
    free(md5_src);
    free(plain);
    return ok;
}

bool netease_eapi_post(const char * api_path, const char * json_object,
                       const char * music_u,
                       int * out_status, uint8_t ** out_body, size_t * out_body_size) {
    if (!api_path || !json_object || !out_status || !out_body || !out_body_size) return false;
    *out_status = 0;
    *out_body = NULL;
    *out_body_size = 0;

    /* Keep large buffers on the heap — fortune workers use a small stack, and
     * http_request_t alone is already tens of KB. Stack overflow here was
     * exiting the process (loader restart) when online calls failed/ran. */
    char * params_hex = (char *) malloc(PARAMS_HEX_MAX);
    char * body = (char *) malloc(BODY_MAX);
    char * cookie = (char *) malloc(HTTP_LONG_HEADER_VALUE_MAX);
    http_request_t * req = (http_request_t *) calloc(1, sizeof(*req));
    if (!params_hex || !body || !cookie || !req) {
        free(params_hex);
        free(body);
        free(cookie);
        free(req);
        return false;
    }

    bool ok = false;
    if (!netease_eapi_params(api_path, json_object, params_hex, PARAMS_HEX_MAX)) goto done;

    const char * eapi_path = api_path;
    if (strncmp(api_path, "/api", 4) == 0) eapi_path = api_path + 4;

    if (snprintf(req->url, sizeof(req->url), "%s%s", EAPI_BASE, eapi_path) >= (int) sizeof(req->url))
        goto done;

    int body_len = snprintf(body, BODY_MAX, "params=%s", params_hex);
    if (body_len < 0 || (size_t) body_len >= BODY_MAX) goto done;

    if (music_u && music_u[0]) {
        if (snprintf(cookie, HTTP_LONG_HEADER_VALUE_MAX, "%s; MUSIC_U=%s", COOKIE_BASE, music_u)
            >= (int) HTTP_LONG_HEADER_VALUE_MAX)
            goto done;
    } else {
        snprintf(cookie, HTTP_LONG_HEADER_VALUE_MAX, "%s", COOKIE_BASE);
    }

    req->method = HTTP_METHOD_POST;
    req->content_type = "application/x-www-form-urlencoded";
    req->body = (const uint8_t *) body;
    req->body_len = (size_t) body_len;
    req->verify_tls = true;
    req->connect_timeout_ms = 10000;
    req->total_timeout_ms = 20000;
    req->max_response_bytes = MAX_RESPONSE;
    req->header_count = 1;
    snprintf(req->headers[0].name, sizeof(req->headers[0].name), "User-Agent");
    snprintf(req->headers[0].value, sizeof(req->headers[0].value), "%s", USER_AGENT);
    snprintf(req->long_header_name, sizeof(req->long_header_name), "Cookie");
    req->long_header_value = cookie;

    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!http_request_ex(req, NULL, &resp)) {
        http_response_free(&resp);
        goto done;
    }
    *out_status = resp.status;
    *out_body = resp.body;
    *out_body_size = resp.body_len;
    resp.body = NULL;
    http_response_free(&resp);
    ok = true;

done:
    free(params_hex);
    free(body);
    free(cookie);
    free(req);
    return ok;
}
