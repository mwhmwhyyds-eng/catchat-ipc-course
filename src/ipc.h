#ifndef CATCHAT_IPC_H
#define CATCHAT_IPC_H
#include <stdint.h>
#include <sys/types.h>
#define CHAT_LIMIT 1024
#define CHAT_NAME 32
#define CHAT_ROOM 32
#define CHAT_MAGIC UINT32_C(0x43415431)
#define CHAT_VERSION "1.1.1"
typedef struct {
    long mtype;
    uint32_t magic;
    uint32_t length;
    uint64_t session;
    uint64_t source;
    uint64_t target;
    uint64_t sequence;
    int64_t timestamp;
    char name[CHAT_NAME];
    char text[CHAT_LIMIT + 1];
} ChatMessage;
typedef struct {
    int online;
    pid_t pid;
    uint64_t instance;
    char name[CHAT_NAME];
} ChatPeer;
typedef struct {
    int control, role_fd, peer_fd, role;
    int queues[2];
    int64_t created[2];
    uint64_t session, instance;
    char room[CHAT_ROOM + 1], name[CHAT_NAME], path[256];
    unsigned long sent, received, discarded;
} ChatIPC;
int ipc_room_valid(const char *room);
int ipc_open(ChatIPC *c, const char *room, int role, const char *name);
int ipc_peer(ChatIPC *c, ChatPeer *peer, int wait_ms);
int ipc_send(ChatIPC *c, const ChatPeer *peer, const char *text);
int ipc_receive(ChatIPC *c, ChatMessage *m);
int ipc_stats(ChatIPC *c, unsigned long pending[2], unsigned long capacity[2]);
void ipc_close(ChatIPC *c);
int ipc_inspect(const char *room, int cleanup);
#endif
