#!/usr/bin/env python3
"""Fit the constants of Tessera's frontier cost model on this machine.

The cost model picks the frontier, the number of most frequent values whose pairs run on the matrix accelerator, by
estimating the time of every candidate. Its constants are seconds per unit of work, measured on one CPU. This script
runs COUNT(*) self-joins of the given relations with the frontier forced to 0 and to each candidate, fits every
constant to the measured time of the part of the run that it charges, with the counts of work that the model itself
estimates, and then checks the frontier that the fitted model picks against the fastest forced one. Results never
depend on the constants, only the speed does.

Run it with the thread count and on the cores that the database engine uses, on relations like the ones it serves.
"""
import argparse
import math
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = ['tdpbuud', 'block_pair', 'sparse_product', 'hl_row_group', 'll_product', 'grid_row', 'grid_nnz']
UNITS = {
    'tdpbuud': 'tile product',
    'block_pair': 'pair of 32-value blocks',
    'sparse_product': 'product of the sparse path without a frontier',
    'hl_row_group': 'heavy-light row x 64 frontier values',
    'll_product': 'product of two rarer values',
    'grid_row': 'row with a frontier value',
    'grid_nnz': 'frontier nonzero',
}
STAT = re.compile(
    r'tessera: n \d+ rows \d+ items (\d+) H (\d+) \| build ([\d.]+) s, order ([\d.]+), panels ([\d.]+), '
    r'dense ([\d.]+), sparse ([\d.]+), total ([\d.]+) \| tdpbuud (\d+) panels \d+ block_pairs (\d+) '
    r'sparse_products (\d+) cells\(i<=j\) \d+ \| colidx ([\d.]+) hl ([\d.]+) hl_tdpbuud \d+ grid_rows (\d+) '
    r'hl_rows (\d+) hl_build ([\d.]+)')
STAT_KEYS = ['items', 'H', 'build', 'order', 'panels', 'dense', 'sparse', 'total', 'tdpbuud', 'block_pairs',
             'sparse_products', 'colidx', 'hl', 'grid_rows', 'hl_rows', 'hl_build']
MODEL = re.compile(r'H (\d+) tdpbuud (\S+) heavy (\S+) hl_rows (\S+) ll (\S+) grid_rows (\S+) grid_nnz (\S+) '
                   r'model (\S+) s')
MIN_T = 0.02
MARK = re.compile(r'calibrate: (done )?frontier (-?\d+) rep (\d+)(?: seconds ([\d.]+))?')


def defaults():
    text = open(os.path.join(HERE, '..', 'core', 'tessera.hpp')).read()
    body = text[text.index('struct CostModel'):]
    body = body[:body.index('};')]
    found = dict(re.findall(r'double (\w+) = ([0-9.eE+-]+);', body))
    return {n: float(found[n]) for n in NAMES}


def run(args, path, frontiers, env=None, quiet=False):
    cmd = [args.bin, '-t', str(args.threads), '-r', str(args.reps), '-k', str(args.key_col), '-x',
           str(args.value_col)]
    if args.memory:
        cmd += ['-m', str(args.memory), '-d', args.spill_dir]
    cmd += [path] + [str(h) for h in frontiers]
    p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env)
    if p.returncode != 0:
        sys.exit('calibrate: %s failed:\n%s' % (' '.join(cmd), p.stderr[-3000:]))
    runs, cur = {}, None
    for line in p.stderr.splitlines():
        m = MARK.match(line)
        if m:
            cur = (int(m.group(2)), int(m.group(3)))
            r = runs.setdefault(cur, {'log': {}})
            if m.group(1):
                r['seconds'] = float(m.group(4))
            continue
        m = MODEL.match(line)
        if m and cur:
            runs[cur]['log'][int(m.group(1))] = [float(v) for v in m.groups()[1:]]
            continue
        m = STAT.search(line)
        if m and cur:
            runs[cur].update({k: float(v) for k, v in zip(STAT_KEYS, m.groups())})
        elif line.startswith('calibrate: ') and 'loaded' in line and not quiet:
            print('  ' + line[len('calibrate: '):])
        elif 'TESSERA_COST_MODEL' in line:
            print('  ' + line)
    out = {}
    for h in frontiers:
        reps = [runs[(h, r)] for r in range(1, args.reps + 1)]
        if any('total' not in r for r in reps):
            sys.exit('calibrate: no report for frontier %d; is the library built with this tools/Makefile?' % h)
        med = {k: statistics.median(r[k] for r in reps) for k in STAT_KEYS + ['seconds']}
        med['log'] = runs[(h, 1)]['log']
        out[h] = med
    return out


