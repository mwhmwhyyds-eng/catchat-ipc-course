#define _GNU_SOURCE
#include "ui.h"
#include "text.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BASE "\033[0;38;5;252;48;5;234m"
#define MUTED "\033[38;5;245m"
#define CYAN "\033[38;5;81m"
#define GREEN "\033[38;5;114m"
#define AMBER "\033[38;5;221m"
#define HEADER "\033[1;38;5;255;48;5;24m"

typedef struct { int entry; size_t offset, length; int heading; } VisualLine;
static VisualLine *visual_lines;
static size_t visual_capacity;

static int message_lines(const char *text, int width)
{
    int count = 2; /* sender/time heading and one spacer */
    size_t pos = 0, len = strlen(text);
    while (pos < len) {
        size_t n = text_wrap(text + pos, width);
        if (!n) n = text_next(text, pos) - pos;
        pos += n; count++;
    }
    return count;
}

int ui_start(ChatUI *u, int plain)
{
    memset(u, 0, sizeof(*u)); u->plain = plain; u->dirty = 1;
    snprintf(u->notice, sizeof(u->notice), "Enter 发送  |  /help 帮助  |  Ctrl+Q 退出");
    if (plain) return 0;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { errno = ENOTTY; return -1; }
    if (tcgetattr(STDIN_FILENO, &u->original)) return -1;
    struct termios raw = u->original;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) return -1;
    u->active = 1;
    printf("\033[?1049h\033[?2004h\033[?25l"); fflush(stdout);
    return 0;
}

void ui_stop(ChatUI *u)
{
    free(visual_lines); visual_lines = NULL; visual_capacity = 0;
    if (!u->active) return;
    printf("\033[0m\033[?2004l\033[?25h\033[?1049l"); fflush(stdout);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &u->original);
    u->active = 0;
}

void ui_add(ChatUI *u, int kind, const char *name, const char *text, int64_t timestamp)
{
    if (u->count == UI_HISTORY) { u->first = (u->first + 1) % UI_HISTORY; u->count--; }
    ChatEntry *e = &u->entries[(u->first + u->count++) % UI_HISTORY];
    e->kind = kind;
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->text, sizeof(e->text), "%s", text);
    time_t t = (time_t)timestamp;
    struct tm tm;
    if (localtime_r(&t, &tm)) strftime(e->time, sizeof(e->time), "%H:%M:%S", &tm);
    else snprintf(e->time, sizeof(e->time), "--:--:--");
    if (u->scroll) {
        int side = u->cols >= 106 && u->rows >= 26 ? 28 : 0;
        int width = u->cols - side - 6;
        if (width < 1) width = 54;
        u->scroll += message_lines(text, width);
        if (kind == 2) u->unread++;
    }
    u->dirty = 1;
    if (u->plain) {
        if (kind == 2) printf("RECV %s %s\n", name, text);
        else if (kind == 0) printf("INFO %s\n", text);
    }
}

void ui_notice(ChatUI *u, const char *text)
{
    snprintf(u->notice, sizeof(u->notice), "%s", text); u->dirty = 1;
    u->notice_level = 2;
    if (u->plain) printf("NOTICE %s\n", text);
}

void ui_success(ChatUI *u, const char *text)
{
    ui_notice(u, text); u->notice_level = 1;
}

static void at(int row, int col) { printf("\033[%d;%dH", row, col); }

static void clipped(const char *s, int width)
{
    if (width <= 0) return;
    size_t n = text_fit(s, width);
    fwrite(s, 1, n, stdout);
}

static void line(int row, int col, const char *color, const char *text, int width)
{
    at(row, col); fputs(color, stdout); clipped(text, width); fputs(BASE, stdout);
}

static int visual_append(int *total, VisualLine value)
{
    if ((size_t)*total == visual_capacity) {
        size_t capacity = visual_capacity ? visual_capacity * 2 : 1024;
        VisualLine *grown = realloc(visual_lines, capacity * sizeof(*grown));
        if (!grown) return -1;
        visual_lines = grown; visual_capacity = capacity;
    }
    visual_lines[(*total)++] = value;
    return 0;
}

