#!/usr/bin/env python3
"""Compare two guest-state hash logs (GCN_STATE_HASH=<every>:<file>, docs/diagnostics.md).

    python tools/state_hash_diff.py a.txt b.txt

Lines are matched by frame. Prints how many frames both logs have, the first frame where
they part and what parted there (the virtual time, the poll count, RAM, ARAM, the locked
cache, and which 1 MB blocks of RAM and ARAM), then how many of the frames after it differ.
Exits 0 when every shared frame matches, 1 when one differs, 2 on a bad log.
"""
import sys

RAM_BASE = 0x80000000
BLOCK = 0x100000


def load(path):
    rows = {}
    with open(path) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            head, ram_blocks, aram_blocks = line.split('|')
            frame, poll, tick, all_, ram, aram, lc = head.split()
            rows[int(frame)] = {
                'tick': int(tick), 'poll': int(poll), 'all': all_, 'ram': ram, 'aram': aram, 'lc': lc,
                'ram_blocks': ram_blocks.split(), 'aram_blocks': aram_blocks.split(),
            }
    return rows


def blocks(a, b):
    return [i for i, (x, y) in enumerate(zip(a, b)) if x != y]


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip())
        return 2
    try:
        a, b = load(sys.argv[1]), load(sys.argv[2])
    except (OSError, ValueError) as e:
        print(f'cannot read the logs: {e}')
        return 2
    shared = sorted(set(a) & set(b))
    if not shared:
        print('no frame in common')
        return 2
    print(f'{len(shared)} frames in common, {shared[0]} to {shared[-1]} '
          f'({len(a)} in {sys.argv[1]}, {len(b)} in {sys.argv[2]})')
    first = next((f for f in shared if a[f]['all'] != b[f]['all'] or a[f]['poll'] != b[f]['poll']), None)
    if first is None:
        print('SAME: every frame in common has the same state')
        return 0
    x, y = a[first], b[first]
    last_same = max((f for f in shared if f < first), default=None)
    print(f'DIFFERENT from frame {first}' + (f' (the same at {last_same})' if last_same is not None else ''))
    for k in ('tick', 'poll', 'ram', 'aram', 'lc'):
        if x[k] != y[k]:
            print(f'  {k}: {x[k]} vs {y[k]}')
    for name, key, base in (('ram', 'ram_blocks', RAM_BASE), ('aram', 'aram_blocks', 0)):
        diff = blocks(x[key], y[key])
        if diff:
            spans = ', '.join(f'{base + i * BLOCK:08X}-{base + (i + 1) * BLOCK:08X}' for i in diff)
            print(f'  {name} blocks: {spans}')
    after = [f for f in shared if f >= first]
    n = sum(1 for f in after if a[f]['all'] != b[f]['all'])
    print(f'  {n} of the {len(after)} frames from there differ')
    return 1


if __name__ == '__main__':
    sys.exit(main())
