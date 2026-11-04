#!/usr/bin/env python3
"""Compare real C++ ROI outputs with frozen FP32 references; NumPy only, no RKNN."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def errors(actual, expected, atol=1e-4, rtol=1e-4):
    a = np.asarray(actual, dtype=np.float64)
    b = np.asarray(expected, dtype=np.float64)
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} != {b.shape}")
    finite = bool(np.isfinite(a).all() and np.isfinite(b).all())
    difference = np.abs(a - b)
    violations = (~np.isfinite(a)) | (difference > atol + rtol * np.abs(b))
    reference_norm = float(np.linalg.norm(b.ravel()))
    difference_norm = float(np.linalg.norm((a - b).ravel())) if finite else None
    return dict(
        passed=finite and not bool(violations.any()), finite=finite,
        max_abs=float(difference.max()) if finite else None,
        mean_abs=float(difference.mean()) if finite else None,
        difference_l2=difference_norm, reference_l2=reference_norm,
        relative_l2=(difference_norm / reference_norm
                     if finite and reference_norm > 0 else None),
        zero_reference=reference_norm == 0,
        violation_count=int(violations.sum()), elements=int(b.size),
    )


def load_array(base, specification):
    path = base / specification['file']
    if path.stat().st_size != specification['bytes']:
        raise ValueError(f"Wrong file length: {path}")
    if sha256(path) != specification['sha256']:
        raise ValueError(f"SHA256 mismatch: {path}")
    return np.fromfile(path, dtype='<f4').reshape(specification['shape'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('--outputs', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.package / 'MANIFEST.json').read_text())
    gate = manifest['comparison']
    rows = []
    for case in manifest['cases']:
        row = dict(id=case['id'], passed=False, comparisons={})
        try:
            # Validate the actual inputs too, not only the expected outputs.
            load_array(args.package, case['p3'])
            load_array(args.package, case['box'])
            for field, suffix in [('expected_roi', 'roi'),
                                  ('expected_normalized_roi', 'normalized_roi')]:
                expected = load_array(args.package, case[field])
                output = args.outputs / (case['id'] + '.' + suffix + '.f32')
                if output.stat().st_size != expected.size * 4:
                    raise ValueError(f"Wrong output length: {output}")
                actual = np.fromfile(output, dtype='<f4').reshape(expected.shape)
                row['comparisons'][suffix] = errors(actual, expected, gate['atol'], gate['rtol'])
                row['comparisons'][suffix]['output_sha256'] = sha256(output)
            row['passed'] = all(v['passed'] for v in row['comparisons'].values())
        except (OSError, ValueError) as exc:
            row['error'] = str(exc)
        rows.append(row)
    report = dict(
        scope='Same FP32 P3 and same fixed box: C++ postprocessing only; NOT NPU precision or AUROC',
        passed=bool(rows) and all(row['passed'] for row in rows),
        manifest_sha256=sha256(args.package / 'MANIFEST.json'),
        case_count=len(rows), passed_cases=sum(row['passed'] for row in rows),
        comparison=gate, results=rows,
    )
    # Never overwrite a previous test report.
    with args.report.open('x', encoding='utf-8') as fp:
        json.dump(report, fp, ensure_ascii=False, indent=2, allow_nan=False)
        fp.write('\n')
    print(json.dumps({k: report[k] for k in ('passed', 'case_count', 'passed_cases')}, indent=2))
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