static void history(ChatUI *u, int width, int top, int height)
{
    int total = 0;
    for (int j = 0; j < u->count; j++) {
        int idx = (u->first + j) % UI_HISTORY;
        ChatEntry *e = &u->entries[idx];
        if (visual_append(&total, (VisualLine){idx, 0, 0, 1})) goto out_of_memory;
        size_t pos = 0, len = strlen(e->text);
        while (pos < len) {
            size_t n = text_wrap(e->text + pos, width - 4);
            if (!n) n = text_next(e->text, pos) - pos;
            if (visual_append(&total, (VisualLine){idx, pos, n, 0})) goto out_of_memory;
            pos += n;
        }
        if (visual_append(&total, (VisualLine){idx, 0, 0, 2})) goto out_of_memory;
    }
    int max_scroll = total > height ? total - height : 0;
    if (u->scroll > max_scroll) u->scroll = max_scroll;
    if (u->scroll < 0) u->scroll = 0;
    if (!u->scroll) u->unread = 0;
    int start = total - height - u->scroll;
    if (start < 0) start = 0;
    for (int row = 0; row < height && start + row < total; row++) {
        VisualLine *v = &visual_lines[start + row];
        ChatEntry *e = &u->entries[v->entry];
        at(top + row, 3);
        if (v->heading == 2) continue;
        if (v->heading) {
            char label[128];
            snprintf(label, sizeof(label), "%s  %s%s", e->time, e->name,
                     e->kind == 1 ? " · 我" : "");
            fputs(e->kind == 1 ? GREEN : e->kind == 2 ? CYAN : MUTED, stdout);
            clipped(label, width - 3);
        } else {
            fputs(e->kind ? BASE : MUTED, stdout);
            fputs("  ", stdout); fwrite(e->text + v->offset, 1, v->length, stdout);
        }
        fputs(BASE, stdout);
    }
    return;
out_of_memory:
    line(top, 3, AMBER, "历史显示内存不足；仍可编辑输入或退出", width - 3);
}

