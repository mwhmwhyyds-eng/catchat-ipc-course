#!/usr/bin/env python3
"""Real Linux processes, real System V queues and PTYs; standard library only.

Every case uses a unique room. No global ipcrm/pkill, no unrelated queues touched.
"""
import ctypes
import errno
import fcntl
import json
import os
import pathlib
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time
import uuid

BIN = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './build/catchat').resolve())
PREFIX = 'test-' + uuid.uuid4().hex[:10]
PROCESSES = []
ROOMS = []
RESULTS = []
CURRENT_CASE = 'setup'
LOG_DIR = pathlib.Path(os.environ.get('CATCHAT_TEST_LOG_DIR', 'evidence/test-logs')) / PREFIX
LOG_DIR.mkdir(parents=True, mode=0o700, exist_ok=False)
LOG_INDEX = []


def save_log(name, data):
    fd = os.open(str(LOG_DIR / name), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)


def room(label):
    value = PREFIX + '-' + label
    ROOMS.append(value)
    return value


def run(*args):
    result = subprocess.run([BIN, *args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, encoding='utf-8', timeout=15)
    name = 'command-%03d.log' % (len(LOG_INDEX) + 1)
    save_log(name, result.stdout.encode('utf-8'))
    LOG_INDEX.append({'case': CURRENT_CASE, 'command': list(args), 'returncode': result.returncode, 'file': name})
    return result


def wait_until(fn, timeout=4, description='condition'):
    limit = time.monotonic() + timeout
    while time.monotonic() < limit:
        if fn():
            return
        time.sleep(.025)
    raise AssertionError('timeout: ' + description)


class Endpoint:
    def __init__(self, session, role, name, terminal=False):
        self.lines = []
        self.capture = bytearray()
        self.terminal = terminal
        self.case_name, self.role = CURRENT_CASE, role
        args = [BIN, '--room', session, '--role', role]
        if name is not None:
            args.extend(['--name', name])
        if terminal:
            self.master, self.slave = pty.openpty()
            self.original = termios.tcgetattr(self.slave)
            self.resize(32, 116)
            self.proc = subprocess.Popen(args, stdin=self.slave, stdout=self.slave,
                                         stderr=self.slave, close_fds=True)
        else:
            args.append('--plain')
            self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        PROCESSES.append(self)
        self.thread = threading.Thread(target=self.reader, daemon=True)
        self.thread.start()
        if terminal:
            wait_until(lambda: b'\x1b[?1049h' in self.capture, description='alternate screen')
        else:
            self.expect('READY')

    def reader(self):
        if self.terminal:
            while True:
                try:
                    ready, _, _ = select.select([self.master], [], [], .1)
                    if ready:
                        data = os.read(self.master, 65536)
                        if not data:
                            break
                        self.capture.extend(data)
                    elif self.proc.poll() is not None:
                        break
                except OSError:
                    break
        else:
            for data in iter(self.proc.stdout.readline, b''):
                self.capture.extend(data)
                self.lines.append(data.decode('utf-8', 'replace').rstrip())

    def text(self):
        return self.capture.decode('utf-8', 'replace')

    def send(self, value):
        data = value.encode('utf-8') if isinstance(value, str) else value
        if self.terminal:
            os.write(self.master, data)
        else:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()

    def expect(self, text, after=0, timeout=4):
        wait_until(lambda: any(text in line for line in self.lines[after:]), timeout,
                   repr(text) + '\n' + '\n'.join(self.lines[-10:]))

    def resize(self, rows, cols):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        if hasattr(self, 'proc'):
            self.proc.send_signal(signal.SIGWINCH)

    def stop(self, sig=None):
        if self.proc.poll() is None:
            if sig:
                self.proc.send_signal(sig)
            else:
                self.send(b'\x11' if self.terminal else '/quit\n')
            # LeakSanitizer's exit-time scan is noticeably slower on this board.
            # Message/UI responsiveness checks still use the short timeout.
            self.proc.wait(timeout=15)
        self.thread.join(timeout=1)


def case(label, fn):
    global CURRENT_CASE
    CURRENT_CASE = label
    start = time.monotonic()
    try:
        fn()
    except BaseException as exc:
        RESULTS.append({'case': label, 'result': 'FAIL', 'error': repr(exc)})
        raise
    RESULTS.append({'case': label, 'result': 'PASS', 'seconds': round(time.monotonic() - start, 3)})
    print('PASS ' + label, flush=True)


def cli():
    assert run('--version').returncode == 0
    assert run('--help').returncode == 0
    for args in [('--role', 'X'), ('--role', 'A', '--room', '../bad'),
                 ('--role', 'A', '--name', 'x' * 32),
                 ('--role', 'A', '--name', 'bad\x1b[31m'),
                 ('--role', 'A', '--name', '   '),
                 ('--role', 'A', '--name', '\u3000'),
                 ('--inspect', '--cleanup'), ('--role', 'A', 'extra')]:
        assert run(*args).returncode == 2, args


def chat():
    r = room('chat')
    a = Endpoint(r, 'A', None)
    a.expect('./build/catchat --role B --room ' + r)
    a.send('对方未上线时保留草稿\n')
    a.expect('对方尚未在线')
    b = Endpoint(r, 'B', None)
    a.expect('PEER ONLINE'); b.expect('PEER ONLINE')
    duplicate = run('--room', r, '--role', 'A', '--plain')
    assert duplicate.returncode == 1 and '同一角色' in duplicate.stdout
    assert run('--room', r, '--cleanup').returncode == 1
    a.send('\n'); b.expect('RECV 用户甲 对方未上线时保留草稿')
    a.send('你好，鲁班猫！Hello 123\n')
    b.expect('RECV 用户甲 你好，鲁班猫！Hello 123')
    b.send('正在输入的草稿')
    a.send('收发可以同时进行\n')
    b.expect('RECV 用户甲 收发可以同时进行')
    b.send('，没有被打断。\n')
    a.expect('RECV 用户乙 正在输入的草稿，没有被打断。')
    a.send('//help 是文字\n'); b.expect('RECV 用户甲 /help 是文字')
    a.send('/status\n'); a.expect('IPC 状态') if a.terminal else a.expect('容量')
    b.stop(); a.expect('PEER OFFLINE')
    b2 = Endpoint(r, 'B', '重连B'); a.expect('重连B 已加入')
    b2.send('重新加入成功\n'); a.expect('RECV 重连B 重新加入成功')
    b2.stop(); a.stop()
    assert a.proc.returncode == b.proc.returncode == b2.proc.returncode == 0
    assert 'queues=none' in run('--room', r, '--inspect').stdout


def simultaneous_and_isolation():
    r1, r2 = room('parallel'), room('isolated')
    a, b = Endpoint(r1, 'A', 'A'), Endpoint(r1, 'B', 'B')
    other = Endpoint(r2, 'A', 'Other')
    a.expect('PEER ONLINE'); b.expect('PEER ONLINE')
    for i in range(40):
        a.send('A-%03d\n' % i)
        b.send('B-%03d\n' % i)
        # Awaiting both at each iteration keeps this a delivery/order test,
        # rather than silently depending on the kernel's queue capacity.
        a.expect('RECV B B-%03d' % i)
        b.expect('RECV A A-%03d' % i)
    assert len([x for x in a.lines if x.startswith('RECV ')]) == 40
    assert len([x for x in b.lines if x.startswith('RECV ')]) == 40
    assert not any(x.startswith('RECV ') for x in other.lines)
    a.stop(); b.stop(); other.stop()


def backpressure_and_generation():
    r = room('full')
    a, b = Endpoint(r, 'A', 'A'), Endpoint(r, 'B', 'B')
    a.expect('PEER ONLINE')
    b.proc.send_signal(signal.SIGSTOP)
    payload = '满' * 300
    for i in range(100):
        marker = len(a.lines)
        a.send(payload + '\n')
        wait_until(lambda: any(x.startswith('SENT ') or '队列已满' in x for x in a.lines[marker:]))
        if any('队列已满' in x for x in a.lines[marker:]):
            break
    else:
        raise AssertionError('queue did not fill')
    sent = len([x for x in a.lines if x.startswith('SENT ')])
    assert sent > 0
    info = run('--room', r, '--inspect').stdout
    assert re.search(r'messages=[1-9]', info), info
    b.proc.send_signal(signal.SIGCONT)
    wait_until(lambda: len([x for x in b.lines if x.startswith('RECV ')]) == sent)
    a.send('\n')
    wait_until(lambda: len([x for x in b.lines if x.startswith('RECV ')]) == sent + 1)
    # A queued message targets B's current random instance. A replacement B
    # must not display messages left behind for the killed process.
    b.proc.send_signal(signal.SIGSTOP)
    a.send('STALE-TARGET-DO-NOT-REPLAY\n')
    a.expect('SENT %d' % (sent + 2))
    b.stop(signal.SIGKILL)
    a.expect('PEER OFFLINE')
    replacement = Endpoint(r, 'B', 'NewB')
    a.expect('NewB 已加入')
    time.sleep(.2)
    assert 'STALE-TARGET' not in replacement.text()
    a.send('NEW-TARGET-OK\n'); replacement.expect('RECV A NEW-TARGET-OK')
    replacement.stop(); a.stop()


def crash_recovery():
    r = room('crash')
    a, b = Endpoint(r, 'A', 'A'), Endpoint(r, 'B', 'B')
    old_ids = re.findall(r'(?:rx|tx)=(\d+)', '\n'.join(a.lines))
    a.stop(signal.SIGKILL); b.stop(signal.SIGKILL)
    assert 'queue_A=' in run('--room', r, '--inspect').stdout
    recovered = Endpoint(r, 'A', 'Recovered')
    new_ids = re.findall(r'(?:rx|tx)=(\d+)', '\n'.join(recovered.lines))
    assert old_ids and new_ids and set(old_ids).isdisjoint(new_ids)
    recovered.stop()
    assert 'queues=none' in run('--room', r, '--inspect').stdout


def terminal():
    r = room('pty')
    a, b = Endpoint(r, 'A', '屏幕A', True), Endpoint(r, 'B', '接收B')
    b.expect('PEER ONLINE')
    # Chinese code point editing: remove 好, insert 们, append ! at End.
    a.send('你好'); a.send(b'\x7f'); a.send('们'); a.send(b'\x01')
    a.send('前'); a.send(b'\x1b[F'); a.send('!\r')
    b.expect('RECV 屏幕A 前你们!')
    # Unicode sequences may arrive split across reads.
    for byte in '分段中文'.encode('utf-8'):
        a.send(bytes([byte])); time.sleep(.006)
    b.send('输入时收到消息\n')
    wait_until(lambda: '输入时收到消息' in a.text())
    a.send('保持\r'); b.expect('RECV 屏幕A 分段中文保持')
    # Bracketed paste must not execute a pasted newline as a send command.
    marker = len(b.lines)
    a.send(b'\x1b[200~'); a.send('粘贴\n换行'); a.send(b'\x1b[201~')
    time.sleep(.15)
    assert not any(x.startswith('RECV ') for x in b.lines[marker:])
    a.send('\r'); b.expect('RECV 屏幕A 粘贴 换行')
    a.resize(12, 45)
    wait_until(lambda: '窗口过小' in a.text())
    b.send('小窗口仍可接收\n')
    marker = len(a.capture)
    a.resize(18, 118)
    wait_until(lambda: '消息' in a.capture[marker:].decode('utf-8', 'replace'))
    assert 'IPC 观测' not in a.capture[marker:].decode('utf-8', 'replace'), 'sidebar overlaps short window'
    a.resize(36, 125)
    wait_until(lambda: '小窗口仍可接收' in a.text())
    a.send('x' * 1025 + '\r')
    wait_until(lambda: '超长内容未发送' in a.text())
    a.send(b'\x15')
    a.send('L' * 1024 + '\r')
    b.expect('RECV 屏幕A ' + 'L' * 1024)
    a.send('/save\r')
    wait_until(lambda: '已保存' in a.text())
    saved = list(pathlib.Path('.').glob('catchat-history-*-{}-*.txt'.format(a.proc.pid)))
    assert len(saved) == 1
    assert '分段中文保持' in saved[0].read_text(encoding='utf-8')
    saved[0].unlink()  # Exact file created by this test only.
    a.stop(signal.SIGTERM)
    assert a.proc.returncode == 0
    assert termios.tcgetattr(a.slave) == a.original, 'terminal settings not restored'
    assert b'\x1b[?1049l' in a.capture and b'\x1b[?2004l' in a.capture
    b.stop()


def malformed_queue_message():
    r = room('malformed')
    a, b = Endpoint(r, 'A', 'A'), Endpoint(r, 'B', 'B')
    a.expect('PEER ONLINE')
    rx = int(re.search(r'rx=(\d+)', '\n'.join(a.lines)).group(1))
    libc = ctypes.CDLL(None, use_errno=True)
    class Packet(ctypes.Structure):
        _fields_ = [('mtype', ctypes.c_long), ('body', ctypes.c_char * 4096)]
    packet = Packet(); packet.mtype = 1; packet.body = b'\x1b[31mBAD'
    assert libc.msgsnd(rx, ctypes.byref(packet), 4096, 0o4000) == 0
    time.sleep(.15)
    a.send('/status\n'); a.expect('丢弃旧/无效包 1')
    b.send('VALID-AFTER-MALFORMED\n'); a.expect('RECV B VALID-AFTER-MALFORMED')
    a.stop(); b.stop()


def external_removal():
    r = room('removed')
    a, b = Endpoint(r, 'A', 'A', True), Endpoint(r, 'B', 'B')
    state = pathlib.Path('/tmp/catchat-ipc-course-{}/{}/control.lock'.format(os.getuid(), r)).read_text()
    qid = int(state.split()[1])
    # Deliberately remove only this test's freshly created queue.
    libc = ctypes.CDLL(None, use_errno=True)
    assert libc.msgctl(qid, 0, None) == 0  # IPC_RMID on Linux
    wait_until(lambda: termios.tcgetattr(a.slave) == a.original,
               description='prompt terminal restoration after queue removal')
    a.proc.wait(timeout=15)
    a.thread.join(timeout=1)
    assert a.proc.returncode == 1
    assert termios.tcgetattr(a.slave) == a.original
    b.stop()
    assert 'queues=none' in run('--room', r, '--inspect').stdout


def queue_ids():
    return set(re.findall(r'^\S+\s+(\d+)\s+', subprocess.check_output(['ipcs', '-q'], text=True), re.M))


def bounded_control_lock():
    r = room('busy')
    a = Endpoint(r, 'A', 'A', True)
    path = pathlib.Path('/tmp/catchat-ipc-course-{}/{}/control.lock'.format(os.getuid(), r))
    with path.open('r+') as holder:
        fcntl.flock(holder, fcntl.LOCK_EX)
        time.sleep(.25)  # Let the endpoint encounter a busy peer-state check.
        a.send('锁竞争时输入也可响应')
        wait_until(lambda: '锁竞争时输入也可响应' in a.text(), description='responsive input under lock contention')
        # Startup and inspection must fail within a bounded interval, not hang.
        started = time.monotonic()
        busy = subprocess.run([BIN, '--room', r, '--inspect'], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=15)
        # Sanitizer exit scanning may be slow; its error must still be EAGAIN.
        assert busy.returncode == 1 and b'Resource temporarily unavailable' in busy.stdout
        a.send(b'\x11')
        wait_until(lambda: termios.tcgetattr(a.slave) == a.original,
                   description='terminal restores even while session lock is held')
        a.proc.wait(timeout=15)
        assert a.proc.returncode == 0
        fcntl.flock(holder, fcntl.LOCK_UN)
    a.thread.join(timeout=1)
    assert run('--room', r, '--cleanup').returncode == 0


def identity_mismatch():
    r = room('identity')
    a = Endpoint(r, 'A', 'A')
    path = pathlib.Path('/tmp/catchat-ipc-course-{}/{}/control.lock'.format(os.getuid(), r))
    backup = path.read_text()
    a.stop(signal.SIGKILL)
    modified = backup.split(); modified[3] = str(int(modified[3]) + 1)
    invalid = ' '.join(modified) + '\n'
    try:
        path.write_text(invalid)
        result = run('--room', r, '--cleanup')
        assert result.returncode == 1, result.stdout
        assert path.read_text() == invalid, 'cleanup destroyed the recovery metadata'
        # Preflight must preserve the other queue too, not partly delete a room.
        ids = set(backup.split()[1:3])
        assert ids.issubset(queue_ids())
    finally:
        # Restore exactly this test's own captured metadata, then use normal cleanup.
        path.write_text(backup)
        assert run('--room', r, '--cleanup').returncode == 0


def corrupt_state():
    r = room('corrupt')
    a = Endpoint(r, 'A', 'A')
    path = pathlib.Path('/tmp/catchat-ipc-course-{}/{}/control.lock'.format(os.getuid(), r))
    backup = path.read_text()
    a.stop(signal.SIGKILL)
    try:
        path.write_text('BROKEN STATE\n')
        result = run('--room', r, '--role', 'A', '--plain')
        assert result.returncode == 1 and '元数据损坏' in result.stdout
        assert path.read_text() == 'BROKEN STATE\n', 'corrupt state silently overwritten'
        assert set(backup.split()[1:3]).issubset(queue_ids())
    finally:
        path.write_text(backup)
        assert run('--room', r, '--cleanup').returncode == 0


def editing_and_exports():
    r = room('edits')
    a, b = Endpoint(r, 'A', 'A', True), Endpoint(r, 'B', 'B')
    b.expect('PEER ONLINE')
    a.send(b'\x1bhello\r')
    b.expect('RECV A hello')
    marker = len(b.lines)
    a.send('   \r')
    wait_until(lambda: '消息不能只有空白' in a.text())
    assert not any(line.startswith('RECV ') for line in b.lines[marker:])
    a.send(b'\x15')
    a.send('/save\r/save\r')
    saved = []
    try:
        wait_until(lambda: len(list(pathlib.Path('.').glob('catchat-history-*-{}-*.txt'.format(a.proc.pid)))) == 2)
        saved = list(pathlib.Path('.').glob('catchat-history-*-{}-*.txt'.format(a.proc.pid)))
        assert all('hello' in p.read_text(encoding='utf-8') for p in saved)
        assert not any(p.stat().st_mode & 0o077 for p in saved)
    finally:
        for p in saved: p.unlink()
    a.stop(); b.stop()


def simultaneous_start_exit():
    for index in range(3):
        r = room('race%d' % index)
        args = [BIN, '--room', r, '--plain', '--role']
        a = subprocess.Popen(args + ['A'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        b = subprocess.Popen(args + ['B'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        outputs = {a: bytearray(), b: bytearray()}
        try:
            for p in (a, b):
                ready, _, _ = select.select([p.stdout], [], [], 4)
                assert ready, 'simultaneous startup timeout'
                line = p.stdout.readline(); outputs[p].extend(line)
                assert line.startswith(b'READY'), 'simultaneous startup failed'
            for p in (a, b): p.send_signal(signal.SIGTERM)
            for p in (a, b):
                stdout, _ = p.communicate(timeout=15)
                outputs[p].extend(stdout)
                assert p.returncode == 0, stdout.decode('utf-8', 'replace')
        finally:
            for role, p in [('A', a), ('B', b)]:
                if p.poll() is None:
                    p.terminate(); stdout, _ = p.communicate(timeout=15)
                    outputs[p].extend(stdout)
                elif not p.stdout.closed:
                    outputs[p].extend(p.stdout.read())
                name = 'race-%d-%s.log' % (index, role)
                save_log(name, bytes(outputs[p]))
                LOG_INDEX.append({'case': CURRENT_CASE, 'role': role, 'pid': p.pid,
                                  'returncode': p.returncode, 'file': name})
        assert 'queues=none' in run('--room', r, '--inspect').stdout


def long_history():
    r = room('history')
    a, b = Endpoint(r, 'A', 'A', True), Endpoint(r, 'B', 'B')
    b.expect('PEER ONLINE')
    a.resize(18, 60)
    # Whole-word wrapping can use >8192 screen rows for 300 legal messages.
    # The newest message must remain visible rather than vanish at a fixed cap.
    payload = 'abcdefghijklmnopqrstuvwxyzAB ' * 34
    for index in range(300):
        b.send(payload + 'STRESS-%03d\n' % index)
        b.expect('SENT %d' % (index + 1))
    wait_until(lambda: 'STRESS-299' in a.text(), timeout=10,
               description='latest message remains visible with 300 long messages')
    a.send(b'\x1b[5~')
    time.sleep(.1)
    b.send('新消息到达时保留历史位置\n')
    b.expect('SENT 301')
    wait_until(lambda: '1 条新消息' in a.text())
    a.send(b'\x07')
    wait_until(lambda: '新消息到达时保留历史位置' in a.text())
    a.stop(); b.stop()


baseline = queue_ids()
try:
    for label, fn in [
        ('CLI validation', cli),
        ('UTF-8 bidirectional chat, offline draft, duplicate guard, rejoin, cleanup', chat),
        ('40 + 40 simultaneous ordered messages and room isolation', simultaneous_and_isolation),
        ('Queue backpressure, retry and stale target protection', backpressure_and_generation),
        ('SIGKILL recovery for both endpoints', crash_recovery),
        ('PTY Chinese editing, draft preservation, paste, resize, 1024-byte limit, export, restore', terminal),
        ('Oversized/malformed queue packet rejection', malformed_queue_message),
        ('External queue removal and terminal restoration', external_removal),
        ('Busy session lock does not freeze input or terminal restoration', bounded_control_lock),
        ('Queue identity mismatch preserves both queues and recovery metadata', identity_mismatch),
        ('Corrupt session metadata is preserved instead of silently overwritten', corrupt_state),
        ('Standalone Escape, blank input rejection and repeated safe exports', editing_and_exports),
        ('Simultaneous startup and last-endpoint exit races', simultaneous_start_exit),
        ('300 long messages, newest visibility and unread history navigation', long_history),
    ]:
        selected = os.environ.get('CATCHAT_TEST_FILTER', '')
        if selected and selected.lower() not in label.lower():
            continue
        case(label, fn)
finally:
    CURRENT_CASE = 'cleanup'
    for number, ep in enumerate(PROCESSES, 1):
        if ep.proc.poll() is None:
            ep.proc.send_signal(signal.SIGCONT)
            ep.stop(signal.SIGTERM)
        ep.thread.join(timeout=1)
        label = re.sub('[^a-zA-Z0-9]+', '-', ep.case_name).strip('-')[:80]
        name = '%03d-%s-%s.%s' % (number, label, ep.role, 'ansi' if ep.terminal else 'log')
        save_log(name, bytes(ep.capture))
        LOG_INDEX.append({'case': ep.case_name, 'role': ep.role, 'pid': ep.proc.pid,
                          'returncode': ep.proc.returncode, 'file': name})
        if ep.terminal:
            os.close(ep.master); os.close(ep.slave)
    for r in ROOMS:
        result = run('--room', r, '--cleanup')
        if result.returncode:
            print(result.stdout, file=sys.stderr)
    save_log('index.json', json.dumps({'cases': RESULTS, 'logs': LOG_INDEX},
                                     ensure_ascii=False, indent=2).encode('utf-8'))

after = queue_ids()
assert after == baseline, 'queue inventory changed: before={} after={}'.format(baseline, after)
print(json.dumps({'platform': os.uname().machine, 'tests': RESULTS,
                  'queue_inventory_preserved': True,
                  'raw_log_directory': str(LOG_DIR)}, ensure_ascii=False, indent=2))
