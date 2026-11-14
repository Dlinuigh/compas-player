#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Build eapi "params=" hex ciphertext for a given /api/... path and JSON object body. */
bool netease_eapi_params(const char * api_path, const char * json_object,
                         char * out_hex, size_t out_hex_size);

/* POST to interface.music.163.com/eapi/... ; caller frees *out_body. */
bool netease_eapi_post(const char * api_path, const char * json_object,
                       const char * music_u,
                       int * out_status, uint8_t ** out_body, size_t * out_body_size);
