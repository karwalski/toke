"""133.1 reproduction: KERN's 60 public Python/Toke pairs, toke side re-run on tkc 2.8.0.

Runs with /tmp/kern-venv/bin/python (tiktoken, python-minifier 3.2.0, toke-tokenizer 0.1.0, tokenizers).
"""
import json, os, re, subprocess, sys, random, statistics, tempfile, types
from pathlib import Path

# --- stub datasets so benchmark_modern imports without the HF dependency
class _Stub(types.ModuleType):
    def __getattr__(self, n):
        return lambda *a, **k: None
for _m in ('datasets', 'evalplus', 'evalplus.data', 'evalplus.evaluate', 'evalplus.sanitize'):
    sys.modules[_m] = _Stub(_m)
sys.path.insert(0, '/tmp/kern-oscar')
from benchmark_toke import build_python_programs, PROBE_INPUTS  # noqa: E402
from kern_transpiler import transpile  # noqa: E402
import python_minifier, tiktoken  # noqa: E402
from toke_tokenizer import count_tokens as toke_native_count  # noqa: E402
from tokenizers import Tokenizer  # noqa: E402

TKC = '/Users/matthew.watt/tk/toke/tkc'
EVAL_851 = Path('/tmp/toke-eval-851')
EVAL_HEAD = Path('/Users/matthew.watt/tk/toke-eval')
OUT = Path('/Users/matthew.watt/tk/toke/docs/about/reviews/kern-2026-08.repro.json')
kern16k = Tokenizer.from_file('/tmp/kern-oscar/benchmark_results/native-tokenizer/kern-16k-tokenizer.json')
enc = {n: tiktoken.get_encoding(n) for n in ('cl100k_base', 'o200k_base')}
ERR = re.compile(r'\bE\d{4}\b')


def run(cmd, timeout=60, inp=None):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, input=inp)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, '', 'timeout'


def first_err(out):
    m = ERR.search(out)
    return m.group(0) if m else ''


def counts(text):
    return {n: len(e.encode(text, disallowed_special=())) for n, e in enc.items()}


def probe_py(src, arg, root, name):
    p = root / f'{name}.py'; p.write_text(src)
    rc, out, err = run([sys.executable, '-I', str(p), json.dumps(arg, separators=(',', ':'))], 30)
    if rc != 0:
        return False, None
    try:
        return True, json.loads(out.strip().splitlines()[-1])
    except Exception:
        return False, None


def probe_toke(src, arg, root, name, legacy):
    s = root / f'{name}.toke'; b = root / f'{name}-bin'; s.write_text(src + '\n')
    cmd = [TKC] + (['--legacy'] if legacy else []) + [str(s), '--out', str(b)]
    rc, out, err = run(cmd, 240)
    if rc != 0 or not b.exists():
        return 'compile', None
    rc, out, err = run([str(b), json.dumps(arg, separators=(',', ':'))], 30)
    if rc != 0:
        return 'runtime', None
    try:
        return 'ran', json.loads(out.strip().splitlines()[-1])
    except Exception:
        return 'invalid_json', None


