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
    char md5_src[4096];
    int n = snprintf(md5_src, sizeof(md5_src), "nobody%suse%smd5forencrypt", api_path, json_object);
    if (n < 0 || (size_t) n >= sizeof(md5_src)) return false;
    md5_hex(md5_src, digest);

    char plain[8192];
    n = snprintf(plain, sizeof(plain), "%s-36cd479b6b5-%s-36cd479b6b5-%s",
                 api_path, json_object, digest);
    if (n < 0 || (size_t) n >= sizeof(plain)) return false;
    return aes_ecb_hex(plain, out_hex, out_hex_size);
}

bool netease_eapi_post(const char * api_path, const char * json_object,
                       const char * music_u,
                       int * out_status, uint8_t ** out_body, size_t * out_body_size) {
    if (!api_path || !json_object || !out_status || !out_body || !out_body_size) return false;
    *out_status = 0;
    *out_body = NULL;
    *out_body_size = 0;

    char params_hex[16384];
    if (!netease_eapi_params(api_path, json_object, params_hex, sizeof(params_hex))) return false;

    const char * eapi_path = api_path;
    if (strncmp(api_path, "/api", 4) == 0) eapi_path = api_path + 4;

    char url[512];
    if (snprintf(url, sizeof(url), "%s%s", EAPI_BASE, eapi_path) >= (int) sizeof(url)) return false;

    char body[17000];
    int body_len = snprintf(body, sizeof(body), "params=%s", params_hex);
    if (body_len < 0 || (size_t) body_len >= sizeof(body)) return false;

    char cookie[HTTP_LONG_HEADER_VALUE_MAX];
    if (music_u && music_u[0]) {
        if (snprintf(cookie, sizeof(cookie), "%s; MUSIC_U=%s", COOKIE_BASE, music_u) >= (int) sizeof(cookie))
            return false;
    } else {
        snprintf(cookie, sizeof(cookie), "%s", COOKIE_BASE);
    }

    http_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.url, sizeof(req.url), "%s", url);
    req.method = HTTP_METHOD_POST;
    req.content_type = "application/x-www-form-urlencoded";
    req.body = (const uint8_t *) body;
    req.body_len = (size_t) body_len;
    req.verify_tls = true;
    req.connect_timeout_ms = 10000;
    req.total_timeout_ms = 20000;
    req.max_response_bytes = MAX_RESPONSE;
    req.header_count = 1;
    snprintf(req.headers[0].name, sizeof(req.headers[0].name), "User-Agent");
    snprintf(req.headers[0].value, sizeof(req.headers[0].value), "%s", USER_AGENT);
    snprintf(req.long_header_name, sizeof(req.long_header_name), "Cookie");
    req.long_header_value = cookie;

    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!http_request_ex(&req, NULL, &resp)) {
        http_response_free(&resp);
        return false;
    }
    *out_status = resp.status;
    *out_body = resp.body;
    *out_body_size = resp.body_len;
    resp.body = NULL;
    http_response_free(&resp);
    return true;
}
