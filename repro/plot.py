import csv
import os
import statistics
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

RED, RED_L, RED_M = '#D85A4D', '#F2BFB7', '#D17870'
BLUE, GREEN, VIOLET, GRAY, EDGE = '#3D6FA0', '#4A9D7A', '#8E72B5', '#8C8C8C', '#555555'

plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Arial', 'Helvetica', 'DejaVu Sans'],
    'font.size': 13,
    'axes.labelsize': 13,
    'xtick.labelsize': 12,
    'ytick.labelsize': 12,
    'legend.fontsize': 12,
    'legend.frameon': False,
    'axes.linewidth': 1.1,
    'lines.linewidth': 2.0,
    'lines.markersize': 8,
    'savefig.bbox': 'tight',
    'savefig.dpi': 200,
})

ARMS9 = ['tessera', 'tessera_h0', 'spalm', 'duckdb132', 'duckdb156']
LABEL = {
    'tessera': 'Tessera',
    'tessera_h0': 'Tessera, no matrix accelerator',
    'spalm': 'SPALM',
    'duckdb132': 'DuckDB v1.3.2',
    'duckdb156': 'DuckDB 1.5.6',
    'apriori132': 'Apriori SQL (v1.3.2)',
    'apriori156': 'Apriori SQL (1.5.6)',
}
COLOR = {'tessera': RED, 'tessera_h0': RED_L, 'spalm': GREEN, 'duckdb132': BLUE, 'duckdb156': VIOLET,
         'apriori132': BLUE, 'apriori156': VIOLET}
GROUPS9 = ['NYTimes', 'MovieLens', 'kosarak', 'human_gene1', 'NYTimes_8K', 'MovieLens_8K', 'PubMed_8K',
           'LiveJournal_8K', 'Orkut_16K']
TITLE10 = {'nytimes': 'NYTimes', 'ml32m': 'MovieLens', 'kosarak': 'kosarak'}
MARK10 = {'nytimes': '^', 'ml32m': 's', 'kosarak': 'o'}


def read(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f, delimiter='\t'))


def plain(v, _pos=None):
    return f'{int(round(v)):,}' if v >= 1 else f'{v:.10f}'.rstrip('0').rstrip('.')


def style(ax):
    for side in ('top', 'right'):
        ax.spines[side].set_visible(False)
    ax.tick_params(direction='out', length=4, top=False, right=False)
    ax.grid(True, alpha=0.3, linewidth=0.5)
    ax.set_axisbelow(True)
    for axis in (ax.xaxis, ax.yaxis):
        if axis.get_scale() == 'log':
            lo, hi = sorted(axis.get_view_interval())
            if sum(lo <= v <= hi for v in axis.get_ticklocs()) < 2:
                axis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
            axis.set_major_formatter(FuncFormatter(plain))
            axis.set_minor_formatter(NullFormatter())


def save(fig, figdir, name):
    for ext in ('pdf', 'png'):
        fig.savefig(os.path.join(figdir, f'{name}.{ext}'))
    plt.close(fig)


