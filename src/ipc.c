#define _GNU_SOURCE
#include "ipc.h"
#include "text.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/msg.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* All room metadata operations hold control.lock. Role locks live for the
 * process lifetime; lock files are deliberately never unlinked (inode races). */
static uint64_t nonce(void)
{
    uint64_t v;
    if (getrandom(&v, sizeof(v), 0) == sizeof(v) && v) return v;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ (uint64_t)getpid();
}

int ipc_room_valid(const char *room)
{
    size_t n = strlen(room);
    if (!n || n > CHAT_ROOM) return 0;
    for (size_t i = 0; i < n; i++) {
        char b = room[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
              (b >= '0' && b <= '9') || b == '_' || b == '-')) return 0;
    }
    return 1;
}

static int private_dir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    if (lstat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        errno = EACCES; return -1;
    }
    return 0;
}

static int private_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) || st.st_nlink != 1) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

static int prepare(ChatIPC *c, const char *room)
{
    memset(c, 0, sizeof(*c));
    c->control = c->role_fd = c->peer_fd = -1;
    c->queues[0] = c->queues[1] = -1;
    if (!ipc_room_valid(room)) { errno = EINVAL; return -1; }
    snprintf(c->path, sizeof(c->path), "/tmp/catchat-ipc-course-%lu", (unsigned long)getuid());
    if (private_dir(c->path)) return -1;
    size_t used = strlen(c->path);
    snprintf(c->path + used, sizeof(c->path) - used, "/%s", room);
    if (private_dir(c->path)) return -1;
    char p[320];
    snprintf(p, sizeof(p), "%s/control.lock", c->path);
    c->control = private_file(p);
    if (c->control < 0) return -1;
    snprintf(c->room, sizeof(c->room), "%s", room);
    return 0;
}

static int role_file(ChatIPC *c, int role)
{
    char p[320];
    snprintf(p, sizeof(p), "%s/%c.lock", c->path, 'A' + role);
    return private_file(p);
}

static int held(int fd)
{
    if (!flock(fd, LOCK_EX | LOCK_NB)) { flock(fd, LOCK_UN); return 0; }
    if (errno == EWOULDBLOCK || errno == EAGAIN) return 1;
    return -1;
}

/* A stopped endpoint may hold control.lock. Never freeze the interactive
 * event loop waiting for it; setup/teardown get a bounded grace period. */
static int room_lock(int fd, int wait_ms)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (;;) {
        if (!flock(fd, LOCK_EX | LOCK_NB)) return 0;
        if (errno != EWOULDBLOCK && errno != EAGAIN) return -1;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= wait_ms) { errno = EAGAIN; return -1; }
        const struct timespec delay = {0, 10000000};
        if (nanosleep(&delay, NULL)) return -1;
    }
}

static int read_state(ChatIPC *c)
{
    char buf[256] = {0};
    unsigned long long cookie;
    long long t0, t1;
    ssize_t n = pread(c->control, buf, sizeof(buf) - 1, 0);
    if (n < 0) return -1;
    if (!n) return 0;
    int used = 0;
    if (n >= (ssize_t)sizeof(buf) - 1 ||
        sscanf(buf, "CATCHAT1 %d %d %lld %lld %llu %n", &c->queues[0],
               &c->queues[1], &t0, &t1, &cookie, &used) != 5 || buf[used] ||
        c->queues[0] < 0 || c->queues[1] < 0 || c->queues[0] == c->queues[1] ||
        t0 < 0 || t1 < 0 || !cookie) { errno = EPROTO; return -1; }
    c->created[0] = t0; c->created[1] = t1; c->session = cookie;
    return 1;
}

static int queue_matches(ChatIPC *c, int i, const struct msqid_ds *ds)
{
    return ds->msg_perm.uid == getuid() && ds->msg_perm.cuid == getuid() &&
           (ds->msg_perm.mode & 0777) == 0600 && ds->msg_perm.__key == IPC_PRIVATE &&
           (int64_t)ds->msg_ctime == c->created[i];
}

static int owned_queue(ChatIPC *c, int i)
{
    struct msqid_ds ds;
    if (c->queues[i] < 0 || msgctl(c->queues[i], IPC_STAT, &ds)) return 0;
    return queue_matches(c, i, &ds);
}