programs = build_python_programs(EVAL_851 / 'benchmark/baselines/python/solutions.py')
assert len(programs) == 60, len(programs)
rows = []
tmp = Path(tempfile.mkdtemp(prefix='repro133-'))
for tid, py in sorted(programs.items()):
    r = {'task_id': tid}
    kern = transpile(py, compact=True)
    mini = python_minifier.minify(py, rename_globals=False)
    t851 = (EVAL_851 / f'benchmark/solutions/{tid}.toke').read_text().strip()
    thead = (EVAL_HEAD / f'benchmark/solutions/{tid}.toke').read_text().strip()
    f851 = tmp / f'{tid}.851.tk'; f851.write_text(t851 + '\n')
    # (1) as KERN measured: v0.3 source, --legacy --check
    rc, o, e = run([TKC, '--legacy', '--check', str(f851)])
    r['v03_legacy_check_ok'] = rc == 0; r['v03_first_error'] = first_err(o + e)
    # (2) tkc --migrate -> --check -> --min
    rc, mig, e = run([TKC, '--migrate', str(f851)])
    r['migrate_ok'] = rc == 0 and bool(mig.strip())
    fmig = tmp / f'{tid}.mig.tk'; fmig.write_text(mig if r['migrate_ok'] else t851 + '\n')
    rc, o, e = run([TKC, '--check', str(fmig)])
    r['mig_check_ok'] = rc == 0; r['mig_first_error'] = first_err(o + e)
    rc, mn, e = run([TKC, '--min', str(fmig)])
    mig_min = mn.strip() if rc == 0 and mn.strip() else mig.strip()
    r['mig_min_ok'] = rc == 0
    # (3) toke-eval HEAD (130.2 migrated) solution -> --check -> --min
    fhead = tmp / f'{tid}.head.tk'; fhead.write_text(thead + '\n')
    rc, o, e = run([TKC, '--check', str(fhead)])
    r['head_check_ok'] = rc == 0; r['head_first_error'] = first_err(o + e)
    rc, mn, e = run([TKC, '--min', str(fhead)])
    head_min = mn.strip() if rc == 0 and mn.strip() else thead
    r['head_min_ok'] = rc == 0
    # tokens
    for name, text in [('python', py), ('kern_compact', kern), ('python_minifier', mini),
                       ('toke_v03', t851), ('toke_migrated', mig.strip() if r['migrate_ok'] else t851),
                       ('toke_migrated_min', mig_min), ('toke_head', thead), ('toke_head_min', head_min)]:
        c = counts(text); r[f'{name}_cl100k'] = c['cl100k_base']; r[f'{name}_o200k'] = c['o200k_base']
        r[f'{name}_chars'] = len(text)
    r['toke_v03_native16k'] = toke_native_count(t851)
    r['toke_migrated_min_native16k'] = toke_native_count(mig_min)
    r['toke_head_min_native16k'] = toke_native_count(head_min)
    r['kern_compact_kern16k'] = len(kern16k.encode(kern).ids)
    # probes (their inputs, their oracle = the Python JSON-CLI program)
    arg = PROBE_INPUTS[tid]
    ok, oracle = probe_py(py, arg, tmp, f'{tid}-py')
    r['python_probe_ok'] = ok
    ok2, kv = probe_py(__import__('kern_compiler').compile_kern(kern), arg, tmp, f'{tid}-kern')
    r['kern_probe_ok'] = ok and ok2 and kv == oracle
    st, v = probe_toke(t851, arg, tmp, f'{tid}-v03', True)
    r['toke_v03_probe'] = st if not (ok and st == 'ran' and v == oracle) else 'pass'
    st, v = probe_toke(mig_min if r['mig_check_ok'] else (mig.strip() if r['migrate_ok'] else t851), arg, tmp, f'{tid}-mig', False)
    r['toke_migrated_probe'] = st if not (ok and st == 'ran' and v == oracle) else 'pass'
    st, v = probe_toke(head_min, arg, tmp, f'{tid}-head', False)
    r['toke_head_probe'] = st if not (ok and st == 'ran' and v == oracle) else 'pass'
    rows.append(r)
    print(tid, 'v03chk', r['v03_legacy_check_ok'], r['v03_first_error'], '| mig', r['migrate_ok'], r['mig_check_ok'],
          r['mig_first_error'], '| head', r['head_check_ok'], r['head_first_error'],
          '| cl100k py/kern/v03/mig_min/head_min', r['python_cl100k'], r['kern_compact_cl100k'], r['toke_v03_cl100k'],
          r['toke_migrated_min_cl100k'], r['toke_head_min_cl100k'], '| probes', r['toke_v03_probe'], r['toke_migrated_probe'], r['toke_head_probe'], flush=True)

# --- aggregates + bootstrap CIs on sum-ratios
keys = [k for k in rows[0] if k.endswith(('_cl100k', '_o200k', '_native16k', '_kern16k', '_chars'))]
tot = {k: sum(r[k] for r in rows) for k in keys}
rng = random.Random(0)
N = len(rows)