void ui_render(ChatUI *u, ChatIPC *c, const ChatPeer *peer)
{
    if (u->plain) return;
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)) { ws.ws_row = 24; ws.ws_col = 80; }
    int rows = ws.ws_row ? ws.ws_row : 24;
    int cols = ws.ws_col ? ws.ws_col : 80;
    if (rows > 150) rows = 150;
    if (cols > 240) cols = 240;
    if (rows != u->rows || cols != u->cols) u->dirty = 1;
    u->rows = rows; u->cols = cols;
    if (!u->dirty) return;
    u->dirty = 0;
    printf("\033[?25l" BASE "\033[H");
    /* Full rows avoid remnants after resize; never print the last column. */
    for (int r = 1; r <= rows; r++) { at(r, 1); printf("\033[2K"); }
    if (cols < 60 || rows < 18) {
        line(2, 2, AMBER, "窗口过小：请调整至至少 60 列 × 18 行", cols - 3);
        line(4, 2, MUTED, "通信保持运行；Ctrl+Q 退出", cols - 3);
        fflush(stdout); return;
    }
    at(1, 1); fputs(HEADER, stdout);
    for (int i = 0; i < cols - 1; i++) putchar(' ');
    char buf[256];
    snprintf(buf, sizeof(buf), "CatChat %s  /  本地消息队列通信", CHAT_VERSION);
    line(1, 2, HEADER, buf, cols - 14);
    snprintf(buf, sizeof(buf), "终端 %c", 'A' + c->role);
    line(1, cols - 9, HEADER, buf, 8);
    snprintf(buf, sizeof(buf), "房间 %s  ·  System V IPC", c->room);
    line(2, 3, MUTED, buf, cols - 5);
    snprintf(buf, sizeof(buf), "%s  →  %s   [%s]", c->name,
             peer->online ? peer->name : "等待另一端加入", peer->online ? "在线" : "离线");
    line(3, 3, peer->online ? GREEN : AMBER, buf, cols - 5);
    int side = cols >= 106 && rows >= 26 ? 28 : 0;
    int chat_width = cols - side - 2;
    int input_top = rows - 5;
    history(u, chat_width, 5, input_top - 6);
    if (side) {
        int x = cols - side + 2;
        for (int r = 3; r < input_top; r++) line(r, x - 2, MUTED, "│", 1);
        line(5, x, CYAN, "IPC 观测", side - 3);
        line(7, x, MUTED, "System V · 双向队列", side - 3);
        snprintf(buf, sizeof(buf), "接收队列  #%d", c->queues[c->role]);
        line(9, x, BASE, buf, side - 3);
        snprintf(buf, sizeof(buf), "发送队列  #%d", c->queues[1 - c->role]);
        line(10, x, BASE, buf, side - 3);
        unsigned long pending[2], capacity[2];
        if (!ipc_stats(c, pending, capacity)) {
            snprintf(buf, sizeof(buf), "待收 %lu   待发 %lu", pending[c->role], pending[1 - c->role]);
            line(12, x, MUTED, buf, side - 3);
        }
        snprintf(buf, sizeof(buf), "已入队  %lu", c->sent);
        line(14, x, BASE, buf, side - 3);
        snprintf(buf, sizeof(buf), "已收到  %lu", c->received);
        line(15, x, BASE, buf, side - 3);
        line(16, x, MUTED, "队列权限  0600", side - 3);
        line(18, x, MUTED, "PgUp / PgDn 翻阅", side - 3);
        if (input_top > 21) line(20, x, MUTED, "F1 查看快捷键", side - 3);
    }
    at(input_top, 2); fputs(MUTED, stdout);
    for (int i = 0; i < cols - 3; i++) putchar('-');
    if (u->scroll)
        snprintf(buf, sizeof(buf), "消息  %zu/%d 字节  [历史 · %lu 条新消息]", u->length, CHAT_LIMIT, u->unread);
    else snprintf(buf, sizeof(buf), "消息  %zu/%d 字节", u->length, CHAT_LIMIT);
    line(input_top + 1, 3, CYAN, buf, cols - 5);
    line(input_top + 2, 3, GREEN, "> ", 2);
    size_t start = 0;
    int available = cols - 8;
    while (text_width(u->input + start, u->cursor - start) >= available)
        start = text_next(u->input, start);
    if (u->length) line(input_top + 2, 5, BASE, u->input + start, available);
    else line(input_top + 2, 5, MUTED, "输入消息…  /help 查看命令", available);
    const char *notice_color = u->notice_level == 1 ? GREEN : u->notice_level == 2 ? AMBER : MUTED;
    line(rows - 1, 3, notice_color, u->notice, cols - 5);
    const char *footer = u->scroll ? "PgUp/PgDn 翻阅  Ctrl+G 回到底部  Ctrl+Q 退出" :
                         cols < 86 ? "Enter 发送  /help 帮助  Ctrl+Q 退出" :
                         "Enter 发送   /help 帮助   /save 导出   Ctrl+Q 退出";
    line(rows, 3, MUTED, footer, cols - 5);
    at(input_top + 2, 5 + text_width(u->input + start, u->cursor - start));
    printf("\033[?25h"); fflush(stdout);
}

static void insert(ChatUI *u, const char *bytes, size_t n)
{
    if (u->length + n > CHAT_LIMIT) {
        u->overflow = 1;
        ui_notice(u, "输入超过 1024 字节；请删减或 Ctrl+U 清空后重试"); return;
    }
    memmove(u->input + u->cursor + n, u->input + u->cursor, u->length - u->cursor + 1);
    memcpy(u->input + u->cursor, bytes, n);
    u->cursor += n; u->length += n; u->dirty = 1;
}

static void backspace(ChatUI *u)
{
    size_t p = text_prev(u->input, u->cursor);
    memmove(u->input + p, u->input + u->cursor, u->length - u->cursor + 1);
    u->length -= u->cursor - p; u->cursor = p; u->overflow = 0;
}

static int escape_key(ChatUI *u)
{
    const char *s = u->escape;
    if (!strcmp(s, "[D")) u->cursor = text_prev(u->input, u->cursor);
    else if (!strcmp(s, "[C")) u->cursor = text_next(u->input, u->cursor);
    else if (!strcmp(s, "[H") || !strcmp(s, "[1~")) u->cursor = 0;
    else if (!strcmp(s, "[F") || !strcmp(s, "[4~")) u->cursor = u->length;
    else if (!strcmp(s, "[3~") && u->cursor < u->length) {
        size_t end = text_next(u->input, u->cursor);
        memmove(u->input + u->cursor, u->input + end, u->length - end + 1);
        u->length -= end - u->cursor; u->overflow = 0;
    }
    else if (!strcmp(s, "[5~")) u->scroll += u->rows > 12 ? u->rows - 12 : 6;
    else if (!strcmp(s, "[6~")) u->scroll -= u->rows > 12 ? u->rows - 12 : 6;
    else if (!strcmp(s, "[200~")) u->paste = 1;
    else if (!strcmp(s, "[201~")) u->paste = 0;
    else if (!strcmp(s, "OP") || !strcmp(s, "[11~")) return UI_HELP;
    if (u->scroll < 0) u->scroll = 0;
    return UI_NONE;
}

