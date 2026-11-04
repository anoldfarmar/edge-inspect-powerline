#!/usr/bin/env python3
"""Real board verification; Python is an offline reference/test driver only."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', type=Path)
    ap.add_argument('--full', action='store_true', help='Also validate 50 supports, 13 probes and 23 ROI references')
    ap.add_argument('--data-root', type=Path, help='Explicit frozen image root; required for --full')
    args = ap.parse_args()
    if args.full and (args.data_root is None or not args.data_root.is_dir()):
        ap.error('--full requires an existing --data-root; no dataset path is guessed')
    regression = ROOT / 'assets/regression'
    run = (args.run or ROOT / 'artifacts/runs' / ('stage1_' + dt.datetime.now().strftime('%Y%m%d_%H%M%S_%f'))).resolve()
    run.mkdir(parents=True, exist_ok=False)
    binary = ROOT / 'artifacts/build/edgeinspect_cli'
    checks = []
    counter = 0

    def execute(command, fail=False):
        nonlocal counter
        counter += 1
        command = list(map(str, command))
        p = subprocess.run(command, capture_output=True, text=True)
        (run / f'command_{counter:03d}.log').write_text(p.stdout + p.stderr)
        with (run / 'commands.jsonl').open('a') as f:
            f.write(json.dumps(dict(command=command, exit_code=p.returncode, expected_failure=fail)) + '\n')
        if (p.returncode == 0) == fail:
            raise RuntimeError('Unexpected command status: ' + str(command) + '\n' + p.stdout + p.stderr)
        return p

    def cli(*arguments, fail=False):
        return execute([binary, '--root', ROOT, *arguments], fail)

    def check(name, condition, **details):
        checks.append(dict(name=name, passed=bool(condition), **details))
        if not condition:
            raise AssertionError(name)

    try:
        execute(['ctest', '--test-dir', binary.parent, '--output-on-failure'])
        kernels = json.loads((ROOT / 'assets/source_manifest.json').read_text())['files']
        check('original_algorithm_sources_unchanged', all(sha(ROOT / x['file']) == x['sha256'] for x in kernels), files=len(kernels))
        preprocess = run / 'preprocessing'; preprocess.mkdir()
        if args.full:
            manifest = json.loads((regression / 'support_cache/support_features_manifest.json').read_text())
            rows = []
            for sample in manifest['rows']:
                sid = sample['id']; image = args.data_root / sample['relative_path']
                target = preprocess / (sid + '.rgb')
                cli('--image', image, '--preprocess-only', '--rgb-out', target, '--output', target.with_suffix('.json'))
                with Image.open(image) as im:
                    reference = im.convert('RGB').resize((320, 320), Image.Resampling.BILINEAR).tobytes()
                rows.append(dict(id=sid, equal_pil=target.read_bytes() == reference,
                                 equal_cached_rgb=sha(target) == sample['input_rgb_sha256']))
            check('support50_rgb_exact', all(x['equal_pil'] and x['equal_cached_rgb'] for x in rows), cases=rows)
            print('50 support RGB inputs: exact', flush=True)

        # Exercise resize rounding, expansion, identity, shrink, gray/alpha and ignored EXIF.
        rng = np.random.default_rng(3001)
        synthetic = []
        for index, (w, h, mode) in enumerate([(1, 7, 'RGB'), (320, 320, 'RGB'), (321, 240, 'RGB'),
                                             (1930, 1080, 'RGB'), (17, 13, 'RGBA'), (99, 200, 'L')]):
            shape = (h, w) if mode == 'L' else (h, w, len(mode))
            image = preprocess / f'synthetic_{index}.png'
            Image.fromarray(rng.integers(0, 256, shape, dtype=np.uint8)).save(image)
            target = image.with_suffix('.rgb')
            cli('--image', image, '--preprocess-only', '--rgb-out', target, '--output', image.with_suffix('.json'))
            with Image.open(image) as im:
                reference = im.convert('RGB').resize((320, 320), Image.Resampling.BILINEAR).tobytes()
            synthetic.append(dict(name=image.name, exact=target.read_bytes() == reference))
        image = preprocess / 'exif_progressive.jpg'
        exif = Image.Exif(); exif[274] = 6
        Image.fromarray(rng.integers(0, 256, (199, 401, 3), dtype=np.uint8)).save(image, progressive=True, exif=exif)
        cli('--image', image, '--preprocess-only', '--rgb-out', image.with_suffix('.rgb'), '--output', image.with_suffix('.json'))
        with Image.open(image) as im:
            reference = im.convert('RGB').resize((320, 320), Image.Resampling.BILINEAR).tobytes()
        synthetic.append(dict(name=image.name, exact=image.with_suffix('.rgb').read_bytes() == reference))
        check('synthetic_rgb_exact', all(x['exact'] for x in synthetic), cases=synthetic)

        if args.full:
            # New engine predictions/P3 must equal the original real board tensors.
            probes = json.loads((regression / 'probes13/BOARD_FIXTURES.json').read_text())['probes']
            probe_dir = run / 'probes13'; probe_dir.mkdir()
            comparisons = []
            for p in probes:
                output = probe_dir / (p['name'] + '.json')
                cli('--rgb', regression / 'probes13' / p['input_rgb'], '--asset', p['asset'] or 'glass-insulator', '--output', output, '--diagnostics')
                tensors = Path(str(output) + '.tensors')
                old = regression / 'probes13/board_check/run_20261021_214224_813015'
                errors = {}
                for new, previous in [('predictions.f32', '.output0.f32'), ('backbone_p3.f32', '.output1.f32')]:
                    a = np.fromfile(tensors / new, '<f4'); b = np.fromfile(old / (p['name'] + previous), '<f4')
                    errors[new] = dict(bitwise_equal=a.tobytes() == b.tobytes(), max_abs=float(np.max(np.abs(a.astype('f8')-b))))
                comparisons.append(dict(id=p['name'], tensors=errors))
            check('probes13_original_board_parity', all(v['bitwise_equal'] for x in comparisons for v in x['tensors'].values()), cases=comparisons,
                  scope='Same unchanged RKNN model and exact RGB on same board; not server FP32/FP16 accuracy acceptance')
            print('13 real RKNN predictions/P3: identical to old board', flush=True)

            # Execute the unchanged deployment ROI fixture and the original comparator.
            package = regression / 'roi23'; roi = run / 'roi23'; roi.mkdir()
            for p in json.loads((package / 'MANIFEST.json').read_text())['cases']:
                execute([binary.parent / 'third_party/insplad/insplad_roi_fixture', package / p['p3']['file'], package / p['box']['file'],
                         roi / (p['id'] + '.roi.f32'), roi / (p['id'] + '.normalized_roi.f32')])
            execute([sys.executable, ROOT / 'tests/compare_roi_reference.py', '--package', package,
                     '--outputs', roi, '--report', run / 'roi23_comparison.json'])
            check('roi23_fixed_fp32_gate', json.loads((run / 'roi23_comparison.json').read_text())['passed'], atol=1e-4, rtol=1e-4)

        # Same raw bytes into old executable and new image CLI, normal + abnormal.
        samples = [(x['id'], x['file']) for x in json.loads((ROOT / 'samples/manifest.json').read_text())['samples']]
        jobs = []
        for label, original_path in samples:
            image = ROOT / original_path
            cli('--image', image, '--asset', 'glass-insulator', '--output', run / (label + '.json'), '--rgb-out', run / (label + '.rgb'), '--diagnostics')
            with Image.open(image) as im:
                reference = im.convert('RGB').resize((320, 320), Image.Resampling.BILINEAR).tobytes()
            check(label + '_query_rgb_exact', (run / (label + '.rgb')).read_bytes() == reference)
            jobs.append('\t'.join([label, str(run / (label + '.rgb')), 'glass-insulator', '7']))
        (run / 'old_jobs.tsv').write_text('\n'.join(jobs) + '\n')
        execute([ROOT / 'artifacts/build/third_party/insplad/insplad_rknn_eval', ROOT / 'models/insplad_shared_p3_320_fp.rknn', ROOT / 'banks/matched_rknn',
                 run / 'old_jobs.tsv', run / 'old_scores.jsonl', '1', '0', '4'])
        sample_results = []
        for old in map(json.loads, (run / 'old_scores.jsonl').read_text().splitlines()):
            new = json.loads((run / (old['id'] + '.json')).read_text())['runs'][0]
            sample_results.append(dict(label=old['id'], score=new['score'], previous_score=old['score'],
                difference=abs(new['score']-old['score']), box_equal=new['box'] == old['box'], fallback_equal=new['fallback'] == old['fallback']))
        check('normal_anomaly_vs_original_program', all(x['difference'] == 0 and x['box_equal'] and x['fallback_equal'] for x in sample_results), cases=sample_results)

        rgb = run / 'normal.rgb'
        # Failures produce a nonzero status and no success result.
        bad = run / 'corrupt.jpg'; bad.write_bytes(b'not a JPEG')
        truncated = run / 'truncated.jpg'; truncated.write_bytes(image.read_bytes()[:-100])
        oversized = run / 'oversized.png'; Image.new('RGB', (5000, 5000)).save(oversized)
        short = run / 'short.rgb'; short.write_bytes(b'0')
        cmyk = run / 'cmyk.jpg'; Image.new('CMYK', (10, 10)).save(cmyk)
        failures = [
            ('corrupt', ['--image', bad]), ('truncated', ['--image', truncated]), ('oversized', ['--image', oversized]),
            ('byte_limit', ['--image', image, '--max-bytes', '1']), ('short_rgb', ['--rgb', short]),
            ('unsupported_cmyk', ['--image', cmyk]), ('no_bank', ['--rgb', rgb, '--asset', 'unknown']),
            ('missing_model', ['--rgb', rgb, '--root', run / 'empty_root']),
        ]
        for name, flags in failures:
            out = run / ('failure_' + name + '.json')
            command = [binary, '--root', ROOT, '--asset', 'glass-insulator', '--output', out]
            # Override by constructing a unique option list, not relying on last-wins parsing.
            if '--root' in flags:
                command = [binary, '--asset', 'glass-insulator', '--output', out]
            if '--asset' in flags:
                command = [binary, '--root', ROOT, '--output', out]
            execute([*command, *flags], fail=True)
            check('failure_' + name, not out.exists())
        existing = run / 'existing.json'; existing.write_text('KEEP\n')
        cli('--rgb', rgb, '--asset', 'glass-insulator', '--output', existing, fail=True)
        check('existing_result_preserved', existing.read_text() == 'KEEP\n')
        cli('--rgb', rgb, '--asset', 'glass-insulator', '--output', run / 'absent_directory/result.json', fail=True)
        check('result_save_failure', not (run / 'absent_directory/result.json').exists())

        execute([binary.parent / 'edgeinspect_lifecycle_test', ROOT, rgb])
        check('lifecycle_and_post_init_failure', True, note='See command log: real contexts, invalid request then valid, successful and failed constructor cleanup, descriptor counts')
        cli('--rgb', rgb, '--asset', 'glass-insulator', '--repeat', '100', '--output', run / 'repeat100.json')
        repeat = json.loads((run / 'repeat100.json').read_text())
        times = [r['timing_ms']['inference_total'] for r in repeat['runs']]
        check('repeat100_one_initialized_engine', repeat['engine_initializations'] == 1 and len(times) == 100 and
              len({r['score'] for r in repeat['runs']}) == 1,
              peak_rss_kib=repeat['peak_rss_kib'], mean_ms=statistics.mean(times), median_ms=statistics.median(times), p95_ms=float(np.percentile(times, 95)),
              scope='Unwarmed repeated same RGB, real forward each time; excludes file preprocessing and initialization; not a service/video benchmark or proof of zero memory leaks')
    except Exception as e:
        checks.append(dict(name='execution_error', passed=False, error=str(e)))
    finally:
        sources = {str(p.relative_to(ROOT)): sha(p) for folder in ['apps', 'include', 'src', 'tests', 'third_party/insplad'] for p in (ROOT / folder).rglob('*') if p.is_file() and '__pycache__' not in str(p)}
        passed = bool(checks) and all(c['passed'] for c in checks)
        report = dict(passed=passed, mode='full' if args.full else 'basic', checks=checks, source_sha256=sources, binary_sha256=sha(binary),
                      model_sha256=sha(ROOT / 'models/insplad_shared_p3_320_fp.rknn'), run=str(run),
                      limitations=['No queue/SQLite/HTTP/systemd implementation in stage1.', 'No anomaly threshold or full AUROC acceptance invented.',
                                   'Actual RKNN run-error and output-get-error fault injection not performed.', 'RGB equivalence applies to recorded cases, not all possible JPEG decoders/images.'])
        (run / 'comparison.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        (run / 'REPORT.md').write_text('# EdgeInspect stage1 actual validation\n\n' + '\n'.join(f"- {x['name']}: {'PASS' if x['passed'] else 'FAIL'}" for x in checks) + '\n\nSee comparison.json and commands.jsonl for numerical evidence and scope.\n')
        print(json.dumps(dict(passed=passed, run=str(run), check_count=len(checks)), indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