def boot_ratio(num, den, reps=10000):
    pt = tot[num] / tot[den]
    vals = []
    for _ in range(reps):
        idx = [rng.randrange(N) for _ in range(N)]
        a = sum(rows[i][num] for i in idx); b = sum(rows[i][den] for i in idx)
        vals.append(a / b)
    vals.sort()
    return {'point': pt, 'ci95': [vals[int(0.025 * reps)], vals[int(0.975 * reps) - 1]]}


ratios = {}
for num, den in [('kern_compact_cl100k', 'toke_v03_cl100k'), ('kern_compact_cl100k', 'toke_migrated_min_cl100k'),
                 ('kern_compact_cl100k', 'toke_head_min_cl100k'), ('python_minifier_cl100k', 'toke_migrated_min_cl100k'),
                 ('toke_migrated_min_cl100k', 'python_cl100k'), ('toke_head_min_cl100k', 'python_cl100k'),
                 ('toke_v03_cl100k', 'python_cl100k'),
                 ('kern_compact_o200k', 'toke_migrated_min_o200k'), ('toke_migrated_min_o200k', 'python_o200k'),
                 ('kern_compact_kern16k', 'toke_v03_native16k'), ('kern_compact_kern16k', 'toke_migrated_min_native16k'),
                 ('kern_compact_kern16k', 'toke_head_min_native16k'),
                 ('toke_migrated_min_native16k', 'python_cl100k'), ('toke_migrated_min_native16k', 'toke_migrated_min_cl100k')]:
    ratios[f'{num} / {den}'] = boot_ratio(num, den)

summary = {
    'generated_at': __import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat(),
    'tkc_version': run([TKC, '--version'])[1].strip(),
    'toke_eval_v03_commit': '851f6d8b2cfedea22833f3787ad96c19e072e952',
    'toke_eval_head_commit': run(['git', '-C', str(EVAL_HEAD), 'rev-parse', 'HEAD'])[1].strip(),
    'kern_commit': run(['git', '-C', '/tmp/kern-oscar', 'rev-parse', 'HEAD'])[1].strip(),
    'python_minifier': '3.2.0', 'toke_tokenizer': '0.1.0',
    'tiktoken': tiktoken.__version__,
    'pairs': N,
    'totals': tot,
    'gates': {
        'v03_legacy_check_ok': sum(r['v03_legacy_check_ok'] for r in rows),
        'v03_first_error_counts': dict(__import__('collections').Counter(r['v03_first_error'] for r in rows if not r['v03_legacy_check_ok'])),
        'migrate_ok': sum(r['migrate_ok'] for r in rows),
        'mig_check_ok': sum(r['mig_check_ok'] for r in rows),
        'mig_first_error_counts': dict(__import__('collections').Counter(r['mig_first_error'] for r in rows if not r['mig_check_ok'])),
        'head_check_ok': sum(r['head_check_ok'] for r in rows),
        'head_first_error_counts': dict(__import__('collections').Counter(r['head_first_error'] for r in rows if not r['head_check_ok'])),
    },
    'probes': {
        'python': sum(r['python_probe_ok'] for r in rows),
        'kern_compact': sum(r['kern_probe_ok'] for r in rows),
        'toke_v03_legacy': sum(r['toke_v03_probe'] == 'pass' for r in rows),
        'toke_migrated': sum(r['toke_migrated_probe'] == 'pass' for r in rows),
        'toke_migrated_stages': dict(__import__('collections').Counter(r['toke_migrated_probe'] for r in rows)),
        'toke_head': sum(r['toke_head_probe'] == 'pass' for r in rows),
        'toke_head_stages': dict(__import__('collections').Counter(r['toke_head_probe'] for r in rows)),
    },
    'sum_ratios_bootstrap95': ratios,
    'per_task': rows,
}
OUT.write_text(json.dumps(summary, indent=1))
print(json.dumps({k: v for k, v in summary.items() if k != 'per_task'}, indent=1))
