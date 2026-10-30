"""Qualify installed DirectX DLL selection using only project-owned fixtures."""
import hashlib
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

if len(sys.argv) not in (5,6,7,8):
    raise SystemExit('usage: test-native-dx-loader-qemu.py BOOT KERNEL FIXTURES OUTPUT [KMT_PROBES [CANCEL_PROBES [UMD_PROBES]]]')
boot=Path(sys.argv[1]).resolve(strict=True)
kernel=Path(sys.argv[2]).resolve(strict=True)
fixtures=Path(sys.argv[3]).resolve(strict=True)
kmt_probes=Path(sys.argv[5]).resolve(strict=True) if len(sys.argv)>=6 else None
cancel_probes=Path(sys.argv[6]).resolve(strict=True) if len(sys.argv)>=7 else None
umd_probes=Path(sys.argv[7]).resolve(strict=True) if len(sys.argv)>=8 else None
out=Path(sys.argv[4]).resolve()
out.mkdir(parents=True,exist_ok=False)
work=Path(os.environ['OSITO_TEST_DISK_DIR']).resolve(strict=True)
import tempfile,shutil
control_dir=tempfile.TemporaryDirectory(prefix='osito-native-dx-',dir='/tmp')
control=Path(control_dir.name)
command=['qemu-system-x86_64','-name','osito-native-dx-loader','-machine','q35',
    '-accel','kvm','-cpu','host','-m','2G','-smp','4',
    '-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
    '-drive',f'if=pflash,format=raw,file={work/"vars.fd"}',
    '-drive',f'format=raw,if=ide,file={work/"esp.img"}',
    '-drive',f'format=raw,if=none,id=data,file={work/"ssh-test.img"}',
    '-device','nvme,serial=osito-native-dx,drive=data',
    '-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0,disable-legacy=on',
    '-display','none','-vga','std','-monitor','none',
    '-chardev',f'socket,id=uart,path={control/"serial.sock"},server=on,wait=off,logfile={out/"serial.log"}',
    '-serial','chardev:uart','-qmp',f'unix:{control/"qmp.sock"},server=on,wait=off',
    '-no-reboot','-no-shutdown']

subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(work/'client')],check=True)
subprocess.run(['truncate','-s','64M',str(work/'esp.img')],check=True)
subprocess.run(['mformat','-i',str(work/'esp.img'),'-F','::'],check=True)
subprocess.run(['mmd','-i',str(work/'esp.img'),'::/EFI','::/EFI/BOOT'],check=True)
subprocess.run(['mcopy','-i',str(work/'esp.img'),str(boot),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
subprocess.run(['cp','/usr/share/OVMF/OVMF_VARS_4M.fd',str(work/'vars.fd')],check=True)
subprocess.run(['truncate','-s','96M',str(work/'ssh-test.img')],check=True)
subprocess.run(['tools/ositofs/mkfs.ositofs3',str(work/'ssh-test.img'),'--label','SSHTest','--inodes','4096'],check=True)
(work/'noautoload').touch()
for name,dest in [('noautoload','/.noautoload'),('client.pub','/.ssh_authorized_keys')]:
    subprocess.run(['tools/ositofs/ositofs-write3',str(work/'ssh-test.img'),str(work/name),'--dest',dest],check=True)
subprocess.run(['tools/ositofs/ositofs-write3',str(work/'ssh-test.img'),str(kernel),
                '--dest','/kernel.elf'],check=True)
subprocess.run(['mcopy','-o','-i',str(work/'esp.img'),str(kernel),'::/EFI/BOOT/kernel.elf'],check=True)
for bits in (32,64):
    source=fixtures/f'pe{bits}'/f'dxloader{bits}-absent.exe'
    subprocess.run(['tools/ositofs/ositofs-write3',str(work/'ssh-test.img'),str(source),'--dest',f'/absent{bits}.exe'],check=True)
    if kmt_probes:
        source=kmt_probes/f'pe{bits}'/f'dxkmt{bits}-empty.exe'
        subprocess.run(['tools/ositofs/ositofs-write3',str(work/'ssh-test.img'),str(source),'--dest',f'/kmt{bits}.exe'],check=True)
    if cancel_probes:
        source=cancel_probes/f'pe{bits}'/f'cancelstack{bits}.exe'
        subprocess.run(['tools/ositofs/ositofs-write3',str(work/'ssh-test.img'),str(source),'--dest',f'/cancel{bits}.exe'],check=True)
shutil.copyfile(work/'client',control/'client')
os.chmod(control/'client',0o600)
with socket.socket() as network:
    network.bind(('127.0.0.1',0)); port=network.getsockname()[1]
command[command.index('-netdev')+1] = f'user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22'
(out/'command.json').write_text(json.dumps(command,indent=2)+'\n')
serial = out/'serial.log'
stop = threading.Event()
uart = None
reader = None
with (out/'qemu.log').open('wb') as log:
    process = subprocess.Popen(command,stdout=log,stderr=log)

def prompt(start=0, timeout=150):
    end = time.monotonic()+timeout
    while time.monotonic()<end:
        assert process.poll() is None, 'QEMU stopped'
        data = serial.read_bytes()[start:] if serial.exists() else b''
        assert b'!!! EXCEPTION' not in data and b'SYSTEM HALTED' not in data, data[-1500:]
        if b'osito> ' in data: return data
        time.sleep(.1)
    raise TimeoutError('no shell prompt')

def drain_uart():
    while not stop.is_set():
        try:
            if not uart.recv(65536): return
        except socket.timeout: pass
        except OSError: return

def local(text):
    start = serial.stat().st_size
    for byte in (text+'\r').encode():
        uart.sendall(bytes([byte])); time.sleep(.03)
    return prompt(start)

def remote(text):
    arguments = ['ssh','-F','/dev/null','-T','-p',str(port),'-i',str(control/'client'),
        '-oIdentitiesOnly=yes','-oBatchMode=yes','-oConnectTimeout=5',
        '-oStrictHostKeyChecking=accept-new',f'-oUserKnownHostsFile={out/"known_hosts"}',
        '-oHostKeyAlias=msgwait-qemu','osito@127.0.0.1',text]
    result = subprocess.run(arguments,capture_output=True,timeout=30)
    assert result.returncode == 0, (text,result.returncode,result.stderr[-300:])
    return result.stdout

records={'kernel_sha256':hashlib.sha256(kernel.read_bytes()).hexdigest(),'completed':False,'tests':{}}
try:
    prompt()
    uart=socket.socket(socket.AF_UNIX);uart.settimeout(.2);uart.connect(str(control/'serial.sock'))
    reader=threading.Thread(target=drain_uart);reader.start()
    (out/'sshd-start.log').write_bytes(local('sshd start'))

    (out/'strict.log').write_bytes(local('win32-loader strict on'))
    data=local('gpu-worker-test')
    (out/'gpu-worker-unavailable.log').write_bytes(data)
    passed=b'GPU worker test: unavailable' in data and b'GPU worker test: FAIL' not in data
    records['tests']['gpu-worker-unavailable']={'passed':passed}
    print('gpu-worker-unavailable',passed,flush=True)
    assert passed,data[-2000:]
    data=local('gpu-render-test')
    (out/'gpu-render-unavailable.log').write_bytes(data)
    passed=b'GPU render test: unavailable' in data and b'GPU render test: FAIL' not in data
    records['tests']['gpu-render-unavailable']={'passed':passed}
    print('gpu-render-unavailable',passed,flush=True)
    assert passed,data[-2000:]
    # Check the actual remote-console admission path without Intel hardware.
    for action,expected in [('on',0xc00000bb),('status',0),('off',0)]:
        start=serial.stat().st_size
        launch=remote('console launch gpu-native '+action)
        request=re.search(rb'queued request (\d+)',launch)
        assert request,launch
        data=prompt(start,timeout=30)
        status=remote('console status')
        assert b'console: idle' in status and b'last request: '+request[1]+b' returned' in status,status
        (out/f'gpu-native-{action}.log').write_bytes(launch+data+status)
        passed=f'Native GPU status={expected} phase=0 adapter=0 engine=0'.encode() in data
        records['tests'][f'gpu-native-{action}']={'passed':passed}
        print('gpu-native',action,passed,flush=True)
        assert passed,data[-2000:]
    # Admission before and after successive PE32/PE64 root launches. The
    # kernel test uses real NT objects and PMM RAM, without a render adapter.
    start=serial.stat().st_size
    launch=remote('console launch win32-vm-test')
    (out/'driver-vm-before-launch.log').write_bytes(launch)
    request=re.search(rb'queued request (\d+)',launch)
    assert request,launch
    data=prompt(start,timeout=30)
    status=remote('console status')
    (out/'driver-vm-before-status.log').write_bytes(status)
    assert b'console: idle' in status and b'last request: '+request[1]+b' returned' in status,status
    (out/'driver-vm-before.log').write_bytes(data)
    passed=(b'NT virtual-memory test: PASS' in data and
        re.search(rb'\[DRIVERVM\] checks=\d+ failures=0',data) is not None)
    records['tests']['driver-vm-before']={'passed':passed}
    print('driver-vm','before',passed,flush=True)
    assert passed,data[-5000:]
    phase='before'
    data=local('win32-process-test')
    (out/f'gpu-process-{phase}.log').write_bytes(data)
    passed=(b'Win32 process identity test: PASS' in data and
        re.search(rb'\[GPUVIEW\] checks=[1-9]\d+ failures=0',data) is not None and
        re.search(rb'\[GPUPROC\] checks=\d+ failures=0',data) is not None and
        re.search(rb'\[GPUPROC\] admission_probe_cpu=[1-9]\d*',data) is not None)
    records['tests'][f'gpu-process-{phase}']={'passed':passed}
    print('gpu-process',phase,passed,flush=True)
    assert passed,data[-4000:]
    data=local('sched-signal-test')
    (out/'gpu-signal-before.log').write_bytes(data)
    passed=(b'GPU scheduler signal test: PASS' in data and
        re.search(rb'\[GPUSIGNAL\] checks=\d+ failures=0 rounds=16 ap_cpu=[1-9]\d* waiters=0',data) is not None)
    records['tests']['gpu-signal-before']={'passed':passed}
    print('gpu-signal','before',passed,flush=True)
    assert passed,data[-5000:]
    if kmt_probes:
        for bits in (32,64):
            data=local(f'winexec /kmt{bits}.exe')
            (out/f'kmt{bits}.log').write_bytes(data)
            passed=(b'KMT DONE failures=00000000' in data and
                    b'KMT FAIL' not in data and b'KMT UNAVAILABLE' not in data)
            probe=kmt_probes/f'pe{bits}'/f'dxkmt{bits}-empty.exe'
            records['tests'][f'kmt-empty-{bits}']={'passed':passed,
                'sha256':hashlib.sha256(probe.read_bytes()).hexdigest()}
            print('kmt-empty',bits,passed,flush=True)
            assert passed,data[-2000:]
    if cancel_probes:
        for bits in (32,64):
            data=local(f'winexec /cancel{bits}.exe')
            (out/f'cancel{bits}.log').write_bytes(data)
            passed=(b'CANCEL-STACK PASS' in data and data.count(b'CANCEL-STACK ROUND PASS')==3 and
                    b'CANCEL-STACK FAIL' not in data)
            records['tests'][f'cancel-stack-{bits}']={'passed':passed,
                'sha256':hashlib.sha256((cancel_probes/f'pe{bits}'/f'cancelstack{bits}.exe').read_bytes()).hexdigest()}
            print('cancel-stack',bits,passed,flush=True)
            assert passed,data[-3000:]
    for bits in (32,64):
        data=local(f'winexec /absent{bits}.exe')
        (out/f'absent{bits}.log').write_bytes(data)
        passed=b'DXLOADER DONE failures=00000000' in data and b'DXLOADER FAIL' not in data
        records['tests'][f'absent{bits}']={'passed':passed}
        print('absent',bits,passed,flush=True)
        assert passed,data[-1800:]

    import importlib.util
    spec=importlib.util.spec_from_file_location('fixture_sync','tools/osito-sync.py');sync=importlib.util.module_from_spec(spec);spec.loader.exec_module(sync)
    ssh=['ssh','-F','/dev/null','-T','-p',str(port),'-i',str(control/'client'),'-oIdentitiesOnly=yes','-oBatchMode=yes','-oStrictHostKeyChecking=accept-new',f'-oUserKnownHostsFile={out/"known_hosts"}','-oHostKeyAlias=msgwait-qemu','-s','osito@127.0.0.1','osito-sync-v1']
    c=sync.Client(ssh)
    try:
        transfer=sync.Sync(c,quiet=True)
        for bits in (32,64):
            origin=fixtures/f'pe{bits}'
            for kind in ('fixture','static-d3d9','static-dxgi','incomplete'):
                destination='Incomplete' if kind=='incomplete' else 'Fixtures'
                transfer.push(origin/f'dxloader{bits}-{kind}.exe',f'/{destination}{bits}/{kind}.exe')
            for folder,guest in [('fixtures','Fixtures'),('incomplete','Incomplete')]:
                for name in ('d3d9.dll','dxgi.dll'):
                    transfer.push(origin/folder/name,f'/{guest}{bits}/{name}')
            if umd_probes:
                for name in (f'umd{bits}-model.exe',f'umd{bits}-rcs-model.exe','osito-intel9.dll'):
                    transfer.push(umd_probes/f'pe{bits}'/name,f'/UMD{bits}/{name}')
    finally:c.close()
    if umd_probes:
        for bits in (32,64):
            for mode,node in (('model',1),('rcs-model',0)):
                data=local(f'winexec /UMD{bits}/umd{bits}-{mode}.exe')
                (out/f'umd{bits}-{mode}.log').write_bytes(data)
                passed=(b'UMD failures=00000000' in data and b'UMD copies=00000003' in data and
                    f'UMD node={node:08x}'.encode() in data and b'UMD FAIL' not in data)
                records['tests'][f'umd-{mode}-{bits}']={'passed':passed,'gpu_execution':False,
                    'dll_sha256':hashlib.sha256((umd_probes/f'pe{bits}/osito-intel9.dll').read_bytes()).hexdigest(),
                    'probe_sha256':hashlib.sha256((umd_probes/f'pe{bits}/umd{bits}-{mode}.exe').read_bytes()).hexdigest()}
                print('umd',mode,bits,passed,flush=True)
                assert passed,data[-2000:]
    oracle=json.loads((fixtures/'native.json').read_text()) if (fixtures/'native.json').exists() else None
    records['native_oracle_verified']=oracle is not None
    for bits in (32,64):
        for kind in ('fixture','static-d3d9','static-dxgi','incomplete'):
            fixture=fixtures/f'pe{bits}'/f'dxloader{bits}-{kind}.exe'
            key=f'{bits}-{kind}'
            if oracle:
                assert oracle[key]['passed'] and oracle[key]['sha256']==hashlib.sha256(fixture.read_bytes()).hexdigest()
            destination='Incomplete' if kind=='incomplete' else 'Fixtures'
            data=local(f'winexec /{destination}{bits}/{kind}.exe')
            (out/f'{kind}{bits}.log').write_bytes(data)
            passed=b'DXLOADER DONE failures=00000000' in data and b'DXLOADER FAIL' not in data
            records['tests'][key]={'passed':passed}
            print(kind,bits,passed,flush=True)
            assert passed,data[-1800:]
    start=serial.stat().st_size
    launch=remote('console launch win32-process-test')
    (out/'gpu-process-launch.log').write_bytes(launch)
    assert b'console: queued request ' in launch,launch
    data=prompt(start,timeout=30)
    status=remote('console status')
    (out/'gpu-process-status.log').write_bytes(status)
    assert b'console: idle' in status and b' returned' in status,status
    (out/'gpu-process-after.log').write_bytes(data)
    passed=(b'Win32 process identity test: PASS' in data and
        re.search(rb'\[GPUVIEW\] checks=\d+ failures=0',data) is not None and
        re.search(rb'\[GPUPROC\] checks=\d+ failures=0',data) is not None and
        re.search(rb'\[GPUPROC\] admission_probe_cpu=[1-9]\d*',data) is not None)
    records['tests']['gpu-process-after']={'passed':passed}
    print('gpu-process','after',passed,flush=True)
    assert passed,data[-4000:]
    start=serial.stat().st_size
    launch=remote('console launch sched-signal-test')
    (out/'gpu-signal-launch.log').write_bytes(launch)
    assert b'console: queued request ' in launch,launch
    data=prompt(start,timeout=30)
    status=remote('console status')
    (out/'gpu-signal-status.log').write_bytes(status)
    assert b'console: idle' in status and b' returned' in status,status
    (out/'gpu-signal-after.log').write_bytes(data)
    passed=(b'GPU scheduler signal test: PASS' in data and
        re.search(rb'\[GPUSIGNAL\] checks=\d+ failures=0 rounds=16 ap_cpu=[1-9]\d* waiters=0',data) is not None)
    records['tests']['gpu-signal-after']={'passed':passed}
    print('gpu-signal','after',passed,flush=True)
    assert passed,data[-5000:]
    data=local('win32-vm-test')
    (out/'driver-vm-after.log').write_bytes(data)
    passed=(b'NT virtual-memory test: PASS' in data and
        re.search(rb'\[DRIVERVM\] checks=\d+ failures=0',data) is not None)
    records['tests']['driver-vm-after']={'passed':passed}
    print('driver-vm','after',passed,flush=True)
    assert passed,data[-5000:]
    (out/'ssh-final.log').write_bytes(remote('uname'))
    (out/'import-diagnostics.log').write_bytes(local('win32-loader'))
    data=serial.read_bytes()
    records.update(exceptions=data.count(b'!!! EXCEPTION'),assertions=data.count(b'assert failed:'),microsoft_binaries_in_guest=False,renderer_substitute_installed=False)
    records['passed']=all(x['passed'] for x in records['tests'].values()) and not records['exceptions'] and not records['assertions']
    (out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    assert records['passed'],records
    records['completed']=True

finally:
    (out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    stop.set()
    if reader:reader.join(timeout=2)
    if uart:uart.close()
    if process.poll() is None:process.terminate()
    try:process.wait(timeout=10)
    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)

control_dir.cleanup()
