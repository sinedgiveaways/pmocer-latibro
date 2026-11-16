#!/usr/bin/env python3
"""A small Windows GUI for inspecting Orbitals Recomp orbital.log timeout dumps."""

import argparse
import re
import sys
import tkinter as tk
from collections import Counter
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

THREAD_RE = re.compile(
    r'^\s*thread\s+(0x[0-9a-fA-F]+)\s+"([^"]*)"\s+'
    r'prio\s+(-?\d+)\s+core\s+(-?\d+)([^:]*):\s*(.*)$'
)
FRAME_RE = re.compile(r'^\s*(?:guest )?frame\s+\d+:\s*(.*)$')
FATAL_RE = re.compile(r'^\s*FATAL: svc #0x26 Break at\b')
KEY_THREADS = re.compile(
    r'GameThread|RenderThread|RHIThread|RHIInterruptThread|RTHeartBeat|FHeartBeat|HttpManagerThread', re.I
)


def analyze_text(text, all_threads=False):
    lines = text.splitlines()
    fatal_start = -1
    for i, line in enumerate(lines):
        if FATAL_RE.search(line):
            fatal_start = i
    if fatal_start >= 0:
        end = len(lines)
        for i in range(fatal_start + 1, len(lines)):
            if 'FATAL: svc #0x26 Break is not implemented' in lines[i]:
                end = i + 1
                break
        lines = lines[fatal_start:end]

    report = ['ORBITALS RECOMP — TIMEOUT LOG ANALYSIS', '=' * 44]
    if fatal_start >= 0:
        fatal = next((s.strip() for s in lines if FATAL_RE.search(s)), None)
        report += ['', 'Fatal event:', f'  {fatal}']
    elif any('GameThread timed out waiting' in s for s in lines):
        report += ['', 'Timeout text found, but no complete svc Break block was present.']
    else:
        report += ['', 'No svc #0x26 Break signature found in the selected log.']

    if any('GameThread timed out waiting for %s after' in s for s in lines):
        report += ['', 'Timeout evidence:', '  Unreal abort stack contains the GameThread timeout format string.']
        if any(re.search(r'sp\+0x[0-9a-fA-F]+ "RenderThread"', s) for s in lines):
            report.append('  Captured wait target string: RenderThread.')
        report.append('  The captured format string does not include the substituted timeout duration.')

    backend = [s.strip() for s in text.splitlines() if '[host]' in s and 'vk: using gpu' in s]
    if backend:
        report += ['', 'Graphics backend evidence (Vulkan selected):']
        report.extend('  ' + s for s in backend[-3:])
    else:
        report += ['', 'No Vulkan device-selection line found. This alone does not prove which backend ran.']

    threads = []
    current = None
    in_dump = False
    for line in lines:
        if 'every guest thread' in line:
            in_dump = True
            continue
        if not in_dump:
            continue
        m = THREAD_RE.match(line)
        if m:
            current = {
                'handle': m.group(1), 'name': m.group(2), 'prio': int(m.group(3)),
                'core': int(m.group(4)), 'flags': m.group(5).strip(),
                'state': m.group(6).strip(), 'frames': [],
            }
            threads.append(current)
            continue
        fm = FRAME_RE.match(line)
        if fm and current is not None:
            current['frames'].append(fm.group(1).strip())
        if line.startswith('  run time at the abort:'):
            break

    if threads:
        def state_kind(t):
            s = t['state']
            for phrase, label in (
                ('waiting on a condition variable', 'condition-variable wait'),
                ('waiting on a mutex', 'mutex wait'), ('waiting on an address', 'address wait'),
                ('waiting on handles', 'handle wait'), ('running or ready', 'running or ready (snapshot label)'),
            ):
                if phrase in s:
                    return label
            return 'other'

        counts = Counter(state_kind(t) for t in threads)
        report += ['', f'Thread snapshot: {len(threads)} guest threads', '  State counts:']
        report.extend(f'    {name}: {count}' for name, count in sorted(counts.items()))
        chosen = threads if all_threads else [t for t in threads if KEY_THREADS.search(t['name'])]
        report += ['', 'Thread stacks (these are fatal-snapshot stacks, not necessarily pre-timeout stacks):']
        if chosen:
            for t in chosen:
                suffix = (' ' + t['flags']) if t['flags'] else ''
                report.append(f"  {t['handle']} \"{t['name']}\" prio {t['prio']} core {t['core']}{suffix}: {t['state']}")
                report.extend('    ' + frame for frame in t['frames'][:8])
        else:
            report.append('  No recognized game/render thread names; choose Include all threads.')
        mutexes = [t for t in threads if 'waiting on a mutex' in t['state']]
        if mutexes:
            report += ['', 'Reported mutex waiters:']
            report.extend(f"  {t['handle']} \"{t['name']}\": {t['state']}" for t in mutexes)
    else:
        report += ['', 'No guest-thread dump parsed. The log needs the section beginning “every guest thread”.']

    runtime = [s.strip() for s in lines if 'run time at the abort:' in s]
    if runtime:
        report += ['', runtime[-1]]
    report += [
        '', 'Interpretation limits:',
        '  The fatal snapshot is taken after GameThread enters the abort handler; it is not its original wait stack.',
        '  “Running or ready” is a snapshot label, not proof that the thread was making progress.',
        '  Idle worker waits are common. This report summarizes evidence; it does not determine root cause.',
    ]
    return '\n'.join(report)


