#!/usr/bin/env python3
"""Actual RK3588 task/CLI parity checks. Uses local samples; no full dataset claim."""
import argparse
import datetime as dt
import json
from pathlib import Path
import queue
import shutil
import sqlite3
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


class Session:
    def __init__(self, run, data, root=ROOT):
        log = run / (data.name + '.log')
        suffix = 1
        while log.exists():
            log = run / (data.name + f'_{suffix}.log')
            suffix += 1
        self.log = log.open('x')
        self.events = queue.Queue()
        self.process = subprocess.Popen(
            [str(ROOT / 'artifacts/build/edgeinspect_tasks'), '--root', str(root),
             '--data-root', str(data)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.log.write(line)
            self.log.flush()
            try:
                value = json.loads(line)
            except (ValueError, TypeError):
                continue  # RKNN Runtime can print non-JSON diagnostics.
            self.events.put(value)
        self.events.put(None)

    def send(self, text):
        self.process.stdin.write(text + '\n')
        self.process.stdin.flush()

    def receive(self, predicate=lambda value: True, timeout=60):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            item = self.events.get(timeout=max(0.001, end - time.monotonic()))
            if item is None:
                raise RuntimeError('Task process exited before expected response')
            if predicate(item):
                return item
        raise TimeoutError('Task response timeout')

    def close(self):
        if self.process.poll() is None:
            self.send('quit')
        code = self.process.wait(timeout=60)
        self.reader.join(timeout=5)
        self.log.close()
        return code

    def terminate(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
        self.reader.join(timeout=5)
        self.log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path)
    args = parser.parse_args()
    run = (args.run or ROOT / 'artifacts/runs' / ('stage2_' + dt.datetime.now().strftime('%Y%m%d_%H%M%S_%f'))).resolve()
    run.mkdir(parents=True, exist_ok=False)
    checks = []
    sessions = []

    def check(name, condition, **details):
        checks.append(dict(name=name, passed=bool(condition), **details))
        if not condition:
            raise AssertionError(name)

    try:
        data = run / 'service'
        source = run / 'normal source with spaces.jpg'
        shutil.copyfile(ROOT / 'samples/normal.jpg', source)
        corrupt = run / 'corrupt.jpg'
        corrupt.write_bytes(b'not an image')
        session = Session(run, data)
        sessions.append(session)
        ready = session.receive(lambda x: x.get('event') == 'READY')
        check('real_engine_ready', ready['engine_initializations'] == 1)
        ids = {}
        for name, image in [('normal', source), ('corrupt', corrupt), ('anomaly', ROOT / 'samples/anomaly.jpg')]:
            session.send(f'submit --image "{image}" --asset glass-insulator')
            accepted = session.receive(lambda x: x.get('event') == 'ACCEPTED' or 'error_code' in x)
            check(name + '_accepted', accepted.get('event') == 'ACCEPTED')
            ids[name] = accepted['task_id']
            if name == 'normal':
                source.write_bytes(b'changed after acceptance')
        records = {}
        for name, task_id in ids.items():
            session.send('wait ' + task_id)
            records[name] = session.receive(lambda x: x.get('task_id') == task_id or 'error_code' in x)
        check('valid_tasks_succeeded', all(records[x]['status'] == 'SUCCEEDED' for x in ['normal', 'anomaly']))
        check('bad_image_failed_and_worker_continued', records['corrupt']['status'] == 'FAILED'
              and records['corrupt']['error_code'] == 'INVALID_INPUT')
        session.send('get does-not-exist')
        check('not_found', session.receive().get('error_code') == 'NOT_FOUND')
        session.send('submit --image "' + str(ROOT / 'samples/normal.jpg') + '" --asset unknown')
        check('invalid_asset_rejected', session.receive().get('error_code') == 'INVALID_ASSET')
        for name in ['normal', 'anomaly']:
            session.send('result ' + ids[name])
            result = session.receive(lambda x: 'score' in x)
            check(name + '_uncalibrated', result['is_anomaly'] is None and result['anomaly_threshold'] is None)
        check('normal_shutdown', session.close() == 0)
        sessions.remove(session)
        with sqlite3.connect(data / 'tasks.sqlite3') as db:
            check('only_accepted_tasks_in_database', db.execute('SELECT COUNT(*) FROM tasks').fetchone()[0] == 3)
        for name in ['normal', 'anomaly']:
            actual = json.loads((data / records[name]['result_path']).read_text())
            saved_input = data / records[name]['input_path']
            check(name + '_saved_copy', saved_input.read_bytes() == (ROOT / f'samples/{name}.jpg').read_bytes())
            reference = run / (name + '_stage1.json')
            result = subprocess.run([str(ROOT / 'artifacts/build/edgeinspect_cli'), '--root', str(ROOT),
                                     '--image', str(ROOT / f'samples/{name}.jpg'), '--asset', 'glass-insulator',
                                     '--output', str(reference)], capture_output=True, text=True, timeout=60)
            (run / (name + '_stage1.log')).write_text(result.stdout + result.stderr)
            check(name + '_stage1_cli_success', result.returncode == 0)
            old = json.loads(reference.read_text())['runs'][0]
            fields = ['score', 'box', 'fallback', 'confidence', 'input_sha256', 'rgb_sha256',
                      'bank_sha256', 'is_anomaly', 'anomaly_threshold', 'decision']
            check(name + '_exact_stage1_parity', all(actual[k] == old[k] for k in fields),
                  score=actual['score'], compared_fields=fields)
        reopened = Session(run, data)
        sessions.append(reopened)
        reopened.receive(lambda x: x.get('event') == 'READY')
        reopened.send('get ' + ids['normal'])
        check('history_after_restart', reopened.receive().get('status') == 'SUCCEEDED')
        check('reopened_shutdown', reopened.close() == 0)
        sessions.remove(reopened)
        missing = Session(run, run / 'missing-model', root=run / 'absent-model-root')
        sessions.append(missing)
        failure = missing.receive(lambda x: 'error_code' in x)
        check('missing_model_no_ready', failure['error_code'] == 'STARTUP_ERROR')
        check('startup_error_nonzero', missing.close() != 0)
        sessions.remove(missing)
    except Exception as exc:
        checks.append(dict(name='execution_error', passed=False, error=str(exc)))
    finally:
        for session in sessions:
            session.terminate()
        passed = bool(checks) and all(x['passed'] for x in checks)
        report = dict(passed=passed, checks=checks, run=str(run),
                      scope='Two local sample images, one corrupt file, actual NPU task execution; no HTTP/video/full dataset evaluation.')
        (run / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        print(json.dumps(dict(passed=passed, run=str(run), checks=len(checks))))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
