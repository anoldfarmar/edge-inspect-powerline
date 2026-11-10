"""Standard-library HTTP test helpers; not used by the C++ application."""
import http.client
import json
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time
import uuid


class HttpSession:
    def __init__(self, command, data_root, log, timeout=60):
        self.data_root = Path(data_root)
        self.log = Path(log).open('x')
        self.events = queue.Queue()
        self.process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        end = time.monotonic() + timeout
        while True:
            try:
                event = self.events.get(timeout=max(0.01, end-time.monotonic()))
            except queue.Empty:
                self.abort()
                raise TimeoutError('HTTP startup timed out')
            if event is None:
                self.reader.join(timeout=1)
                self.log.close()
                raise RuntimeError('HTTP process exited before HTTP_READY')
            if event.get('event') == 'HTTP_READY':
                self.port = event['port']
                self.ready = event
                return

    def _read(self):
        for line in self.process.stdout:
            self.log.write(line)
            self.log.flush()
            try:
                event = json.loads(line)
            except ValueError:
                continue
            self.events.put(event)
        self.events.put(None)

    def request(self, method, path, body=None, headers=None, chunked=False):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers=headers or {}, encode_chunked=chunked)
            response = connection.getresponse()
            payload = response.read()
            value = json.loads(payload)
            assert 'application/json' in response.getheader('Content-Type', ''), payload
            return response.status, value, dict(response.getheaders())
        finally:
            connection.close()

    def upload(self, content, asset='glass-insulator', filename='image.jpg', parts=None, chunked=False):
        body, content_type = multipart(parts if parts is not None else [
            ('asset', None, asset.encode()), ('image', filename, content)])
        chunks = (body[i:i+7] for i in range(0, len(body), 7)) if chunked else body
        return self.request('POST', '/tasks', chunks, {'Content-Type': content_type}, chunked)

    def wait(self, task_id, timeout=30):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            status, value, _ = self.request('GET', '/tasks/'+task_id)
            assert status == 200, value
            if value['status'] in ['SUCCEEDED', 'FAILED']:
                return value
            time.sleep(0.01)
        raise TimeoutError('Task did not reach a terminal state')

    def close(self, expected=0, send_signal=True):
        if send_signal and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        code = self.process.wait(timeout=30)
        self.reader.join(timeout=2)
        self.log.close()
        assert code == expected, f'Expected exit {expected}, got {code}'

    def abort(self):
        # Unblock the fixture processor, when used, before attempting graceful stop.
        if self.data_root.exists():
            (self.data_root / 'release').touch()
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.log.close()


def multipart(parts):
    boundary = 'edgeinspect-'+uuid.uuid4().hex
    body = bytearray()
    for name, filename, content in parts:
        body.extend(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'.encode())
        if filename is not None:
            body.extend(f'; filename="{filename}"'.encode())
        body.extend(b'\r\n')
        if filename is not None:
            body.extend(b'Content-Type: application/octet-stream\r\n')
        body.extend(b'\r\n'+content+b'\r\n')
    body.extend(f'--{boundary}--\r\n'.encode())
    return bytes(body), 'multipart/form-data; boundary='+boundary