class Analyzer(tk.Tk):
    def __init__(self, initial_path=None):
        super().__init__()
        self.title('Orbitals Timeout Log Analyzer')
        self.geometry('900x700')
        self.minsize(650, 420)
        self.path = tk.StringVar(value=str(initial_path or ''))
        top = ttk.Frame(self, padding=10)
        top.pack(fill='x')
        ttk.Label(top, text='orbital.log:').pack(side='left')
        ttk.Entry(top, textvariable=self.path).pack(side='left', fill='x', expand=True, padx=8)
        ttk.Button(top, text='Browse…', command=self.browse).pack(side='left')
        ttk.Button(top, text='Analyze', command=self.analyze).pack(side='left', padx=(8, 0))
        opts = ttk.Frame(self, padding=(10, 0, 10, 8))
        opts.pack(fill='x')
        self.all_threads = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text='Include all threads', variable=self.all_threads,
                        command=self.analyze_if_loaded).pack(side='left')
        ttk.Button(opts, text='Copy report', command=self.copy_report).pack(side='right')
        body = ttk.Frame(self, padding=(10, 0, 10, 10))
        body.pack(fill='both', expand=True)
        self.output = tk.Text(body, wrap='word', font=('Consolas', 10), state='disabled')
        scroll = ttk.Scrollbar(body, orient='vertical', command=self.output.yview)
        self.output.configure(yscrollcommand=scroll.set)
        self.output.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self.status = tk.StringVar(value='Choose orbital.log, or drop its path in the box, then click Analyze.')
        ttk.Label(self, textvariable=self.status, anchor='w', padding=(10, 5)).pack(fill='x')
        if initial_path:
            self.after(150, self.analyze)

    def browse(self):
        chosen = filedialog.askopenfilename(
            title='Select Orbitals orbital.log',
            filetypes=[('Orbitals log', 'orbital*.log'), ('Log files', '*.log'), ('All files', '*.*')],
        )
        if chosen:
            self.path.set(chosen)
            self.analyze()

    def analyze_if_loaded(self):
        if self.path.get().strip():
            self.analyze()

    def analyze(self):
        path = Path(self.path.get().strip().strip('"'))
        if not path.is_file():
            messagebox.showerror('Log not found', 'Select a valid orbital.log file first.', parent=self)
            return
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
            report = analyze_text(text, self.all_threads.get())
        except OSError as exc:
            messagebox.showerror('Cannot read log', str(exc), parent=self)
            return
        self.output.configure(state='normal')
        self.output.delete('1.0', 'end')
        self.output.insert('1.0', report)
        self.output.configure(state='disabled')
        self.status.set(f'Analyzed {path}  |  {path.stat().st_size:,} bytes  |  Read-only; no log uploaded.')

    def copy_report(self):
        report = self.output.get('1.0', 'end-1c')
        if not report:
            messagebox.showinfo('No report', 'Analyze a log first.', parent=self)
            return
        self.clipboard_clear()
        self.clipboard_append(report)
        self.status.set('Report copied to clipboard.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', nargs='?', help='optional log path; otherwise a file picker opens')
    parser.add_argument('--all-threads', action='store_true', help='show all thread stacks on startup')
    parser.add_argument('--smoke-test', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.smoke_test:
        assert 'Timeout evidence:' in analyze_text(
            'FATAL: svc #0x26 Break at sdk+0x1: abort\n'
            '  texts the abort\'s registers and stack point at:\n'
            '    sp+0x10 "GameThread timed out waiting for %s after %.02f secs"\n'
            '    sp+0x20 "RenderThread"\n'
            '  every guest thread:\n'
            '  thread 0x1002 "GameThread" prio 43 core 1 (this one): running or ready 0\n'
            '    frame 0: sdk+0x123\n'
            '  run time at the abort: 121.71 s\n'
        )
        return 0
    app = Analyzer(args.log)
    if args.all_threads:
        app.all_threads.set(True)
    app.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
