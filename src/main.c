#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include "text.h"
#include "ui.h"
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping, resized;
static ChatUI ui;
static void signal_handler(int sig)
{
    if (sig == SIGWINCH || sig == SIGCONT) resized = 1;
    else stopping = sig;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void help(void)
{
    puts("CatChat " CHAT_VERSION " — 鲁班猫 System V 消息队列双向聊天\n"
         "用法: catchat --role A|B [--room demo] [--name 昵称] [--plain]\n"
         "      catchat --room demo --inspect\n"
         "      catchat --room demo --cleanup\n\n"
         "同一台 Linux 主机（同一 IPC 命名空间）上分别启动 A、B。\n"
         "默认彩色全屏；--plain 为行式诊断模式。\n"
         "命令: /help /status /clear /save /quit；//text 发送以 / 开头的文字。\n"
         "快捷键: Enter 发送，Ctrl+Q/Ctrl+C 退出，Ctrl+U 清空，\n"
         "        左右键/Home/End 编辑，PgUp/PgDn 翻阅，Ctrl+G 回到底部，F1 帮助。\n"
         "房间名: 1–32 个 ASCII 字母、数字、下划线或连字符。\n"
         "消息上限 1024 UTF-8 字节；昵称最多 31 字节、16 列。\n"
         "默认昵称为用户甲/用户乙，可用 --name 自定义。\n"
         "--cleanup 仅清理该房间且拒绝清理有在线端点的会话。\n"
         "--version 显示版本。完整说明见 README.md。");
}

static void add_join_command(const ChatIPC *c)
{
    char command[160];
    snprintf(command, sizeof(command), "./build/catchat --role %c --room %s", 'A' + 1 - c->role, c->room);
    ui_add(&ui, 0, "另一端命令", command, time(NULL));
}

static void add_help(const ChatIPC *c)
{
    ui_add(&ui, 0, "帮助", "/status 查看队列与收发计数；/clear 清空本端显示；/save 导出当前聊天记录；/quit 退出。", time(NULL));
    ui_add(&ui, 0, "快捷键", "左右键、Home/End 编辑；Ctrl+U 清空输入；PgUp/PgDn 翻阅；Ctrl+G 回到底部；Ctrl+Q 退出。以 // 开头可发送 / 开头的消息。", time(NULL));
    add_join_command(c);
}

static void error_notice(const char *action, int error)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s：%s", action, strerror(error)); ui_notice(&ui, buf);
}

static int submit(ChatIPC *c)
{
    char *s = ui.input;
    if (!*s) return 0;
    if (!text_has_content(s)) { ui_notice(&ui, "消息不能只有空白字符；请继续编辑"); return 0; }
    if (!strcmp(s, "/quit")) return 1;
    if (!strcmp(s, "/help")) add_help(c);
    else if (!strcmp(s, "/clear")) ui_clear(&ui);
    else if (!strcmp(s, "/save")) {
        char path[160], note[256];
        if (ui_save(&ui, path, sizeof(path))) error_notice("导出失败", errno);
        else {
            snprintf(note, sizeof(note), "已保存 %s（当前工作目录）", path); ui_success(&ui, note);
        }
    } else if (!strcmp(s, "/status")) {
        unsigned long pending[2], capacity[2]; char buf[256];
        if (ipc_stats(c, pending, capacity)) error_notice("读取队列失败", errno);
        else {
            snprintf(buf, sizeof(buf), "RX #%d 待收 %lu；TX #%d 待发 %lu；已入队 %lu，已收到 %lu，丢弃旧/无效包 %lu。容量 %lu/%lu bytes。",
                     c->queues[c->role], pending[c->role], c->queues[1 - c->role],
                     pending[1 - c->role], c->sent, c->received, c->discarded,
                     capacity[c->role], capacity[1 - c->role]);
            ui_add(&ui, 0, "IPC 状态", buf, time(NULL));
        }
    } else if (*s == '/' && s[1] != '/') {
        ui_notice(&ui, "未知命令；输入 /help，或用 // 发送以 / 开头的文字"); return 0;
    } else {
        ChatPeer peer;
        if (ipc_peer(c, &peer, 50)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                ui_notice(&ui, "会话状态正忙，内容已保留；稍后按 Enter 重试");
            else error_notice("读取对端状态失败", errno);
            return 0;
        }
        const char *message = *s == '/' ? s + 1 : s;
        if (ipc_send(c, &peer, message)) {
            if (errno == ENOTCONN) ui_notice(&ui, "对方尚未在线，内容已保留；对方加入后按 Enter 重试");
            else if (errno == EAGAIN) ui_notice(&ui, "发送队列已满，内容已保留；请等对方接收后重试");
            else error_notice("发送失败，内容已保留", errno);
            return 0;
        }
        ui.scroll = 0;
        ui.unread = 0;
        ui_add(&ui, 1, c->name, message, time(NULL));
        if (ui.plain) printf("SENT %lu\n", c->sent);
        ui_success(&ui, "消息已入队  |  /status 查看通信状态");
    }
    ui_reset_input(&ui);
    return 0;
}