def figure9(t, figdir):
    groups = [g for g in GROUPS9 if any(('9', g, a) in t for a in ARMS9)]
    if not groups:
        return
    arms = [a for a in ARMS9 if any(('9', g, a) in t for g in groups)]
    fig, ax = plt.subplots(figsize=(13.0, 3.6))
    bw = 0.8 / len(arms)
    ymin = 0.1
    for i, a in enumerate(arms):
        for x, g in enumerate(groups):
            off = x - 0.4 + bw * (i + 0.5)
            r = t.get(('9', g, a))
            if not r or r['status'] != 'ok':
                ax.text(off, ymin * 1.25, r['status'] if r else 'n/a', rotation=90, ha='center', va='bottom',
                        fontsize=10, color=GRAY)
                continue
            v = float(r['seconds'])
            ax.bar(off, v - ymin, bw, bottom=ymin, color=COLOR[a], hatch='//' if a == 'tessera_h0' else None,
                   edgecolor=EDGE, linewidth=0.6)
    ax.set_yscale('log')
    ax.set_ylim(ymin, 4e4)
    for x, g in enumerate(groups):
        r = t.get(('9', g, 'tessera'))
        base = [float(t[('9', g, a)]['seconds']) for a in ('spalm', 'duckdb132', 'duckdb156')
                if ('9', g, a) in t and t[('9', g, a)]['status'] == 'ok']
        if r and r['status'] == 'ok' and base:
            v = float(r['seconds'])
            top = max(v, float(t[('9', g, 'tessera_h0')]['seconds'])) if ('9', g, 'tessera_h0') in t and \
                t[('9', g, 'tessera_h0')]['status'] == 'ok' else v
            ax.text(x - 0.4 + bw * 0.5, top * 1.25, f'{min(base) / v:.1f}×', rotation=90, ha='center',
                    va='bottom', fontsize=11, color=RED)
    nfull = sum(1 for g in groups if not g.endswith('K'))
    if 0 < nfull < len(groups):
        ax.axvline(nfull - 0.5, ymax=0.97, color=GRAY, linestyle='--', linewidth=1.0)
        ax.text((nfull - 1) / 2, 1.6e4, 'Full products', ha='center', va='center')
        ax.text((nfull + len(groups) - 1) / 2, 1.6e4, 'Blocks of the most frequent values', ha='center',
                va='center')
    ax.set_xlim(-0.55, len(groups) - 0.45)
    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels([g.replace('_8K', '\ntop 8K').replace('_16K', '\ntop 16K') for g in groups])
    ax.set_ylabel('Time (s)')
    style(ax)
    ax.grid(False, axis='x')
    ax.legend(handles=[Patch(facecolor=COLOR[a], edgecolor=EDGE, hatch='//' if a == 'tessera_h0' else None,
                             label=LABEL[a]) for a in arms],
              loc='lower center', bbox_to_anchor=(0.5, 1.02), ncol=len(arms))
    save(fig, figdir, 'fig9')


def figure10(t, keys, figdir):
    datasets = [d for d in ('nytimes', 'ml32m', 'kosarak') if d in keys]
    if not datasets:
        return

    def series(ds, kind, arm):
        out = []
        for (f, q, a), r in t.items():
            if f == '10' and a == arm and r['status'] == 'ok' and q.startswith(f'{ds}_{kind}_'):
                p = int(q.rsplit('_', 1)[1])
                x = 100.0 * p / keys[ds] if kind == 'having' else p
                out.append((x, float(r['seconds'])))
        return sorted(out)

    fig, axes = plt.subplots(1, len(datasets) + 1, figsize=(4.0 * (len(datasets) + 1), 3.4))
    for ax, ds in zip(axes, datasets):
        s = series(ds, 'having', 'tessera')
        if not s:
            continue
        xs = [x for x, _ in s]
        lo, hi = min(xs) / 1.25, max(xs) * 1.25
        for a in ('duckdb132', 'duckdb156'):
            e = series(ds, 'having', a)
            if e:
                v = statistics.median(y for _, y in e)
                ax.plot([lo, hi], [v, v], color=COLOR[a], linestyle='--')
        for a in ('apriori132', 'apriori156'):
            e = series(ds, 'having', a)
            if e:
                ax.plot([x for x, _ in e], [y for _, y in e], color=COLOR[a], linestyle=':', marker='o' if
                        a == 'apriori132' else 'D')
        ax.plot(xs, [y for _, y in s], color=RED, marker='^', zorder=5)
        h0 = series(ds, 'having', 'tessera_h0')
        if h0 and ds != 'kosarak':
            ax.plot([x * 1.2 if x == max(xs) else x / 1.2 for x, _ in h0], [y for _, y in h0], linestyle='none',
                    marker='v', color=RED_M, markerfacecolor='white', markeredgewidth=1.8, zorder=6)
        ax.set_xscale('log')
        ax.set_yscale('log')
        ax.set_xlim(max(xs) * 1.3, min(xs) / 1.3)
        ax.set_xticks([1, 0.5, 0.2, 0.1])
        ax.minorticks_off()
        ax.set_xlabel('Support (%)')
        ax.set_ylabel('Time (s)')
        ax.set_title(f'{TITLE10[ds]}, HAVING', fontsize=13)
        style(ax)
    ax = axes[-1]
    for ds in datasets:
        s = series(ds, 'topk', 'tessera')
        if not s:
            continue
        xs = [x for x, _ in s]
        e = series(ds, 'topk', 'duckdb132')
        if e:
            v = statistics.median(y for _, y in e)
            ax.plot([min(xs), max(xs)], [v, v], color=BLUE, linestyle='--')
        ax.plot(xs, [y for _, y in s], color=RED, marker=MARK10[ds], zorder=5)
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlabel('Top-$k$ ($k$)')
    ax.set_ylabel('Time (s)')
    ax.set_title('Top-$k$', fontsize=13)
    style(ax)
    handles = [Line2D([0], [0], color=RED, marker='^', label='Tessera'),
               Line2D([0], [0], linestyle='none', marker='v', color=RED_M, markerfacecolor='white',
                      markeredgewidth=1.8, label='Tessera, no matrix accelerator'),
               Line2D([0], [0], color=BLUE, linestyle='--', label=LABEL['duckdb132']),
               Line2D([0], [0], color=VIOLET, linestyle='--', label=LABEL['duckdb156']),
               Line2D([0], [0], color=BLUE, linestyle=':', marker='o', label=LABEL['apriori132']),
               Line2D([0], [0], color=VIOLET, linestyle=':', marker='D', label=LABEL['apriori156'])]
    handles += [Line2D([0], [0], linestyle='none', marker=MARK10[d], color='#333333', label=TITLE10[d])
                for d in datasets]
    fig.legend(handles=handles, loc='lower center', bbox_to_anchor=(0.5, 1.0), ncol=5)
    fig.tight_layout()
    save(fig, figdir, 'fig10')


