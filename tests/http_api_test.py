#!/usr/bin/env python3
"""Deterministic loopback HTTP tests with a controlled processor, no NPU calls."""
import concurrent.futures
import json
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

from http_support import HttpSession, multipart


def main():
    binary = Path(sys.argv[1]).resolve()
    sessions = []
    checked = 0
    with tempfile.TemporaryDirectory(prefix='edgeinspect-http-') as directory:
        root = Path(directory)

        def check(condition, message):
            nonlocal checked
            assert condition, message
            checked += 1

        def start(name, capacity=1, max_bytes=1024):
            data = root/name
            session = HttpSession([binary, data, capacity, max_bytes], data, root/(name+'.log'))
            sessions.append(session)
            return session

        def expect(reply, status, code):
            check(reply[0] == status and reply[1].get('error_code') == code,
                  f'Expected {status}/{code}, got {reply}')

        def clean_uploads(session):
            return all(not list(p.iterdir()) for p in session.data_root.glob('.http-uploads-*'))

        try:
            session = start('basic')
            check(session.ready['listen'] == '127.0.0.1', 'loopback binding')
            check(session.request('GET','/health')[0] == 200, 'health')
            reply = session.upload(b'hold', filename='../../outside.jpg')
            check(reply[0] == 202 and reply[2]['Location'] == reply[1]['status_url'], 'accepted with Location')
            a = reply[1]['task_id']
            until = time.monotonic()+3
            while session.request('GET','/tasks/'+a)[1]['status'] != 'RUNNING':
                assert time.monotonic() < until
                time.sleep(.005)
            reply = session.upload(b'ok', chunked=True)
            check(reply[0] == 202, 'chunked upload')
            b = reply[1]['task_id']
            check(session.request('GET','/tasks/'+b)[1]['status'] == 'QUEUED', 'waiting state')
            expect(session.upload(b'excess'),429,'QUEUE_FULL')
            expect(session.request('GET','/tasks/'+a+'/result'),409,'NOT_READY')
            check(session.request('GET','/tasks?limit=2')[1]['tasks'].__len__() == 2, 'list')
            for query in ['limit=0','limit=1001','limit=x','limit=1&limit=2','other=x']:
                expect(session.request('GET','/tasks?'+query),400,'INVALID_ARGUMENT')
            expect(session.request('GET','/tasks/invalid'),400,'INVALID_ARGUMENT')
            expect(session.request('GET','/tasks/'+'f'*32),404,'NOT_FOUND')
            expect(session.request('GET','/tasks/'+'f'*32+'/result'),404,'NOT_FOUND')
            expect(session.request('GET','/not-an-api'),404,'NOT_FOUND')
            expect(session.request('POST','/tasks',b'{}',{'Content-Type':'application/json'}),415,'UNSUPPORTED_MEDIA_TYPE')
            expect(session.upload(b'ok',asset='unknown'),400,'INVALID_ASSET')
            expect(session.upload(b''),400,'INVALID_ARGUMENT')
            for parts in [
                [('asset',None,b'glass-insulator')],
                [('image','x.jpg',b'ok')],
                [('image','x',b'ok'),('image','y',b'ok')],
                [('asset',None,b'glass-insulator'),('asset',None,b'glass-insulator'),('image','x',b'ok')],
                [('asset','file.txt',b'glass-insulator'),('image','x',b'ok')],
                [('other',None,b'x'),('image','x',b'ok')],
            ]:
                expect(session.upload(b'',parts=parts),400,'INVALID_ARGUMENT')
            expect(session.upload(b'x'*1025),413,'PAYLOAD_TOO_LARGE')
            expect(session.upload(b'x'*1025,chunked=True),413,'PAYLOAD_TOO_LARGE')
            expect(session.upload(b'x',asset='a'*129),400,'INVALID_ASSET')
            expect(session.request('POST','/tasks',b'x'*20000,{'Content-Type':'multipart/form-data; boundary=x'}),413,'PAYLOAD_TOO_LARGE')
            body, content_type = multipart([('image','a',b'ok'),('asset',None,b'glass-insulator')])
            expect(session.request('POST','/tasks',body[:-20],{'Content-Type':content_type}),400,'INVALID_ARGUMENT')
            expect(session.request('POST','/tasks',body,{'Content-Type':content_type,'Content-Encoding':'gzip'}),415,'UNSUPPORTED_MEDIA_TYPE')
            # Disconnect midway through a declared multipart body: no accepted task,
            # and any request-owned temporary upload must be removed.
            sock = socket.create_connection(('127.0.0.1',session.port),timeout=2)
            header = (f'POST /tasks HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: {content_type}\r\n'
                      f'Content-Length: {len(body)}\r\n\r\n').encode()
            sock.sendall(header+body[:-20]); sock.shutdown(socket.SHUT_RDWR); sock.close()
            time.sleep(.02)
            until = time.monotonic()+2
            while not clean_uploads(session) and time.monotonic() < until:
                time.sleep(.01)
            check(clean_uploads(session), 'all rejected/disconnected/accepted upload spool files cleaned')
            with sqlite3.connect(session.data_root/'tasks.sqlite3') as db:
                check(db.execute('SELECT COUNT(*) FROM tasks').fetchone()[0] == 2, 'rejections leave no task rows')
            check(len(list((session.data_root/'tasks').iterdir())) == 2, 'queue full leaves no input directory')
            conflict = subprocess.run([str(binary),str(root/'port-conflict'),'1','1024',str(session.port)],
                                      capture_output=True,text=True,timeout=5)
            check(conflict.returncode == 2 and 'BIND_ERROR' in conflict.stderr, 'occupied port fails startup')
            (session.data_root/'release').touch()
            check(session.wait(a)['status'] == 'SUCCEEDED' and session.wait(b)['status'] == 'SUCCEEDED', 'FIFO tasks finish')
            check(session.request('GET','/tasks/'+a+'/result')[1]['score'] == .25, 'result JSON')
            c = session.upload(b'bad')[1]['task_id']
            check(session.wait(c)['error_code'] == 'INVALID_INPUT', 'bad image failure recorded')
            expect(session.request('GET','/tasks/'+c+'/result'),409,'TASK_FAILED')
            d = session.upload(b'good-after-bad')[1]['task_id']
            check(session.wait(d)['status'] == 'SUCCEEDED', 'worker continues')
            session.close(); sessions.remove(session)
            stats = json.loads((session.data_root/'fixture-counts.json').read_text())
            check(stats == {'initializations':1,'calls':4,'max_active':1}, 'one nonconcurrent processor')
            check(not list(session.data_root.glob('.http-uploads-*')), 'upload directory cleaned on exit')

            session = start('concurrent',capacity=16)
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                replies = list(pool.map(lambda _: session.upload(b'ok'),range(8)))
            check(all(r[0] == 202 for r in replies), 'concurrent clients accepted')
            for reply in replies:
                check(session.wait(reply[1]['task_id'])['status'] == 'SUCCEEDED', 'concurrent task completed')
            session.close(); sessions.remove(session)
            check(json.loads((session.data_root/'fixture-counts.json').read_text())['max_active'] == 1,'HTTP workers share one inference worker')

            session = start('drain',capacity=2)
            a = session.upload(b'hold')[1]['task_id']
            b = session.upload(b'ok')[1]['task_id']
            session.process.send_signal(signal.SIGTERM)
            time.sleep(.1)
            check(session.process.poll() is None,'shutdown waits for active task')
            (session.data_root/'release').touch()
            session.close(send_signal=False); sessions.remove(session)
            with sqlite3.connect(session.data_root/'tasks.sqlite3') as db:
                check(db.execute("SELECT COUNT(*) FROM tasks WHERE status='SUCCEEDED'").fetchone()[0] == 2,'shutdown drains accepted tasks')

            session = start('db-failure')
            with sqlite3.connect(session.data_root/'tasks.sqlite3') as db:
                db.execute("CREATE TRIGGER fail_insert BEFORE INSERT ON tasks BEGIN SELECT RAISE(ABORT,'injected insert error'); END")
            expect(session.upload(b'ok'),500,'DATABASE_ERROR')
            expect(session.request('GET','/health'),503,'SERVICE_FAULT')
            expect(session.upload(b'ok'),503,'NOT_ACCEPTING')
            check(clean_uploads(session),'database failure upload cleaned')
            session.close(expected=2); sessions.remove(session)

            session = start('device-failure')
            a = session.upload(b'fatal')[1]['task_id']
            check(session.wait(a)['error_code'] == 'INFERENCE_ERROR','device failure recorded')
            expect(session.request('GET','/health'),503,'SERVICE_FAULT')
            session.close(expected=2); sessions.remove(session)
            print(f'HTTP API checks passed: {checked}')
        finally:
            for session in sessions:
                session.abort()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
