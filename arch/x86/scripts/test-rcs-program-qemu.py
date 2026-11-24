"""Native program file validation plus PE compiler/UMD regressions; RAM disks."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

root = Path(__file__).resolve().parents[3]
kernel = root / 'arch/x86/build/directx-current/kernel.elf'
boot = root / 'arch/x86/build/boot.efi'
probes = root / 'arch/x86/build/native-compiler-umd'
programs = root / 'arch/x86/build/native-rcs-program'
parser = argparse.ArgumentParser()
parser.add_argument('output')
parser.add_argument('--primitive-fixtures', type=Path)
parser.add_argument('--sampled-fixtures', type=Path)
parser.add_argument('--copy-rect-probes', type=Path)
parser.add_argument('--copy-rect-only', action='store_true')
args = parser.parse_args()
out = Path(args.output).resolve()
out.mkdir(parents=True, exist_ok=False)
results = {'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
           'completed': False, 'passed': False, 'tests': {}}
cases = []
for bits in (32, 64):
    (out / f'pe{bits}').mkdir()
    for name in ['compiler-api-test', f'umd{bits}-model', f'umd{bits}-rcs-model']:
        marker = b'positive=102 rejected=60 failures=0' if name == 'compiler-api-test' else b'UMD failures=00000000'
        cases.append((f'pe{bits}/{name}', probes / f'pe{bits}/{name}.exe', '', marker))
    results[f'dll{bits}_sha256'] = hashlib.sha256((probes / f'pe{bits}/osito-intel9.dll').read_bytes()).hexdigest()
    if args.copy_rect_probes:
        cases.append((f'pe{bits}/gpurect{bits}',args.copy_rect_probes/f'pe{bits}/gpurect{bits}.exe',
                      '',b'RECTCOPY UNAVAILABLE native-prototype'))
bundle = (programs / 'compiled-fixtures.bin').read_bytes()
shader_cases = [('missing', None, b'GPU shader test: FAIL')]
for label, word, value in [('magic',0,0),('version',1,2),('no-cases',2,0),('too-many',2,257),
                            ('length',3,len(bundle)-1),('case-size',4,0xffffffff),('dispatch',6,8),
                            ('reserved',7,1),('program-abi',8,4),('scratch',19,1024)]:
    changed = bytearray(bundle)
    struct.pack_into('<I',changed,word*4,value)
    shader_cases.append((label, bytes(changed), b'GPU shader test: FAIL'))
shader_cases.extend([('truncated',bundle[:15],b'GPU shader test: FAIL'),
                     ('qualified',bundle,b'GPU shader test: unavailable')])
if args.primitive_fixtures:
    geometry = args.primitive_fixtures.read_bytes()
    start = 32 + struct.unpack_from('<I',geometry,16)[0]
    assert struct.unpack_from('<I',geometry,4)[0] == 2
    for label,offset,value in [('geometry-size',28,0),('geometry-huge',28,0xffffffff),
        ('topology',start,3),('vertices-zero',start+4,0),('vertices-huge',start+4,513),
        ('triangle-incomplete',start+4,4),('scissor-x',start+8,96),('scissor-y',start+12,72),
        ('scissor-width',start+16,0),('scissor-height',start+20,73),
        ('vertex-bytes',start+24,0xffffffff),('mask-bytes',start+28,0),('geometry-reserved',start+32,1),
        ('vertex-nan',start+40,0x7fc00000),('vertex-infinity',start+44,0x7f800000),
        ('vertex-z',start+48,0x40000000),('vertex-w',start+52,0)]:
        changed=bytearray(geometry)
        struct.pack_into('<I',changed,offset,value)
        shader_cases.append((label,bytes(changed),b'GPU shader test: FAIL'))
    shader_cases.append(('primitive-qualified',geometry,b'GPU shader test: unavailable'))
if args.sampled_fixtures:
    sampled = (args.sampled_fixtures / 'basic.bin').read_bytes()
    assert struct.unpack_from('<I', sampled, 4)[0] == 3
    first_draw = 32 + struct.unpack_from('<I', sampled, 16)[0]
    first_code = 32 + struct.unpack_from('<I', sampled, 32 + 24)[0]
    for label, offset, value in [('sampled-version',4,4),('sampled-size',28,0),
        ('sampled-huge',28,0xffffffff),('sampled-abi',32,2),
        ('sampled-code',first_code,0xffffffff),('sampled-batch',first_draw,0),
        ('sampled-pitch',first_draw+40,0),('sampled-extra-byte',12,len(sampled)+1)]:
        changed=bytearray(sampled)
        struct.pack_into('<I',changed,offset,value)
        shader_cases.append((label,bytes(changed),b'GPU shader test: FAIL'))
    shader_cases.append(('sampled-qualified',sampled,b'GPU shader test: unavailable'))
    shader_cases.append(('sampled-filtering-qualified',(args.sampled_fixtures/'filtering.bin').read_bytes(),
                         b'GPU shader test: unavailable'))
if args.copy_rect_only:
    assert args.copy_rect_probes
    cases=[case for case in cases if '/gpurect' in case[0]]
    shader_cases=[]
process = None
stop = threading.Event()
uart = None
reader = None
serial = out / 'serial.log'

def prompt(start=0, timeout=100):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        assert process.poll() is None, 'QEMU exited'
        data = serial.read_bytes()[start:] if serial.exists() else b''
        assert b'!!! EXCEPTION' not in data and b'SYSTEM HALTED' not in data, data[-2500:]
        if b'osito> ' in data:
            return data
        time.sleep(.1)
    raise TimeoutError('QEMU shell did not return')

def drain_uart():
    while not stop.is_set():
        try:
            if not uart.recv(65536): return
        except socket.timeout: pass
        except OSError: return

def local(command):
    start = serial.stat().st_size
    for byte in (command + '\r').encode():
        uart.sendall(bytes([byte]))
        time.sleep(.015)
    return prompt(start)

with tempfile.TemporaryDirectory(prefix='osito-rcs-program-', dir='/dev/shm') as temporary:
    work = Path(temporary)
    image = work / 'data.img'
    try:
        subprocess.run(['truncate', '-s', '64M', str(work / 'esp.img')], check=True)
        subprocess.run(['mformat', '-i', str(work / 'esp.img'), '-F', '::'], check=True)
        subprocess.run(['mmd', '-i', str(work / 'esp.img'), '::/EFI', '::/EFI/BOOT'], check=True)
        for source, destination in [(boot, '::/EFI/BOOT/BOOTX64.EFI'), (kernel, '::/EFI/BOOT/kernel.elf')]:
            subprocess.run(['mcopy', '-i', str(work / 'esp.img'), str(source), destination], check=True)
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', work / 'vars.fd')
        subprocess.run(['truncate', '-s', '96M', str(image)], check=True)
        subprocess.run([str(root / 'tools/ositofs/mkfs.ositofs3'), str(image), '--inodes', '4096'], check=True)
        (work / 'noautoload').touch()
        files = [(work / 'noautoload', '/.noautoload'), (kernel, '/kernel.elf')]
        files.extend((probes / f'pe{bits}/osito-intel9.dll', f'/pe{bits}/osito-intel9.dll') for bits in (32, 64))
        files.extend((source, '/' + name + '.exe') for name, source, _, _ in cases)
        for label, contents, _ in shader_cases:
            if contents is None: continue
            source = work / (label + '.bin')
            source.write_bytes(contents)
            files.append((source, '/' + label + '.bin'))
        for source, destination in files:
            subprocess.run([str(root / 'tools/ositofs/ositofs-write3'), str(image),
                            str(source), '--dest', destination], check=True, stdout=subprocess.DEVNULL)
        command = ['qemu-system-x86_64', '-name', 'osito-rcs-program', '-machine', 'q35',
                   '-accel', 'kvm', '-cpu', 'host', '-m', '2G', '-smp', '4',
                   '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                   '-drive', f'if=pflash,format=raw,file={work / "vars.fd"}',
                   '-drive', f'format=raw,if=ide,file={work / "esp.img"}',
                   '-drive', f'format=raw,if=none,id=data,file={image}',
                   '-device', 'nvme,serial=osito-crt,drive=data',
                   '-netdev', 'user,id=net0', '-device', 'virtio-net-pci,netdev=net0,disable-legacy=on',
                   '-display', 'none', '-vga', 'std', '-monitor', 'none',
                   '-chardev', f'socket,id=uart,path={work / "serial.sock"},server=on,wait=off,logfile={serial}',
                   '-serial', 'chardev:uart', '-no-reboot', '-no-shutdown']
        (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (out / 'qemu.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
        prompt()
        uart = socket.socket(socket.AF_UNIX)
        uart.settimeout(.2)
        uart.connect(str(work / 'serial.sock'))
        reader = threading.Thread(target=drain_uart)
        reader.start()
        for name, source, args, marker in cases:
            data = local(f'winexec /{name}.exe{args}')
            (out / (name + '.log')).write_bytes(data)
            exit_marker=b'process exited, code=2' if '/gpurect' in name else b'process exited, code=0'
            passed = (marker in data and b'FAIL line' not in data and b'UMD FAIL' not in data
                      and b'GPUCOPY FAIL' not in data and exit_marker in data)
            results['tests'][name] = {'passed': passed, 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(name, passed, flush=True)
            assert passed, data[-3500:]
        for label, _, marker in shader_cases:
            data = local('gpu-shader-test /' + label + '.bin')
            (out / ('shader-' + label + '.log')).write_bytes(data)
            passed = marker in data and b'[IGFX] native RCS program' not in data
            results['tests']['shader-' + label] = {'passed': passed}
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print('shader-' + label, passed, flush=True)
            assert passed, data[-3500:]
        results['completed'] = results['passed'] = True
    finally:
        stop.set()
        if uart: uart.close()
        if reader: reader.join(timeout=2)
        if process and process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