def check(rows):
    seen = {}
    for r in rows:
        if r['status'] == 'ok':
            seen.setdefault((r['figure'], r['query']), {})[r['arm']] = (r['cells'], r['total'])
    bad = [(k, v) for k, v in sorted(seen.items()) if len(set(v.values())) > 1]
    if bad:
        print('Results that differ between systems:')
        for (f, q), v in bad:
            print(f'  Figure {f} {q}: ' + ', '.join(f'{a} {c} cells, total {s}' for a, (c, s) in sorted(v.items())))
    else:
        print(f'All systems return the same result on each of the {len(seen)} queries.')


def compare(t, paper, out):
    lines = [('figure', 'query', 'arm', 'this_run_s', 'paper_s', 'ratio', 'paper_point')]
    for p in paper:
        r = t.get((p['figure'], p['query'], p['arm']))
        if not r or r['status'] != 'ok':
            continue
        mine, theirs = float(r['seconds']), float(p['seconds'])
        lines.append((p['figure'], p['query'], p['arm'], f'{mine:.3f}', f'{theirs:.3f}', f'{mine / theirs:.2f}',
                      p['paper_point']))
    with open(os.path.join(out, 'compare.tsv'), 'w') as f:
        for line in lines:
            f.write('\t'.join(line) + '\n')
    if len(lines) > 1:
        print(f'\n{"Figure":<7}{"Query":<22}{"System":<12}{"This run (s)":>13}{"Paper (s)":>11}{"Ratio":>7}')
        for f, q, a, mine, theirs, ratio, _ in lines[1:]:
            print(f'{f:<7}{q:<22}{a:<12}{mine:>13}{theirs:>11}{ratio:>7}')


def main():
    out, figdir = sys.argv[1], sys.argv[2]
    rows = read(os.path.join(out, 'times.tsv'))
    t = {(r['figure'], r['query'], r['arm']): r for r in rows}
    keys = {r[0]: int(r[1]) for r in csv.reader(open(os.path.join(out, 'keys.tsv')), delimiter='\t')} \
        if os.path.exists(os.path.join(out, 'keys.tsv')) else {}
    os.makedirs(figdir, exist_ok=True)
    figure9(t, figdir)
    figure10(t, keys, figdir)
    check(rows)
    compare(t, read(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'paper.tsv')), out)
    print(f'\nFigures: {figdir}')


main()
