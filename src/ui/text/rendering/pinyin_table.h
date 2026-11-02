#ifndef PINYIN_TABLE_H
#define PINYIN_TABLE_H

#include <stdbool.h>

/* Toneless single-character pinyin. `v` stands for ü. */
bool pinyin_is_prefix(const char *s);
bool pinyin_is_exact(const char *s);
int pinyin_candidate_count(const char *exact);
/* Writes one NUL-terminated UTF-8 character. */
bool pinyin_candidate_utf8(const char *exact, int index, char out[4]);

#endif
