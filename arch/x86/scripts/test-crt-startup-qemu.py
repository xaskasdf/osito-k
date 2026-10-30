"""CRT/startup and native address-wait integration. Temporary disks live in RAM."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

root = Path(__file__).resolve().parents[3]
kernel = root / 'arch/x86/build/directx-current/kernel.elf'
boot = root / 'arch/x86/build/boot.efi'
probes = root / 'arch/x86/build/crt-startup'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('out')
parser.add_argument('--compiler-bits', type=int, choices=(32, 64), default=64)
parser.add_argument('--compiler-only', action='store_true')
parser.add_argument('--stat-probes', type=Path)
args = parser.parse_args()
out = Path(args.out).resolve()
out.mkdir(parents=True, exist_ok=False)
results = {'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
           'completed': False, 'passed': False, 'tests': {}}
cases = []
for bits in (32, 64):
    for repeat in range(2):
        cases.append((f'crt-exit{bits}-{repeat}', probes / f'crt-exit{bits}.exe', '', b'CRT EXIT PASS'))
    cases.append((f'crt-read{bits}', probes / f'crt-read{bits}.exe', '', b'CRT READ PASS'))
    for variant in ('public', 'native'):
        name = f'address-wait{bits}-{variant}'
        cases.append((name, probes / (name + '.exe'), '', b'ADDRESS WAIT PASS'))
    cases.append((f'crt-args{bits}', probes / f'crt-args{bits}.exe',
                  ' first "a b" "" tail', b'CRT ARGS PASS'))
    cases.append((f'compiler-crt{bits}', root / f'arch/x86/build/native-compiler-crt/compiler-crt{bits}.exe',
                  ' first second', b'COMPILER CRT failures=00000000'))
    cases.append((f'cancel{bits}', root / f'arch/x86/build/directx-current/cancel-probes/pe{bits}/cancelstack{bits}.exe',
                  '', b'CANCEL-STACK PASS'))
if args.compiler_only:
    cases = []
if args.stat_probes:
    for bits in (32, 64):
        for mode in ('legacy', 'ucrt'):
            cases.append((f'stat{bits}-{mode}', args.stat_probes.resolve() / f'pe{bits}/crt_stat_pe{bits}.exe',
                          ' far' + (' ucrt' if mode == 'ucrt' else ''), b'0 failures'))
compiler_bits = args.compiler_bits
compiler_name = f'compiler{compiler_bits}'
exit_marker = b'PE process exited, code=' if compiler_bits == 32 else b'PE64 process exited, code='
fixture = root / 'arch/x86/build/native-compiler-fixtures/ps_3_0-mov.bin'
compiler = root / f'arch/x86/build/native-intel-compiler-pe{compiler_bits}/native-compiler.exe'
results['compiler_bits'] = compiler_bits
results['compiler_sha256'] = hashlib.sha256(compiler.read_bytes()).hexdigest()
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

with tempfile.TemporaryDirectory(prefix='osito-crt-', dir='/dev/shm') as temporary:
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
        files = [(work / 'noautoload', '/.noautoload'), (kernel, '/kernel.elf'),
                 (compiler, f'/{compiler_name}.exe'), (fixture, '/input.bin'),
                 (root / 'arch/x86/build/native-shader-fixtures/vs20-position.bin', '/unsupported.bin')]
        files.extend((source, '/' + name + '.exe') for name, source, _, _ in cases)
        for source, destination in files:
            subprocess.run([str(root / 'tools/ositofs/ositofs-write3'), str(image),
                            str(source), '--dest', destination], check=True, stdout=subprocess.DEVNULL)
        command = ['qemu-system-x86_64', '-name', 'osito-crt-startup', '-machine', 'q35',
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
            passed = marker in data and b'FAIL' not in data
            results['tests'][name] = {'passed': passed, 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(name, passed, flush=True)
            assert passed, data[-3500:]
        for repeat in range(8):
            data = local(f'winexec /{compiler_name}.exe C:/input.bin C:/output.eu 40800000 40400000 40000000 3f800000')
            (out / f'{compiler_name}-{repeat}.log').write_bytes(data)
            passed = (b'validated=1' in data and b'scratch=0' in data and
                      b'D3D9 constant result matches all four expected channels' in data and
                      exit_marker + b'0' in data)
            results['tests'][f'{compiler_name}-{repeat}'] = {'passed': passed}
            print(compiler_name, repeat, passed, flush=True)
            assert passed, data[-4000:]
        data = local(f'winexec /{compiler_name}.exe C:/unsupported.bin C:/rejected.eu')
        (out / 'compiler-rejection.log').write_bytes(data)
        passed = b'frontend status=2 ' in data and exit_marker + b'4' in data
        results['tests']['compiler-rejection'] = {'passed': passed}
        print('compiler-rejection', passed, flush=True)
        assert passed, data[-4000:]
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
