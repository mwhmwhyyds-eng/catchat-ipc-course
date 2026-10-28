#define _XOPEN_SOURCE 700
#include "text.h"
#include <langinfo.h>
#include <locale.h>
#include <string.h>
#include <wctype.h>

int text_init(void)
{
    if (!setlocale(LC_CTYPE, "") || strcmp(nl_langinfo(CODESET), "UTF-8")) {
        if (!setlocale(LC_CTYPE, "C.UTF-8")) return -1;
    }
    return 0;
}

static size_t decode(const char *s, size_t n, wchar_t *wc)
{
    mbstate_t state = {0};
    size_t k = mbrtowc(wc, s, n, &state);
    if (k == (size_t)-1 || k == (size_t)-2 || k == 0) return 0;
    return k;
}

int text_valid(const char *s)
{
    size_t n = strlen(s), pos = 0;
    while (pos < n) {
        wchar_t wc;
        size_t k = decode(s + pos, n - pos, &wc);
        if (!k || wc < 32 || (wc >= 127 && wc < 160) || wcwidth(wc) < 0)
            return 0;
        pos += k;
    }
    return 1;
}

int text_has_content(const char *s)
{
    size_t n = strlen(s), pos = 0;
    while (pos < n) {
        wchar_t wc;
        size_t k = decode(s + pos, n - pos, &wc);
        if (!k) return 0;
        /* Some embedded C.UTF-8 locale tables only classify ASCII spaces.
         * Include Unicode White_Space explicitly (Unicode PropList 15.1). */
        int space = iswspace(wc) || wc == 0x20 || wc == 0x85 || wc == 0xa0 ||
                    wc == 0x1680 || (wc >= 0x2000 && wc <= 0x200a) ||
                    wc == 0x2028 || wc == 0x2029 || wc == 0x202f ||
                    wc == 0x205f || wc == 0x3000;
        if (wcwidth(wc) > 0 && !space) return 1;
        pos += k;
    }
    return 0;
}

size_t text_next(const char *s, size_t pos)
{
    if (!s[pos]) return pos;
    pos++;
    while (((unsigned char)s[pos] & 0xc0) == 0x80) pos++;
    return pos;
}

size_t text_prev(const char *s, size_t pos)
{
    if (!pos) return 0;
    pos--;
    while (pos && ((unsigned char)s[pos] & 0xc0) == 0x80) pos--;
    return pos;
}

int text_width(const char *s, size_t n)
{
    size_t pos = 0;
    int width = 0;
    while (pos < n && s[pos]) {
        wchar_t wc;
        size_t k = decode(s + pos, n - pos, &wc);
        if (!k) break;
        int w = wcwidth(wc);
        if (w > 0) width += w;
        pos += k;
    }
    return width;
}

size_t text_fit(const char *s, int columns)
{
    size_t n = strlen(s), pos = 0;
    int used = 0;
    while (pos < n) {
        wchar_t wc;
        size_t k = decode(s + pos, n - pos, &wc);
        if (!k) break;
        int w = wcwidth(wc);
        if (w < 0 || used + w > columns) break;
        used += w;
        pos += k;
    }
    return pos;
}

static int word_byte(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.';
}

size_t text_wrap(const char *s, int columns)
{
    size_t n = text_fit(s, columns);
    /* Keep a number/path/English word together when it can move to the next
     * line. Long unbroken tokens still hard-wrap to guarantee progress. */
    if (n && s[n] && word_byte((unsigned char)s[n - 1]) && word_byte((unsigned char)s[n])) {
        size_t start = n;
        while (start && word_byte((unsigned char)s[start - 1])) start--;
        if (start) n = start;
    }
    return n;
}