static int remove_queues(ChatIPC *c)
{
    int present[2] = {0};
    /* Check both identities before deleting either. Preserve metadata on any
     * error so a retry/inspection still has the original queue references. */
    for (int i = 0; i < 2; i++) {
        struct msqid_ds ds;
        if (c->queues[i] < 0) continue;
        if (msgctl(c->queues[i], IPC_STAT, &ds)) {
            if (errno == EINVAL || errno == EIDRM) continue;
            return -1;
        }
        if (!queue_matches(c, i, &ds)) { errno = ESTALE; return -1; }
        present[i] = 1;
    }
    for (int i = 0; i < 2; i++) {
        if (present[i] && msgctl(c->queues[i], IPC_RMID, NULL) &&
            errno != EINVAL && errno != EIDRM) return -1;
    }
    return ftruncate(c->control, 0);
}

static int create_queues(ChatIPC *c)
{
    c->session = nonce();
    for (int i = 0; i < 2; i++) {
        struct msqid_ds ds;
        c->queues[i] = msgget(IPC_PRIVATE, 0600);
        if (c->queues[i] < 0) {
            if (i) (void)msgctl(c->queues[0], IPC_RMID, NULL);
            return -1;
        }
        if (msgctl(c->queues[i], IPC_STAT, &ds)) {
            (void)msgctl(c->queues[i], IPC_RMID, NULL);
            if (i) (void)msgctl(c->queues[0], IPC_RMID, NULL);
            return -1;
        }
        c->created[i] = (int64_t)ds.msg_ctime;
    }
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "CATCHAT1 %d %d %lld %lld %llu\n",
                     c->queues[0], c->queues[1], (long long)c->created[0],
                     (long long)c->created[1], (unsigned long long)c->session);
    if (ftruncate(c->control, 0) || pwrite(c->control, buf, (size_t)n, 0) != n) {
        int e = errno; remove_queues(c); errno = e ? e : EIO; return -1;
    }
    return 0;
}

int ipc_open(ChatIPC *c, const char *room, int role, const char *name)
{
    if (prepare(c, room)) return -1;
    c->role = role;
    c->role_fd = role_file(c, role);
    c->peer_fd = role_file(c, 1 - role);
    if (c->role_fd < 0 || c->peer_fd < 0) goto fail;
    if (room_lock(c->control, 1000)) goto fail;
    if (flock(c->role_fd, LOCK_EX | LOCK_NB)) {
        errno = EADDRINUSE; goto locked_fail;
    }
    int online = held(c->peer_fd);
    int state = read_state(c);
    if (online < 0 || state < 0) goto locked_fail;
    if (!online) {
        /* No live endpoints: recover only this room's fingerprinted queues. */
        if (state > 0 && remove_queues(c)) goto locked_fail;
        if (create_queues(c)) goto locked_fail;
    } else if (state != 1 || !owned_queue(c, 0) || !owned_queue(c, 1)) {
        errno = EIDRM; goto locked_fail;
    }
    c->instance = nonce();
    snprintf(c->name, sizeof(c->name), "%s", name);
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%ld %llu\n%s\n", (long)getpid(),
                     (unsigned long long)c->instance, name);
    if (ftruncate(c->role_fd, 0) || pwrite(c->role_fd, buf, (size_t)n, 0) != n) {
        if (!online) remove_queues(c);
        goto locked_fail;
    }
    flock(c->control, LOCK_UN);
    return 0;
locked_fail:
    { int e = errno; flock(c->control, LOCK_UN); errno = e; }
fail:
    { int e = errno;
      if (c->role_fd >= 0) close(c->role_fd);
      if (c->peer_fd >= 0) close(c->peer_fd);
      if (c->control >= 0) close(c->control);
      c->role_fd = c->peer_fd = c->control = -1;
      errno = e; return -1; }
}

int ipc_peer(ChatIPC *c, ChatPeer *peer, int wait_ms)
{
    memset(peer, 0, sizeof(*peer));
    if (room_lock(c->control, wait_ms)) return -1;
    int online = held(c->peer_fd);
    if (online == 1) {
        char buf[160] = {0}; long pid; unsigned long long instance;
        if (pread(c->peer_fd, buf, sizeof(buf) - 1, 0) > 0 &&
            sscanf(buf, "%ld %llu", &pid, &instance) == 2) {
            char *name = strchr(buf, '\n');
            if (name) {
                name++;
                name[strcspn(name, "\n")] = 0;
                if (strlen(name) < CHAT_NAME && text_valid(name)) {
                    peer->online = 1; peer->pid = (pid_t)pid;
                    peer->instance = instance;
                    snprintf(peer->name, sizeof(peer->name), "%s", name);
                }
            }
        }
    }
    flock(c->control, LOCK_UN);
    return online < 0 ? -1 : 0;
}

