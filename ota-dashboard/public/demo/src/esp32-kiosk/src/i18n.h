#pragma once
/*
 * Two UI languages: Indonesian (default, the source strings) and English.
 * tr("indonesian text") returns the English text when the language is EN, otherwise the same pointer.
 * Texts that are not in the table (names, numbers, already-English words) pass through unchanged.
 * Add a language by adding a column to the table in i18n.cpp.
 */
#include <stddef.h>
#include <stdint.h>

enum Lang : uint8_t { LANG_ID = 0, LANG_EN = 1 };
extern uint8_t g_lang;
const char *tr(const char *id);
