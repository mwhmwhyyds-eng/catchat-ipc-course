#!/usr/bin/env python3
"""Capture actual board PTY output for UI review; not synthetic UI mockups."""
import fcntl
import codecs
import hashlib
import json
import os
import pathlib
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time
import uuid

binary = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './build/catchat').resolve())
out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else 'evidence/demo')
out.mkdir(parents=True, exist_ok=True)
room = 'demo-' + uuid.uuid4().hex[:8]
endpoints = []


class Terminal:
    def __init__(self, role, name, session=None, rows=34, cols=118):
        self.role = role
        self.rows, self.cols = rows, cols
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        self.data = bytearray()
        self.events = []
        self.started = time.monotonic()
        self.proc = subprocess.Popen([binary, '--room', session or room, '--role', role, '--name', name],
                                     stdin=self.slave, stdout=self.slave, stderr=self.slave)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        endpoints.append(self)

    def read(self):
        decoder = codecs.getincrementaldecoder('utf-8')('replace')
        while True:
            try:
                ready, _, _ = select.select([self.master], [], [], .1)
                if ready:
                    data = os.read(self.master, 65536)
                    if not data:
                        return
                    self.data.extend(data)
                    self.events.append([round(time.monotonic() - self.started, 6),
                                        'o', decoder.decode(data)])
                elif self.proc.poll() is not None:
                    return
            except OSError:
                return

    def send(self, text):
        os.write(self.master, text.encode('utf-8'))
        time.sleep(.25)

    def snapshot(self, suffix=''):
        name = 'terminal-' + self.role + suffix
        (out / (name + '.ansi')).write_bytes(bytes(self.data))
        with (out / (name + '.cast')).open('w', encoding='utf-8') as f:
            f.write(json.dumps({'version': 2, 'width': self.cols, 'height': self.rows,
                                'env': {'TERM': 'xterm-256color', 'SHELL': 'catchat'}}) + '\n')
            for event in list(self.events):
                f.write(json.dumps(event, ensure_ascii=False) + '\n')


try:
    a = Terminal('A', '用户甲')
    b = Terminal('B', '用户乙')
    time.sleep(.6)
    a.send('你好！这是运行在鲁班猫上的 CatChat。\r')
    b.send('收到了。我们正在通过 System V 消息队列双向通信。\r')
    a.send('聊天区与输入区分开，收到消息也不会打断正在输入的内容。\r')
    b.send('界面右侧可以查看队列编号和收发计数。\r')
    a.send('中文、English 和较长的文字都可以显示；长消息会根据聊天区宽度自动换行。\r')
    a.send('/status\r')
    b.send('这是一条尚未发送的草稿')
    time.sleep(.5)
    a.snapshot(); b.snapshot()
    b.send('\x15')
    for i in range(8):
        b.send('历史消息 %02d：PgUp 可以翻阅之前的聊天内容。\r' % (i + 1))
    a.send('\x1b[5~')
    a.send('翻阅历史时也保留这份草稿')
    a.snapshot('-history-before')
    b.send('这是一条浏览历史期间到达的新消息：当前位置保持不动，底部会提示新消息数量。\r')
    a.snapshot('-history')
    a.send('\x07\x15')
    b.send('\x15/quit\r')
    b.proc.wait(timeout=5)
    time.sleep(.5)
    a.snapshot('-peer-offline')
    small_a = Terminal('A', '用户甲', room + '-small', 18, 60)
    small_b = Terminal('B', '用户乙', room + '-small', 18, 60)
    time.sleep(.6)
    small_a.send('小窗口仍然可以双向聊天。\r')
    small_b.send('容量 16384/16384 bytes，英文和数字尽量整体换行。\r')
    small_a.snapshot('-narrow')
    (out / 'capture.json').write_text(json.dumps({
        'source': 'actual Linux PTY output, captured before exit',
        'platform': os.uname().machine, 'room': room,
        'rows': 34, 'columns': 118, 'board_clock': time.strftime('%Y-%m-%d %H:%M:%S %z'),
        'initial_messages': {'A_to_B': 3, 'B_to_A': 2},
        'version': subprocess.check_output([binary, '--version'], text=True).strip(),
        'binary_sha256': hashlib.sha256(pathlib.Path(binary).read_bytes()).hexdigest(),
    }, ensure_ascii=False, indent=2), encoding='utf-8')
finally:
    for ep in endpoints:
        if ep.proc.poll() is None:
            ep.proc.send_signal(signal.SIGTERM)
            ep.proc.wait(timeout=5)
        ep.reader.join(timeout=1)
        os.close(ep.master); os.close(ep.slave)
print('Captured actual board PTYs into', out)