def estimate(c, H, row):
    tdp, heavy, lr, ll, gr, gz, _ = row
    nb = H / 32
    t = c['tdpbuud'] * tdp + c['block_pair'] * nb * (nb + 1) / 2
    if H > 0:
        t += c['grid_row'] * gr + c['grid_nnz'] * gz
        t += c['hl_row_group'] * lr * ((H + 63) // 64) + c['ll_product'] * ll
    else:
        t += c['sparse_product'] * (ll - heavy)
    return t


def pick(c, log):
    best = None
    for H in sorted(log):
        t = estimate(c, H, log[H])
        if best is None or t < best[1]:
            best = (H, t)
    return best[0]


def fit(samples):
    if not samples:
        return None, None
    k = len(samples[0][0])
    rows = [[x / m for x in xs] for xs, m in samples]
    if k == 1:
        a = [r[0] for r in rows]
        c = [sum(a) / sum(v * v for v in a)]
    else:
        g = [[sum(r[i] * r[j] for r in rows) for j in range(k)] for i in range(k)]
        b = [sum(r[i] for r in rows) for i in range(k)]
        det = g[0][0] * g[1][1] - g[0][1] * g[1][0]
        if abs(det) < 1e-30 * max(1.0, abs(g[0][0] * g[1][1])):
            return None, None
        c = [(b[0] * g[1][1] - b[1] * g[0][1]) / det, (b[1] * g[0][0] - b[0] * g[1][0]) / det]
    err = [sum(ci * xi for ci, xi in zip(c, r)) - 1 for r in rows]
    return c, math.sqrt(sum(e * e for e in err) / len(err))


def fit_pair(samples, d0, d1):
    c, e = fit(samples)
    if c and c[0] > 0 and c[1] > 0:
        return c, e, ''
    one, e = fit([([xs[0] + xs[1] * d1 / d0], m) for xs, m in samples])
    if not one:
        return None, None, ''
    return [one[0], one[0] * d1 / d0], e, ' (second constant scaled with the first)'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+', help='relations as text, one row per line (CSV, TSV, or space separated)')
    ap.add_argument('--key-col', type=int, default=0, help='column of the join key, from 0 (default 0)')
    ap.add_argument('--value-col', type=int, default=1, help='column of the grouped value, from 0 (default 1)')
    ap.add_argument('--threads', type=int, default=0, help='threads per query, as the engine runs them (default: '
                    'all cores)')
    ap.add_argument('--reps', type=int, default=3, help='timed runs per frontier after one untimed run (default 3)')
    ap.add_argument('--max-frontier', type=int, default=16384, help='largest forced frontier (default 16384)')
    ap.add_argument('--memory', type=int, default=0, help='bytes of output kept in memory before spilling to '
                    '--spill-dir (default: no limit)')
    ap.add_argument('--spill-dir', default='/tmp')
    ap.add_argument('--bin', default=os.path.join(HERE, 'build', 'calibrate_run'))
    ap.add_argument('--out', help='also write the export line to this file')
    args = ap.parse_args()
    if not os.path.exists(args.bin):
        sys.exit('calibrate: build the driver first: make -C %s' % HERE)
    base = defaults()
    env = {k: v for k, v in os.environ.items() if k != 'TESSERA_COST_MODEL'}
    data = {}
    for path in args.files:
        print('%s:' % path)
        auto = run(args, path, [-1], env)[-1]
        log = auto['log']
        if not log:
            sys.exit('calibrate: the library reported no cost model; it needs the AMX variant (TESSERA_ISA unset)')
        for H in sorted(log):
            got, want = estimate(base, H, log[H]), log[H][-1]
            if abs(got - want) > 0.03 * want + 1e-3:
                sys.exit('calibrate: the cost model of this library differs from this script (H %d: %.3f s vs %.3f s)'
                         % (H, got, want))
        forced = [H for H in sorted(log) if H <= args.max_frontier]
        res = run(args, path, forced, env, quiet=True)
        data[path] = (auto, res)
        print('  ' + ', '.join('H %d: %.2f s' % (H, res[H]['total']) for H in forced))
        print('  the current model picks H %d: %.2f s' % (auto['H'], auto['total']))
        if auto['total'] < 0.2:
            print('  warning: runs this short are dominated by timer resolution; use a larger relation')
    s = {n: [] for n in ['sparse', 'll', 'dense', 'hl', 'setup']}
    for path, (auto, res) in data.items():
        log = auto['log']
        for H, r in res.items():
            tdp, heavy, lr, ll, gr, gz, _ = log[H]
            parts = {'light': r['sparse'] + r['colidx'], 'dense': r['dense'], 'hl': r['hl'] + r['hl_build'],
                     'setup': r['order'] + r['panels']}
            if H == 0:
                if parts['light'] >= MIN_T:
                    s['sparse'].append(([ll - heavy], parts['light']))
                continue
            nb = H / 32
            if parts['light'] >= MIN_T and ll > 0:
                s['ll'].append(([ll], parts['light']))
            if parts['dense'] >= MIN_T and tdp > 0:
                s['dense'].append(([tdp, nb * (nb + 1) / 2], parts['dense']))
            if parts['hl'] >= MIN_T and lr > 0:
                s['hl'].append(([lr * ((H + 63) // 64)], parts['hl']))
            if parts['setup'] >= MIN_T and gr > 0:
                s['setup'].append(([gr, gz], parts['setup']))
    new, note = dict(base), {}

    def take(names, c, e, n, extra=''):
        if not c:
            for name in names:
                note[name] = 'kept: no run exercised it'
            return
        for name, v in zip(names, c):
            new[name] = v
            note[name] = '%d runs, rms error %.0f%%%s' % (n, 100 * e, extra)

    c, e = fit(s['sparse'])
    take(['sparse_product'], c, e, len(s['sparse']))
    c, e = fit(s['ll'])
    take(['ll_product'], c, e, len(s['ll']))
    c, e = fit(s['hl'])
    take(['hl_row_group'], c, e, len(s['hl']))
    c, e, x = fit_pair(s['dense'], base['tdpbuud'], base['block_pair'])
    take(['tdpbuud', 'block_pair'], c, e, len(s['dense']), x)
    c, e, x = fit_pair(s['setup'], base['grid_row'], base['grid_nnz'])
    take(['grid_row', 'grid_nnz'], c, e, len(s['setup']), x)
    print('\nconstants (seconds per unit, all threads together; the error is that of the part of the run that each one\n'
          'charges, which the model splits only coarsely, so the check below is what counts):')
    for n in NAMES:
        print('  %-15s %.3e -> %.3e  per %s; %s' % (n, base[n], new[n], UNITS[n], note[n]))
    line = 'export TESSERA_COST_MODEL="%s"' % ','.join('%s=%.4g' % (n, new[n]) for n in NAMES)
    venv = dict(env, TESSERA_COST_MODEL=line.split('"')[1])
    print('\ncheck: time at the frontier that each set of constants picks, against the fastest forced frontier')
    loss = {'default': [], 'fitted': []}
    for path, (auto, res) in data.items():
        best = min(res, key=lambda h: res[h]['total'])
        chk = run(args, path, [-1], venv, quiet=True)[-1]
        if chk['H'] != pick(new, auto['log']):
            print('  warning: the library picked H %d, the script expected H %d' % (chk['H'], pick(new, auto['log'])))
        parts = []
        for name, r in (('default', auto), ('fitted', chk)):
            t = res[r['H']]['total'] if r['H'] in res else r['total']
            loss[name].append(t / res[best]['total'] - 1)
            parts.append('%s H %d (%+.0f%%)' % (name, r['H'], 100 * loss[name][-1]))
        print('  %s: fastest H %d (%.2f s); %s' % (path, best, res[best]['total'], '; '.join(parts)))
    d, f = statistics.mean(loss['default']), statistics.mean(loss['fitted'])
    if f < d - 0.01:
        print('  the fitted constants pick faster frontiers than the defaults on these relations')
    elif f > d + 0.01:
        print('  the fitted constants pick slower frontiers than the defaults on these relations; keep the defaults,\n'
              '  or calibrate on more relations like the ones the engine serves')
    else:
        print('  the fitted constants and the defaults pick equally fast frontiers on these relations')
    print('\nTo use the fitted constants, set this in the environment of the process that runs Tessera:\n' + line)
    if args.out:
        with open(args.out, 'w') as f:
            f.write(line + '\n')


if __name__ == '__main__':
    main()
