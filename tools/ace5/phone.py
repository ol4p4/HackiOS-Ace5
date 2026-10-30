#!/usr/bin/env python3
"""Ace 5 RAM boot and single-core trials. No flash, erase or slot changes.

The loader may write logdump, as explicitly authorized for this device.
All iOS images are sent to RAM. Other storage and unported drivers stay disabled.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--serial', required=True, help='Original ADB/bootloader serial')
sub = ap.add_subparsers(dest='command', required=True)
enter = sub.add_parser('enter', help='Temporary boot into the giulia loader')
enter.add_argument('--stage', choices=['diagnostic', 'kernel', 'persist'], default='diagnostic')
ramdisk = sub.add_parser('ramdisk', help='Upload a system image to RAM')
ramdisk.add_argument('--image', type=Path, help='Explicit local system APFS image; defaults to userland/system.apfs')
ramdisk.add_argument('--persist', action='store_true', help='Require the persistence loader (storage writes enabled)')
run = sub.add_parser('run', help='Run single-core iOS, then save logs and framebuffer')
run.add_argument('--persist', action='store_true', help='Enable UFS read/write and var_store persistence')
run.add_argument('--dry-run', action='store_true',
                 help='Print the payload command and command line without touching the phone')
run.add_argument('--seconds', type=int, default=120,
                 help='Return after this many seconds; 0 keeps iOS running until a physical restart')
run.add_argument('--verbose', action='store_true')
run.add_argument('--kernel', type=Path, help='Explicit locally built Mach-O kernel')
run.add_argument('--root-image', type=Path, help='Local small APFS image, e.g. with device firmware')
run.add_argument('--touch', action='store_true', help='Enable the giulia S3910 driver; requires --kernel')
run.add_argument('--pcie', type=int, choices=[1, 2],
                 help='Ace 5 PCIe0: 1 read-only survey, 2 power-up and link training; requires --kernel')
run.add_argument('--gpu-probe', type=int, choices=range(1, 21),
                 help='Ace 5 GPU probe: 1 GCC, 2 GPUCC, 3 CX power, 4 XO clocks/GMU, 5 TCM, 6 streams, 7 firmware, 8 SMMU, 9 operating clocks, 10 GMU boot, 11 HFI query, 12 power tables, 13 core firmware, 14 scheduler memory, 15 GX power, 16 GPU SMMU and firmware, 17 CP initialization, 18 ZAP authentication, 19 nonsecure CP DMA, 20 BR/BV indirect buffer; requires --kernel')
run.add_argument('--bootcpu', type=int, choices=[0, 1], default=0,
                 help='0: validated boot core; 1: experimental separate USB observer core')
run.add_argument('--live-log', action='store_true',
                 help='Save live log snapshots every 10 seconds; requires CPU 1 and a finite run')
run.add_argument('--live-interval', type=float, default=10,
                 help='Seconds between live-log snapshots, 0.1 to 60 (default 10)')
run.add_argument('--shots', type=float, default=0,
                 help='Take a screenshot every N seconds during the run (0: only the one at the end). '
                      'A series is what tells "the picture drifted" from "the boot started bad"')
run.add_argument('--gpu-step-delay', type=int, default=0,
                 help='Pause 0 to 3000 ms before each new GPU core register access for tracing')
run.add_argument('--gpu-direct', action='store_true',
                 help='Use a host CP ring instead of GMU HWSCHED; requires --gpu-probe 13 or later')
run.add_argument('--gpu-keep', action='store_true',
                 help='Publish the experimental A750 RedwoodGPU backend; requires direct stage 20 and zero trace delay')
run.add_argument('--render-trace', action='store_true',
                 help='Log each desktop frame and GL calls slower than 20 ms; requires --gpu-keep')
run.add_argument('--frame-timing', action='store_true',
                 help='Log touch, compositor, presentation and CPU timing for tools/frames.py')
run.add_argument('--thread-trace', choices=['SpringBoard', 'backboardd', 'Preferences', 'wifid',
                                          'mediaserverd', 'PosterBoard', 'CollectionsPoster'],
                 help='Sample up to 16 threads with the existing kernel stack sampler every 5 seconds')
run.add_argument('--metal', action='store_true',
                 help='Allow Metal discovery properties; requires --gpu-keep and a root with RedwoodMetal.bundle')
run.add_argument('--gpu-cached', action='store_true',
                 help='Experimental cached buffers with synchronous ownership; requires --gpu-keep')
run.add_argument('--fb-filter', choices=['bilinear', 'nearest'],
                 help='Display scaling filter; nearest trades edge smoothness for less CPU work')
run.add_argument('--gpu-display', action='store_true',
                 help='Experimental A750 hardware scaling directly into the panel; requires --gpu-keep')
selftest = sub.add_parser('selftest')
selftest.add_argument('--kernel', type=Path, help='Explicit locally built Mach-O kernel')
selftest.add_argument('--dry-run', action='store_true',
                      help='Print the payload command without touching the phone')
sub.add_parser('screenshot')
sub.add_parser('reboot', help='Return to the unchanged Android boot chain')
args = ap.parse_args()
logs = ROOT / 'logs/giulia'
logs.mkdir(parents=True, exist_ok=True)

def cpu_boot_arg():
    """One CPU selector: XNU panics if both cpus= and cpumask= are present.

    cpumask bits are positions in the loader's CPU list, not physical core
    numbers; the loader omits its own core (CPU0, which serves USB).
    """
    cpumask = os.environ.get('ACE5_CPUMASK', '')
    if cpumask:
        if not re.fullmatch(r'(0x[0-9a-fA-F]+|[0-9]+)', cpumask):
            raise RuntimeError('ACE5_CPUMASK must be a number such as 0x7F (positional)')
        return f'cpumask={cpumask}'
    cpus = os.environ.get('ACE5_CPUS', '1')
    if not cpus.isdigit() or not 1 <= int(cpus) <= 8:
        raise RuntimeError('ACE5_CPUS must be an integer 1..8')
    return f'cpus={cpus}'

def call(command, timeout=20, check=True):
    p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    output = p.stdout + p.stderr
    if check and (p.returncode or re.search(r'FAILED|^ERROR:', output, re.M)):
        raise RuntimeError(output.strip())
    return output

def fb(*command, timeout=20):
    return call(['fastboot', '-s', 'xnuloader', *map(str, command)], timeout)

def getvar(key):
    answer = fb('getvar', key)
    m = re.search(r'^' + re.escape(key) + r': (.*)$', answer, re.M)
    return m.group(1).strip() if m else ''

def ensure_loader(kernel=False, persist=None):
    writes = getvar('storage-writes')
    if (getvar('product') != 'xnuloader' or getvar('platform') != 'giulia' or
            (persist is not None and writes != ('enabled' if persist else 'disabled'))):
        raise RuntimeError('Requires the guarded giulia loader built by these tools')
    if kernel and getvar('build-stage') not in ('kernel', 'persist'):
        raise RuntimeError('Diagnostic loader cannot execute XNU; enter --stage kernel first')

def wait_device(serial):
    for _ in range(60):
        devices = call(['fastboot', 'devices'])
        if any(line.split() and line.split()[0] == serial for line in devices.splitlines()):
            return
        time.sleep(1)
    raise RuntimeError(f'Device {serial} did not appear; no flash fallback is attempted')

def screenshot(index=None, prefix=None):
    ensure_loader()
    w, h, stride = map(int, getvar('screen').split())
    fb('oem', 'screenshot')
    raw_path = logs / 'screen.raw'
    fb('get_staged', raw_path, timeout=60)
    data = raw_path.read_bytes()
    if not (0 < w <= 4096 and 0 < h <= 8192 and stride >= w * 4 and len(data) == stride * h):
        raise RuntimeError('Unexpected framebuffer geometry')
    rows = bytearray()
    for y in range(h):
        row = data[y * stride:y * stride + w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows += b'\0' + rgb
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    # A run with --shots takes many: the index keeps them in order and apart even
    # when two land in the same second.
    # One run names every shot with the same prefix, so the series groups: the
    # index says the order, the prefix says which boot it belongs to.
    suffix = '' if index is None else f'-{index:03d}'
    png = logs / ((prefix or time.strftime('%Y%m%d-%H%M%S')) + '-screen' + suffix + '.png')
    png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    raw_path.unlink()
    print(png)

try:
    if args.command == 'enter':
        directory = ROOT / {'diagnostic': 'out/giulia', 'kernel': 'out/giulia-kernel',
                            'persist': 'out/giulia-persist'}[args.stage]
        image = directory / f'hackios-giulia-{args.stage}.img'
        manifest = json.loads(image.with_suffix('.json').read_text())
        allowed = ['logdump', 'darwin'] if args.stage == 'persist' else ['logdump']
        if (manifest['stage'] != args.stage or manifest['allowed_partition_write'] != allowed or
                hashlib.sha256(image.read_bytes()).hexdigest() != manifest['sha256']):
            raise RuntimeError('Image manifest or SHA-256 mismatch')
        adb = ['adb', '-s', args.serial]
        if call(adb + ['get-state'], check=False).strip() == 'device':
            if call(adb + ['shell', 'getprop', 'ro.product.model']).strip() != 'PKG110':
                raise RuntimeError('Connected Android device is not PKG110')
            call(adb + ['reboot', 'bootloader'])
        wait_device(args.serial)
        stock = ['fastboot', '-s', args.serial]
        for key, value in [('product', 'pineapple'), ('unlocked', 'yes')]:
            if not re.search(r'^' + key + ': ' + value + r'$', call(stock + ['getvar', key]), re.M):
                raise RuntimeError('Unexpected or locked bootloader')
        print(call(stock + ['boot', str(image)], timeout=60), flush=True)
        wait_device('xnuloader')
        ensure_loader(kernel=args.stage in ('kernel', 'persist'), persist=args.stage == 'persist')
        print('Giulia loader ready; Android partitions were not flashed.')
    elif args.command == 'ramdisk':
        ensure_loader(kernel=True, persist=args.persist)
        image = args.image.resolve() if args.image else ROOT / 'userland/system.apfs'
        if not image.is_file() or not 0 < image.stat().st_size < 0xffffffff:
            raise RuntimeError('Missing or oversized system image')
        fb('oem', 'ramdisk')
        print(fb('stage', image, timeout=600))
    elif args.command in ('run', 'selftest'):
        persist = args.persist if args.command == 'run' else None
        if not args.dry_run:
            ensure_loader(kernel=True, persist=persist)
        payload = ROOT / 'out/giulia-kernel/run-payload.bin'
        seconds = args.seconds if args.command == 'run' else 15
        if not 0 <= seconds <= 3600:
            raise RuntimeError('seconds must be 0..3600')
        if args.command == 'run' and args.live_log and (args.bootcpu!=1 or seconds==0):
            raise RuntimeError('--live-log requires --bootcpu 1 and a finite return timer')
        cpu_arg = cpu_boot_arg()
        commandline = (f'{cpu_arg} loader_return_after={seconds} epoch={int(time.time())} sb_delay=8 '
                       'touchscreen=0 buttons=0 cpufreq=0 gpu=0 wifi=0 adsp=0')
        extra = os.environ.get('ACE5_CMDLINE', '').strip()
        if extra:
            if re.search(r'(^|\s)(flash|erase|ufs_write|var_store)\b', extra):
                raise RuntimeError('ACE5_CMDLINE may not enable flashing or storage writes')
            commandline += f' {extra}'
        print(f'CPU boot arg: {cpu_arg}', flush=True)
        if args.command == 'run' and args.persist:
            commandline += ' ufs=2 ufs_write=1 var_store=1'
        if args.command == 'run' and args.touch:
            if not args.kernel:
                raise RuntimeError('--touch requires an explicit locally built --kernel')
            commandline += ' ace5_touch=1'
        if args.command == 'run' and args.pcie:
            if not args.kernel:
                raise RuntimeError('--pcie requires an explicit locally built --kernel')
            commandline += f' pcie_step={args.pcie}'
        if args.command == 'run' and args.verbose:
            commandline += ' -v'
        if args.command == 'run' and args.gpu_direct:
            if not args.gpu_probe or args.gpu_probe < 13:
                raise RuntimeError('--gpu-direct requires --gpu-probe 13 or later')
            commandline += ' ace5_gpu_direct=1'
        if args.command == 'run' and args.gpu_keep:
            if args.gpu_probe!=20 or not args.gpu_direct or args.gpu_step_delay:
                raise RuntimeError('--gpu-keep requires --gpu-probe 20 --gpu-direct and zero trace delay')
            commandline += ' ace5_gpu_keep=1'
        if args.command == 'run' and args.render_trace:
            if not args.gpu_keep:
                raise RuntimeError('--render-trace requires --gpu-keep')
            commandline += ' glslow=20000 glframes=1'
        if args.command == 'run' and args.frame_timing:
            commandline += ' touch_lat=1 cpuload=1000'
        if args.command == 'run' and args.metal:
            if not args.gpu_keep:
                raise RuntimeError('--metal requires --gpu-keep')
            commandline += ' metal=1'
        if args.command == 'run' and args.gpu_cached:
            if not args.gpu_keep:
                raise RuntimeError('--gpu-cached requires --gpu-keep')
            commandline += ' gpu_cached=2'
        if args.command == 'run' and args.fb_filter:
            commandline += f' fb_filter={int(args.fb_filter == "bilinear")}'
        if args.command == 'run' and args.gpu_display:
            if not args.gpu_keep:
                raise RuntimeError('--gpu-display requires --gpu-keep')
            commandline += ' fb_gpu=1'
        if args.command == 'run' and args.thread_trace:
            commandline += (f' ustacks={args.thread_trace} ustacks_at=25 ustacks_ms=5000 '
                            'ustacks_count=60 ustacks_tname=* ustacks_threads=16 cpuload=1000')
        if args.command == 'run' and args.gpu_probe:
            if args.gpu_probe>=18 and not args.gpu_direct:
                raise RuntimeError('ZAP stages require --gpu-direct')
            if not args.kernel:
                raise RuntimeError('--gpu-probe requires an explicit locally built --kernel')
            if args.gpu_probe>=3 and (args.bootcpu!=1 or args.seconds==0):
                raise RuntimeError('GPU power probes require --bootcpu 1 and a finite return timer')
            commandline += f' ace5_gpu_probe={args.gpu_probe}'
            if not 0<=args.gpu_step_delay<=3000:
                raise RuntimeError('--gpu-step-delay must be between 0 and 3000 ms')
            if args.gpu_probe>=17 and args.seconds<10+(240*args.gpu_step_delay+999)//1000:
                raise RuntimeError('Return timer is too short for all GPU trace steps and cleanup')
            commandline += f' ace5_gpu_step_delay={args.gpu_step_delay}'
        live_interval=args.live_interval if args.command=='run' else 10
        if not 0.1<=live_interval<=60:
            raise RuntimeError('--live-interval must be between 0.1 and 60 seconds')
        if args.command == 'run' and args.bootcpu:
            commandline += f' bootcpu={args.bootcpu}'
        kernel = args.kernel if args.kernel else ROOT / 'prebuilt/mach.development.bcm2837'
        if not args.dry_run:
            if not kernel.is_file():
                raise RuntimeError(f'Missing kernel: {kernel}')
            print('Kernel SHA-256:', hashlib.sha256(kernel.read_bytes()).hexdigest(), flush=True)
        command = [sys.executable, str(ROOT / 'xnuloader/mkpayload.py'), '-o', str(payload),
                   f'kernel={kernel.resolve()}']
        if args.command == 'run':
            root_image = args.root_image or ROOT / 'userland/root.apfs'
            if not args.dry_run:
                if not root_image.is_file() or not 0 < root_image.stat().st_size <= 32 * 1024 * 1024:
                    raise RuntimeError('Missing or oversized small root image (limit 32 MiB)')
                print('Root image SHA-256:', hashlib.sha256(root_image.read_bytes()).hexdigest(), flush=True)
            command.append(f'ramdisk={root_image.resolve()}')
        command += ['--cmdline', commandline]
        if args.dry_run:
            print('Command line:', commandline, flush=True)
            print('Payload command:', ' '.join(map(str, command)), flush=True)
            sys.exit(0)
        print(call(command))
        fb('stage', payload, timeout=60)
        print(fb('oem', 'run' if args.command == 'run' else 'selftest'), flush=True)
        if args.command == 'run' and seconds == 0:
            print('XNU launch accepted; automatic return is disabled. '
                  'Use a physical power + volume-up restart to return to Android. '
                  'USB diagnostics are unavailable while XNU owns CPU 0.', flush=True)
            sys.exit(0)
        if args.command == 'run' and args.bootcpu == 0:
            print('XNU owns the boot core; waiting before asking USB for its result.', flush=True)
            time.sleep(seconds + 5)
        deadline = time.monotonic() + seconds + 60
        next_live = time.monotonic() + live_interval
        live_prefix = time.strftime('%Y%m%d-%H%M%S')
        live_count = 0
        # --shots: a picture every N seconds, so one boot yields a series instead of
        # a single frame at the end. Frame 1 tells whether it started clean, frame N
        # whether it drifted — the distinction that "4 % vs 51 % black" needed.
        shot_count = 0
        next_shot = time.monotonic() + args.shots if args.shots > 0 else None
        while True:
            result = fb('getvar', 'result')
            if 'result: running' not in result:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Kernel did not return; preserve the screen and inspect the live log')
            if args.command=='run' and args.live_log and time.monotonic()>=next_live:
                response=call(['fastboot','-s','xnuloader','oem','live-log'],check=False)
                # Stock fastboot discards the optional OKAY payload for OEM commands.
                if 'OKAY' in response and 'FAILED' not in response:
                    live_count+=1
                    path=logs/f'{live_prefix}-live-{live_count:03d}.log'
                    fb('get_staged',path)
                    print(f'Live log: {path.name} ({path.stat().st_size} bytes)',flush=True)
                elif 'no live kernel' not in response:
                    raise RuntimeError('Live-log request failed: '+response.strip())
                next_live=time.monotonic()+live_interval
            if next_shot is not None and time.monotonic() >= next_shot:
                shot_count += 1
                try:
                    screenshot(shot_count, live_prefix)
                except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
                    print(f'Screenshot {shot_count} failed: {error}', flush=True)
                next_shot += args.shots
            time.sleep(min(2, live_interval))
        log = logs / (time.strftime('%Y%m%d-%H%M%S') + '-' + args.command + '.log')
        # Fetch immediately: idle "oem log" would discard the staged kernel log.
        fb('get_staged', log)
        print(result, log)
        screenshot()
        saved_log = log.read_bytes().decode('utf-8', errors='replace')
        for line in saved_log.splitlines():
            if re.search(r'ace5-gpu: (stage [0-9]+|probe ended|.*cleanup FAILED)', line):
                print(line, flush=True)
        if 'panic(cpu ' in saved_log or re.search(r'ace5-gpu: (stage [0-9]+ FAILED|.*cleanup FAILED|live failure)', saved_log):
            raise RuntimeError(f'Kernel/probe failure; saved diagnostic log: {log}')
        if args.command == 'run' and args.gpu_probe and f'ace5-gpu: stage {args.gpu_probe} complete' not in saved_log:
            raise RuntimeError(f'GPU probe did not complete requested stage {args.gpu_probe}; see {log}')
        if args.command == 'run' and args.gpu_keep and 'ace5-gpu: persistent startup ready=1' not in saved_log:
            raise RuntimeError(f'A750 backend did not start; see {log}')
    elif args.command == 'screenshot':
        screenshot()
    else:
        ensure_loader()
        print(fb('reboot'))
except (RuntimeError, subprocess.TimeoutExpired, OSError, ValueError) as error:
    raise SystemExit(str(error))
