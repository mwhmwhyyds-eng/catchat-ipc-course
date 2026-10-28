#ifndef CATCHAT_UI_H
#define CATCHAT_UI_H
#include "ipc.h"
#include <signal.h>
#include <termios.h>
#define UI_HISTORY 300
enum { UI_NONE, UI_SUBMIT, UI_QUIT, UI_HELP };
typedef struct {
    int kind; /* 0 system, 1 self, 2 peer */
    char name[CHAT_NAME];
    char time[16];
    char text[CHAT_LIMIT + 1];
} ChatEntry;
typedef struct {
    int plain, active, rows, cols, scroll, dirty, count, first;
    unsigned long unread, export_serial;
    int notice_level;
    struct termios original;
    ChatEntry entries[UI_HISTORY];
    char input[CHAT_LIMIT + 1], notice[256];
    size_t cursor, length;
    char escape[32], utf8[8];
    size_t escape_n, utf8_n, utf8_need;
    int paste, overflow;
} ChatUI;
int ui_start(ChatUI *u, int plain);
void ui_stop(ChatUI *u);
void ui_add(ChatUI *u, int kind, const char *name, const char *text, int64_t timestamp);
void ui_notice(ChatUI *u, const char *text);
void ui_success(ChatUI *u, const char *text);
void ui_render(ChatUI *u, ChatIPC *c, const ChatPeer *peer);
int ui_key(ChatUI *u, unsigned char key);
void ui_reset_input(ChatUI *u);
void ui_clear(ChatUI *u);
int ui_save(ChatUI *u, char *path, size_t size);
#endif