int main(int argc, char **argv)
{
    const char *room = "demo", *name = NULL;
    int role = -1, plain = 0, inspect = 0, cleanup = 0;
    static const struct option options[] = {
        {"role", required_argument, NULL, 'r'}, {"room", required_argument, NULL, 's'},
        {"name", required_argument, NULL, 'n'}, {"plain", no_argument, NULL, 'p'},
        {"inspect", no_argument, NULL, 'i'}, {"cleanup", no_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0}
    };
    if (text_init()) { fputs("无法启用 UTF-8 locale\n", stderr); return 1; }
    int opt;
    while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (opt) {
        case 'r':
            if (strlen(optarg) != 1 || !strchr("AaBb", optarg[0])) role = -2;
            else role = (optarg[0] == 'A' || optarg[0] == 'a') ? 0 : 1;
            break;
        case 's': room = optarg; break;
        case 'n': name = optarg; break;
        case 'p': plain = 1; break;
        case 'i': inspect = 1; break;
        case 'c': cleanup = 1; break;
        case 'h': help(); return 0;
        case 'v': puts("CatChat " CHAT_VERSION); return 0;
        default: return 2;
        }
    }
    if (!ipc_room_valid(room) || optind != argc || (inspect && cleanup) ||
        ((inspect || cleanup) && role != -1)) {
        fputs("参数不合法；使用 --help 查看用法\n", stderr); return 2;
    }
    if (inspect || cleanup) {
        if (ipc_inspect(room, cleanup)) { perror("CatChat inspect/cleanup"); return 1; }
        return 0;
    }
    if (role < 0) { fputs("请指定 --role A 或 --role B\n", stderr); return 2; }
    if (!name) name = role == 0 ? "用户甲" : "用户乙";
    if (!text_has_content(name) || strlen(name) >= CHAT_NAME || !text_valid(name) || text_width(name, strlen(name)) > 16) {
        fputs("昵称须为可显示 UTF-8 文字，最多 31 字节、16 列\n", stderr); return 2;
    }
    if (!plain && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fputs("全屏模式需要交互终端；诊断时可使用 --plain\n", stderr); return 2;
    }
    setvbuf(stdout, NULL, _IOFBF, 65536);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = signal_handler; sigemptyset(&sa.sa_mask);
    int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH, SIGCONT, SIGPIPE};
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++) sigaction(signals[i], &sa, NULL);
    /* Do not suspend with the terminal left in raw mode. Ctrl+Q exits cleanly. */
    signal(SIGTSTP, SIG_IGN);
    ChatIPC c;
    if (ipc_open(&c, room, role, name)) {
        if (errno == EADDRINUSE) fputs("该房间的同一角色已运行，请使用另一角色或房间\n", stderr);
        else if (errno == EAGAIN) fputs("会话状态正忙，请稍后重试\n", stderr);
        else if (errno == EPROTO) fputs("本房间元数据损坏，已保留原文件和队列，请先检查本房间状态\n", stderr);
        else if (errno == ESTALE) fputs("队列身份与本房间记录不一致，已保留原记录并停止自动清理\n", stderr);
        else perror("CatChat ipc_open");
        return 1;
    }
    if (ui_start(&ui, plain)) { perror("CatChat terminal"); ipc_close(&c); return 1; }
    if (plain) printf("READY role=%c room=%s rx=%d tx=%d\n", 'A' + role, room, c.queues[role], c.queues[1-role]);
    ui_add(&ui, 0, "CatChat", "欢迎。两个终端运行在同一台鲁班猫上，消息通过内核 System V 队列传递。", time(NULL));
    add_join_command(&c);
    ChatPeer peer = {0};
    int64_t next_peer = 0, next_stats = 0, next_frame = 0, escape_started = 0;
    int fatal = 0, quit = 0;
    while (!stopping && !quit) {
        int64_t now = monotonic_ms();
        if (now >= next_peer) {
            ChatPeer current;
            int peer_result = ipc_peer(&c, &current, 0);
            if (peer_result && errno != EAGAIN && errno != EWOULDBLOCK) { fatal = errno; break; }
            if (!peer_result && (current.online != peer.online || current.instance != peer.instance)) {
                char buf[160];
                if (current.online) snprintf(buf, sizeof(buf), "%s 已加入，可以开始聊天。", current.name);
                else snprintf(buf, sizeof(buf), "%s 已离线，等待重新加入。", peer.name);
                ui_add(&ui, 0, "会话", buf, time(NULL));
                if (plain) printf("PEER %s\n", current.online ? "ONLINE" : "OFFLINE");
                peer = current;
            }
            next_peer = now + 200;
        }
        /* System V queue IDs are not pollable file descriptors. Drain a bounded
         * batch, then poll stdin for <=50 ms; this also prevents a CPU spin. */
        for (int i = 0; i < 32; i++) {
            ChatMessage m;
            int result = ipc_receive(&c, &m);
            if (result < 0) { fatal = errno; break; }
            if (!result) break;
            if (result == 1) ui_add(&ui, 2, m.name, m.text, m.timestamp);
        }
        if (fatal) break;
        if (now >= next_stats) { ui.dirty = 1; next_stats = now + 1000; }
        if (resized) { ui.dirty = 1; resized = 0; }
        if (ui.escape_n && escape_started && now - escape_started > 200) {
            ui.escape_n = 0; escape_started = 0;
        }
        if (now >= next_frame) { ui_render(&ui, &c, &peer); next_frame = now + 50; }
        fflush(stdout);
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&p, 1, 50);
        if (ready < 0 && errno != EINTR) { fatal = errno; break; }
        if (ready > 0 && (p.revents & POLLIN)) {
            unsigned char bytes[256]; ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (n < 0 && errno != EINTR && errno != EAGAIN) { fatal = errno; break; }
            if (n == 0) break;
            for (ssize_t i = 0; i < n && !quit; i++) {
                if (bytes[i] == 27) escape_started = now;
                int result = ui_key(&ui, bytes[i]);
                if (result == UI_QUIT) quit = 1;
                else if (result == UI_HELP) add_help(&c);
                else if (result == UI_SUBMIT) quit = submit(&c);
            }
        } else if (ready > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
    }
    ui_stop(&ui); ipc_close(&c);
    if (fatal) { fprintf(stderr, "通信已停止：%s；本端已退出并恢复终端。\n", strerror(fatal)); return 1; }
    printf("CatChat 已退出。已入队 %lu 条，已收到 %lu 条。\n", c.sent, c.received);
    return 0;
}