int ipc_send(ChatIPC *c, const ChatPeer *peer, const char *text)
{
    if (!peer->online) { errno = ENOTCONN; return -1; }
    ChatMessage m = {0};
    m.mtype = 1; m.magic = CHAT_MAGIC;
    m.session = c->session; m.source = c->instance; m.target = peer->instance;
    m.sequence = c->sent + 1; m.timestamp = (int64_t)time(NULL);
    m.length = (uint32_t)strlen(text);
    if (m.length > CHAT_LIMIT || !text_valid(text)) { errno = EINVAL; return -1; }
    snprintf(m.name, sizeof(m.name), "%s", c->name);
    memcpy(m.text, text, m.length + 1);
    size_t body = offsetof(ChatMessage, text) - sizeof(long) + m.length + 1;
    if (msgsnd(c->queues[1 - c->role], &m, body, IPC_NOWAIT)) return -1;
    c->sent++;
    return 0;
}

int ipc_receive(ChatIPC *c, ChatMessage *m)
{
    memset(m, 0, sizeof(*m));
    ssize_t n = msgrcv(c->queues[c->role], m, sizeof(*m) - sizeof(long),
                       0, IPC_NOWAIT | MSG_NOERROR);
    if (n < 0) return errno == ENOMSG || errno == EINTR ? 0 : -1;
    size_t header = offsetof(ChatMessage, text) - sizeof(long);
    if ((size_t)n < header + 1 || m->magic != CHAT_MAGIC || m->session != c->session ||
        m->target != c->instance || m->length > CHAT_LIMIT ||
        (size_t)n != header + m->length + 1 || m->text[m->length] != 0 ||
        strlen(m->text) != m->length || !memchr(m->name, 0, CHAT_NAME) ||
        !text_valid(m->text) || !text_valid(m->name)) {
        c->discarded++; return 2;
    }
    c->received++;
    return 1;
}

int ipc_stats(ChatIPC *c, unsigned long pending[2], unsigned long capacity[2])
{
    for (int i = 0; i < 2; i++) {
        struct msqid_ds ds;
        if (msgctl(c->queues[i], IPC_STAT, &ds)) return -1;
        pending[i] = ds.msg_qnum; capacity[i] = ds.msg_qbytes;
    }
    return 0;
}

void ipc_close(ChatIPC *c)
{
    if (c->control < 0) return;
    if (!room_lock(c->control, 1000)) {
        if (held(c->peer_fd) == 0 && remove_queues(c))
            fputs("CatChat: 本房间资源清理失败，可用 --cleanup 重试\n", stderr);
        close(c->role_fd); c->role_fd = -1;
        flock(c->control, LOCK_UN);
    } else {
        fputs("CatChat: 会话锁正忙，本端已释放；队列留待其他端点退出或下次启动时回收\n", stderr);
    }
    if (c->role_fd >= 0) close(c->role_fd);
    close(c->peer_fd); close(c->control); c->control = -1;
}

int ipc_inspect(const char *room, int cleanup)
{
    ChatIPC c;
    if (prepare(&c, room)) return -1;
    int rc = -1;
    if (room_lock(c.control, 1000)) goto done;
    int a = role_file(&c, 0), b = role_file(&c, 1);
    if (a < 0 || b < 0) {
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        goto unlock;
    }
    int ha = held(a), hb = held(b), state = read_state(&c);
    printf("room=%s A=%s B=%s\n", room, ha == 1 ? "online" : "offline",
           hb == 1 ? "online" : "offline");
    if (ha < 0 || hb < 0) goto files_done;
    if (cleanup && (ha || hb)) { errno = EBUSY; goto files_done; }
    if (state > 0) {
        for (int i = 0; i < 2; i++) {
            struct msqid_ds ds;
            if (owned_queue(&c, i) && !msgctl(c.queues[i], IPC_STAT, &ds))
                printf("queue_%c=%d messages=%lu bytes=%lu capacity=%lu mode=0600\n",
                       'A' + i, c.queues[i], (unsigned long)ds.msg_qnum,
                       (unsigned long)ds.msg_cbytes, (unsigned long)ds.msg_qbytes);
            else printf("queue_%c=unavailable\n", 'A' + i);
        }
        if (cleanup && remove_queues(&c)) goto files_done;
    } else if (state == 0) puts("queues=none");
    else { errno = EPROTO; goto files_done; }
    if (cleanup) puts("cleanup=ok (this room only; lock files retained)");
    rc = 0;
files_done:
    close(a); close(b);
unlock:
    { int e = errno; flock(c.control, LOCK_UN); errno = e; }
done:
    { int e = errno; close(c.control); errno = e; }
    return rc;
}
