#!/usr/bin/env python3
"""Actual RK3588 HTTP upload -> TaskManager -> inference -> result verification."""
import argparse
import datetime as dt
import json
from pathlib import Path
import sqlite3
import subprocess

from http_support import HttpSession

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path)
    args = parser.parse_args()
    run = (args.run or ROOT/'artifacts/runs'/('stage3_'+dt.datetime.now().strftime('%Y%m%d_%H%M%S_%f'))).resolve()
    run.mkdir(parents=True, exist_ok=False)
    checks = []
    session = None

    def check(name, condition, **details):
        checks.append(dict(name=name, passed=bool(condition), **details))
        if not condition:
            raise AssertionError(name)

    try:
        data = run/'service'
        binary = ROOT/'artifacts/build/edgeinspect_http'
        command = [binary, '--root', ROOT, '--data-root', data, '--port', '0']
        session = HttpSession(command, data, run/'http.log')
        check('real_engine_ready_on_loopback', session.ready['listen'] == '127.0.0.1'
              and session.ready['engine_initializations'] == 1)
        status, value, _ = session.request('GET','/health')
        check('health_ready', status == 200 and value['status'] == 'READY')
        ids = {}
        for name in ['normal','corrupt','anomaly']:
            content = (ROOT/f'samples/{name}.jpg').read_bytes() if name != 'corrupt' else b'broken JPEG'
            status, value, headers = session.upload(content, filename='../../client-name.jpg')
            check(name+'_accepted',status == 202 and headers['Location'] == value.get('status_url'))
            ids[name] = value['task_id']
        records = {name:session.wait(task_id) for name,task_id in ids.items()}
        check('normal_succeeded',records['normal']['status'] == 'SUCCEEDED')
        check('corrupt_failed',records['corrupt']['status'] == 'FAILED' and records['corrupt']['error_code'] == 'INVALID_INPUT')
        check('anomaly_succeeded_after_corrupt',records['anomaly']['status'] == 'SUCCEEDED')
        results = {}
        for name in ['normal','anomaly']:
            status, results[name], _ = session.request('GET','/tasks/'+ids[name]+'/result')
            check(name+'_result_json',status == 200 and results[name]['is_anomaly'] is None
                  and results[name]['anomaly_threshold'] is None)
            check(name+'_exact_input_copy',(data/records[name]['input_path']).read_bytes() == (ROOT/f'samples/{name}.jpg').read_bytes())
        status, value, _ = session.request('GET','/tasks/'+ids['corrupt']+'/result')
        check('failed_result_is_409',status == 409 and value['error_code'] == 'TASK_FAILED')
        status, value, _ = session.request('GET','/tasks?limit=10')
        check('list_contains_three_tasks',status == 200 and len(value['tasks']) == 3)
        status, value, _ = session.upload(b'fake',asset='unknown')
        check('invalid_asset_rejected',status == 400 and value['error_code'] == 'INVALID_ASSET')
        session.close(); session = None
        check('upload_spool_cleaned',not list(data.glob('.http-uploads-*')))
        with sqlite3.connect(data/'tasks.sqlite3') as db:
            check('database_only_accepted_tasks',db.execute('SELECT COUNT(*) FROM tasks').fetchone()[0] == 3)
        for name in ['normal','anomaly']:
            target = run/(name+'_stage1.json')
            reference = subprocess.run([str(ROOT/'artifacts/build/edgeinspect_cli'),'--root',str(ROOT),
                                        '--image',str(ROOT/f'samples/{name}.jpg'),'--asset','glass-insulator',
                                        '--output',str(target)],capture_output=True,text=True,timeout=60)
            (run/(name+'_stage1.log')).write_text(reference.stdout+reference.stderr)
            check(name+'_stage1_cli_success',reference.returncode == 0)
            old = json.loads(target.read_text())['runs'][0]
            fields = ['score','box','fallback','confidence','input_sha256','rgb_sha256','bank_sha256',
                      'is_anomaly','anomaly_threshold','decision']
            check(name+'_exact_stage1_parity',all(results[name][k] == old[k] for k in fields),score=results[name]['score'],compared_fields=fields)
        session = HttpSession(command,data,run/'restart.log')
        status, value, _ = session.request('GET','/tasks/'+ids['normal'])
        check('history_after_restart',status == 200 and value['status'] == 'SUCCEEDED')
        session.close(); session = None
        missing = subprocess.run([str(binary),'--root',str(run/'absent-model'),'--data-root',str(run/'missing'),
                                  '--port','0'],capture_output=True,text=True,timeout=30)
        (run/'missing.log').write_text(missing.stdout+missing.stderr)
        check('missing_model_no_ready',missing.returncode != 0 and 'HTTP_READY' not in missing.stdout)
    except Exception as exc:
        checks.append(dict(name='execution_error',passed=False,error=str(exc)))
    finally:
        if session is not None:
            session.abort()
        passed = bool(checks) and all(x['passed'] for x in checks)
        report = dict(passed=passed,checks=checks,run=str(run),
                      scope='Real loopback HTTP, two local samples and corrupt file, same unchanged model/banks; no full dataset, video, HTTPS or crash-recovery claim.')
        (run/'report.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
        print(json.dumps(dict(passed=passed,run=str(run),checks=len(checks))))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
