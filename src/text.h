#ifndef CATCHAT_TEXT_H
#define CATCHAT_TEXT_H
#include <stddef.h>
#include <wchar.h>
int text_init(void);
int text_valid(const char *s);
int text_has_content(const char *s);
size_t text_next(const char *s, size_t pos);
size_t text_prev(const char *s, size_t pos);
int text_width(const char *s, size_t n);
size_t text_fit(const char *s, int columns);
size_t text_wrap(const char *s, int columns);
#endif
