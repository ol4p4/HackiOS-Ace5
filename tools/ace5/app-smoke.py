#!/usr/bin/env python3
"""The 16.5 exit gate: the four system-app groups, judged from a run's artifacts.

`docs/DRIVERS.md` names this script as the gate but it did not exist. It does
not drive the phone: it reads what `phone.py run` already saved (the log, and
optionally the screenshot series) and answers per case PASS / FAIL / BLOCKED.

    python3 tools/ace5/app-smoke.py logs/giulia/20261030-142126-run.log
    python3 tools/ace5/app-smoke.py --log L --group network
    python3 tools/ace5/app-smoke.py --list
    python3 tools/ace5/app-smoke.py --log L --json

A run exercises one recipe, so judge the group it exercised with --group; a
case outside the selected groups is not counted either way.

BLOCKED is a real answer: a group whose subsystem has not been ported (Wi-Fi,
audio, camera) is not a failure of this run, and it is not green either. The
gate is "all four groups PASS", so a BLOCKED group keeps the gate closed.
"""
import argparse
import json
import re
import sys
from pathlib import Path

# (group, case, status, what proves it)
CASES = [
    ('basic', 'SpringBoard reaches the home screen', 'PASS',
     r'System shell "com\.apple\.springboard" finished startup'),
    ('basic', 'the compositor draws frames', 'PASS',
     r'redwood-gl:.*frames|glshow:.*frame|MiniIOMobileFramebuffer: \d+ frames'),
    ('basic', 'touch reports', 'PASS',
     r's3910: ready|s3910: reports=\d+'),
    ('basic', 'Settings opens (needs a run with rc.jobs.settings)', 'PASS',
     r'SpringBoard was asked to open .*Preferences|Preferences: finished startup'),
    ('network', 'loopback is up', 'PASS',
     r'lo0: inet 127\.0\.0\.1'),
    ('network', 'Safari renders a page over loopback', 'PASS',
     r'webserve: GET / -> 200'),
    ('network', 'Wi-Fi radio joins an AP', 'BLOCKED',
     r'ath12k: firmware ready|wlan0: .*associated'),
    ('audio', 'ADSP is running', 'BLOCKED',
     r'adsp: running|qcom,adsp-loader: running'),
    ('audio', 'a sound plays', 'BLOCKED',
     r'speaker: .*playing|audio: .*route'),
    ('camera', 'camera preview', 'BLOCKED',
     r'camss: .*stream|Camera: .*preview'),
]

GROUPS = ['basic', 'network', 'audio', 'camera']


def evaluate(log_text):
    rows = []
    for group, case, status, pattern in CASES:
        found = re.search(pattern, log_text, re.IGNORECASE) is not None
        if status == 'BLOCKED':
            verdict = 'BLOCKED'
        else:
            verdict = 'PASS' if found else 'FAIL'
        rows.append({'group': group, 'case': case, 'verdict': verdict,
                     'evidence': 'log pattern matched' if found else ''})
    return rows


def group_verdicts(rows):
    out = {}
    for group in GROUPS:
        cases = [r for r in rows if r['group'] == group]
        if any(c['verdict'] == 'FAIL' for c in cases):
            out[group] = 'FAIL'
        elif all(c['verdict'] == 'BLOCKED' for c in cases):
            out[group] = 'BLOCKED'
        elif any(c['verdict'] == 'BLOCKED' for c in cases):
            out[group] = 'PARTIAL'
        else:
            out[group] = 'PASS'
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--log', type=Path, help='a phone.py run log')
    ap.add_argument('--group', action='append', choices=GROUPS,
                    help='judge only this group (repeatable); default all')
    ap.add_argument('--list', action='store_true', help='print the case table and exit')
    ap.add_argument('--json', action='store_true', help='machine-readable output')
    args = ap.parse_args()

    if args.list:
        for group, case, status, _ in CASES:
            print('%-8s %-6s %s' % (group, status, case))
        return 0
    if not args.log:
        ap.error('--log is required unless --list is given')
    if not args.log.is_file():
        raise SystemExit('no such log: %s' % args.log)

    rows = evaluate(args.log.read_text(encoding='utf-8', errors='replace'))
    selected = args.group or GROUPS
    rows = [row for row in rows if row['group'] in selected]
    groups = {g: v for g, v in group_verdicts(rows).items() if g in selected}
    gate = all(v == 'PASS' for v in groups.values())

    if args.json:
        print(json.dumps({'log': str(args.log), 'cases': rows, 'groups': groups,
                          'gate': 'PASS' if gate else 'OPEN'}, indent=2))
        return 0 if gate else 1

    print('%-8s %-8s %s' % ('group', 'verdict', 'case'))
    for row in rows:
        print('%-8s %-8s %s' % (row['group'], row['verdict'], row['case']))
    print()
    for group in selected:
        print('%-8s %s' % (group, groups[group]))
    print('\n16.5 gate: %s' % ('PASS' if gate else 'OPEN'))
    return 0 if gate else 1


if __name__ == '__main__':
    sys.exit(main())
