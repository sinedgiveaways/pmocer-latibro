#!/usr/bin/env python3
"""Summarize Orbitals Together thread state around an Unreal RenderThread timeout.

This is a read-only, standard-library analyzer for orbitals.log. It prints a compact,
shareable text report; it never uploads or modifies the input log.

Usage:
  python tools/run/analyze_timeout.py path/to/orbitals.log
  python tools/run/analyze_timeout.py path/to/orbitals.log --all-threads
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

THREAD_RE = re.compile(
    r'^\s*thread\s+(?P<handle>0x[0-9a-fA-F]+)\s+"(?P<name>[^"]*)"\s+'
    r'prio\s+(?P<prio>-?\d+)\s+core\s+(?P<core>-?\d+)'
    r'(?P<flags>[^:]*):\s*(?P<state>.*)$'
)
FRAME_RE = re.compile(r'^\s*(?:guest )?frame\s+\d+:\s*(.*)$')
TEXT_RE = re.compile(r'^\s*sp\+(?:0x[0-9a-fA-F]+)\s+"(.*)"\s*$')
FATAL_RE = re.compile(r'^\s*FATAL: svc #0x26 Break at\b')


def clean_powershell_prefix(line):
    """Remove Select-String's optional path:line: prefix, if analyzing pasted output."""
    return re.sub(r'^.*?orbitals(?:\.\d+)?\.log:\d+:', '', line, count=1)


def find_fatal_region(lines):
    starts = [i for i, line in enumerate(lines) if FATAL_RE.search(line)]
    if not starts:
        return None
    start = starts[-1]
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if 'FATAL: svc #0x26 Break is not implemented' in lines[i]:
            end = i + 1
            break
    return start, end


def parse_threads(lines):
    threads = []
    current = None
    in_dump = False
    for line in lines:
        if 'every guest thread' in line:
            in_dump = True
            continue
        if not in_dump:
            continue
        match = THREAD_RE.match(line)
        if match:
            current = match.groupdict()
            current['frames'] = []
            current['prio'] = int(current['prio'])
            current['core'] = int(current['core'])
            threads.append(current)
            continue
        frame = FRAME_RE.match(line)
        if frame and current is not None:
            current['frames'].append(frame.group(1).strip())
            continue
        # Dump ended at its runtime summary or the generic fatal tail.
        if line.startswith('  run time at the abort') or 'FATAL: svc #0x26 Break is not implemented' in line:
            break
    return threads


def classify_state(text):
    for phrase in ('waiting on a condition variable', 'waiting on a mutex',
                   'waiting on an address', 'waiting on handles'):
        if phrase in text:
            return phrase
    if 'running or ready' in text:
        return 'running or ready (snapshot label)'
    return text or 'unknown'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log', type=Path, help='path to orbitals.log (or a pasted text excerpt)')
    ap.add_argument('--all-threads', action='store_true', help='include every thread instead of key/render threads')
    args = ap.parse_args()

    try:
        raw_lines = args.log.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError as exc:
        ap.error(str(exc))
    lines = [clean_powershell_prefix(line) for line in raw_lines]
    region = find_fatal_region(lines)
    if region:
        lo, hi = region
        fatal_lines = lines[lo:hi]
    else:
        fatal_lines = lines

    markers = [line.strip() for line in fatal_lines if FATAL_RE.search(line)]
    if not markers:
        # The log may be truncated to just selected lines or include another fatal format.
        markers = [line.strip() for line in lines if 'GameThread timed out waiting' in line]

    print('Orbitals timeout log analysis')
    print(f'Input: {args.log}')
    if markers:
        print('\nFatal event:')
        for line in markers[-2:]:
            print(f'  {line}')
    else:
        print('\nNo svc #0x26 Break signature found in this log.')

    abort_texts = [m.group(1) for line in fatal_lines if (m := TEXT_RE.match(line))]
    timeout_texts = [text for text in abort_texts if 'timed out waiting' in text]
    target_texts = [text for text in abort_texts if text in ('RenderThread', 'RHIThread')]
    if timeout_texts:
        print('\nTimeout message found in abort stack text:')
        for text in timeout_texts:
            print(f'  {text}')
        if target_texts:
            print(f"  Captured wait target string: {', '.join(target_texts)}")

    backend = []
    for line in lines:
        if re.search(r'\bvk: using gpu\b', line, re.I):
            backend.append(line.strip())
        if re.search(r'\bORBITALS_GPU\s*=\s*null\b', line, re.I):
            backend.append(line.strip())
    if backend:
        print('\nGraphics backend evidence:')
        for line in backend[-4:]:
            print(f'  {line}')

    threads = parse_threads(fatal_lines)
    if threads:
        counts = Counter(classify_state(t['state']) for t in threads)
        print(f'\nThread snapshot: {len(threads)} guest threads')
        print('  State counts: ' + '; '.join(f'{state}: {n}' for state, n in counts.most_common()))
        key_threads = [t for t in threads if re.search(
            r'GameThread|RenderThread|RHIThread|RHIInterruptThread|RTHeartBeat|FHeartBeat|HttpManagerThread',
            t['name'], re.I)]
        shown = threads if args.all_threads else key_threads
        if shown:
            print('\nRelevant thread records (stacks are from the fatal snapshot):')
            for t in shown:
                flags = t['flags'].strip()
                suffix = f' {flags}' if flags else ''
                print(f"  {t['handle']} \"{t['name']}\" prio {t['prio']} core {t['core']}{suffix}: "
                      f"{classify_state(t['state'])}")
                for frame in t['frames'][:8]:
                    print(f'    {frame}')
        else:
            print('  No recognized GameThread/render thread names in the parsed dump.')
        mutex = [t for t in threads if 'waiting on a mutex' in t['state']]
        if mutex:
            print('\nThreads reported waiting on a mutex:')
            for t in mutex:
                print(f"  {t['handle']} \"{t['name']}\": {t['state']}")
    else:
        print('\nNo thread dump parsed. Provide the full fatal section, including “every guest thread”.')

    elapsed = [line.strip() for line in fatal_lines if 'run time at the abort:' in line]
    if elapsed:
        print('\n' + elapsed[-1])

    print('\nInterpretation limits:')
    print('  The fatal thread dump is taken while the GameThread is handling the abort; its stack is not the pre-timeout wait stack.')
    print('  “running or ready” is a scheduler snapshot label, not proof the thread was actively making progress.')
    print('  Sleeping worker threads are common; these records alone cannot identify a missed signal or prove a deadlock.')
    print('  This report summarizes evidence only; it does not claim a root cause.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
