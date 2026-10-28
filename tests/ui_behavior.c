#include "text.h"
#include "ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ChatUI ui;

int main(void)
{
    assert(text_init() == 0);
    assert(text_has_content("  中文 "));
    assert(!text_has_content(" 　"));
    assert(!text_has_content("\xcc\x81")); /* combining mark alone */
    const char *numbers = "capacity 16384/16384 bytes";
    size_t n = text_wrap(numbers, 15);
    assert(n == strlen("capacity "));
    assert(text_wrap(numbers + n, 15) == strlen("16384/16384 "));
    assert(text_wrap("你好啊", 3) == strlen("你"));
    assert(text_wrap("abcdefghijk", 5) == 5); /* long tokens must progress */

    ui.cols = 116; ui.rows = 32; ui.scroll = 7;
    char long_message[151]; memset(long_message, 'x', 150); long_message[150] = 0;
    ui_add(&ui, 2, "对方", long_message, 0);
    /* This message uses two body rows plus heading/spacer. Keeping the same
     * visible content therefore requires moving the offset by four rows. */
    assert(ui.scroll == 11 && ui.unread == 1);
    ui_add(&ui, 0, "系统", "notice", 0);
    assert(ui.scroll == 14 && ui.unread == 1);
    assert(ui_key(&ui, 7) == UI_NONE);
    assert(ui.scroll == 0 && ui.unread == 0);
    ui_key(&ui, 27);
    for (const char *s = "hello"; *s; s++) ui_key(&ui, (unsigned char)*s);
    assert(!strcmp(ui.input, "hello"));
    ui_clear(&ui);
    assert(ui.count == 0 && ui.unread == 0);
    ui_stop(&ui);
    puts("PASS UTF-8 word wrapping, stable history viewport and unread indicator");
    return 0;
}
