"""Native compiler API and D3D9 UMD lifetime tests. Temporary disks live in RAM."""
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
probes = root / 'arch/x86/build/native-compiler-umd'
out = Path(sys.argv[1]).resolve()
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

with tempfile.TemporaryDirectory(prefix='osito-compiler-umd-', dir='/dev/shm') as temporary:
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
        for source, destination in files:
            subprocess.run([str(root / 'tools/ositofs/ositofs-write3'), str(image),
                            str(source), '--dest', destination], check=True, stdout=subprocess.DEVNULL)
        command = ['qemu-system-x86_64', '-name', 'osito-compiler-umd', '-machine', 'q35',
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
            passed = (marker in data and b'FAIL line' not in data and b'UMD FAIL' not in data
                      and b'process exited, code=0' in data)
            results['tests'][name] = {'passed': passed, 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(name, passed, flush=True)
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