int ui_key(ChatUI *u, unsigned char key)
{
    u->dirty = 1;
    if (key == 3 || key == 17) return UI_QUIT;
    /* A standalone Escape must not swallow the next typed letters. */
    if (u->escape_n == 1 && key != '[' && key != 'O') u->escape_n = 0;
    if (u->escape_n) {
        if (u->escape_n >= sizeof(u->escape) - 1) { u->escape_n = 0; return UI_NONE; }
        u->escape[u->escape_n - 1] = (char)key;
        u->escape[u->escape_n++] = 0;
        if (u->escape_n > 2 && key >= 0x40 && key <= 0x7e) {
            int result = escape_key(u); u->escape_n = 0; return result;
        }
        return UI_NONE;
    }
    if (key == 27) { u->escape_n = 1; u->utf8_n = 0; return UI_NONE; }
    if (u->utf8_n) {
        if ((key & 0xc0) != 0x80) { u->utf8_n = 0; ui_notice(u, "忽略无效 UTF-8 输入"); }
        else {
            u->utf8[u->utf8_n++] = (char)key;
            if (u->utf8_n == u->utf8_need) {
                u->utf8[u->utf8_n] = 0;
                if (text_valid(u->utf8)) insert(u, u->utf8, u->utf8_n);
                else ui_notice(u, "忽略不可显示字符");
                u->utf8_n = 0;
            }
            return UI_NONE;
        }
    }
    if (key >= 0xc2 && key <= 0xf4) {
        u->utf8_need = key < 0xe0 ? 2 : key < 0xf0 ? 3 : 4;
        u->utf8[0] = (char)key; u->utf8_n = 1; return UI_NONE;
    }
    if (u->paste) {
        if (key == '\r' || key == '\n' || key == '\t') { insert(u, " ", 1); return UI_NONE; }
        if (key >= 32 && key < 127) { char c = (char)key; insert(u, &c, 1); }
        return UI_NONE;
    }
    switch (key) {
    case '\r': case '\n':
        if (u->overflow) ui_notice(u, "超长内容未发送；请先删减或 Ctrl+U 清空");
        else return UI_SUBMIT;
        break;
    case 4: if (!u->length) return UI_QUIT; break;
    case 1: u->cursor = 0; break;
    case 5: u->cursor = u->length; break;
    case 7: u->scroll = 0; u->unread = 0; break;
    case 21: ui_reset_input(u); break;
    case 12: u->dirty = 1; break;
    case 8: case 127: backspace(u); break;
    default:
        if (key >= 32 && key < 127) { char c = (char)key; insert(u, &c, 1); }
        break;
    }
    return UI_NONE;
}

void ui_reset_input(ChatUI *u)
{
    u->input[0] = 0; u->cursor = u->length = 0;
    u->utf8_n = 0; u->overflow = 0; u->dirty = 1;
}

void ui_clear(ChatUI *u)
{
    u->count = u->first = u->scroll = 0; u->unread = 0; u->dirty = 1;
}

int ui_save(ChatUI *u, char *path, size_t size)
{
    struct tm tm; time_t now = time(NULL);
    if (!localtime_r(&now, &tm)) { errno = EINVAL; return -1; }
    char stamp[48]; strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    int fd = -1;
    for (int attempt = 0; attempt < 1000; attempt++) {
        int n = snprintf(path, size, "catchat-history-%s-%ld-%03lu.txt", stamp, (long)getpid(), ++u->export_serial);
        if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    fputs("CatChat 本地聊天记录（当前内存中的最近 300 条）\n", f);
    for (int j = 0; j < u->count; j++) {
        ChatEntry *e = &u->entries[(u->first + j) % UI_HISTORY];
        fprintf(f, "[%s] %s%s: %s\n", e->time, e->name, e->kind == 1 ? " (我)" : "", e->text);
    }
    int bad = ferror(f);
    if (fclose(f)) bad = 1;
    if (bad) { errno = EIO; return -1; }
    return 0;
}
