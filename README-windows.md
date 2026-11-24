# OsitoK en Windows — arranque rápido

Guía para compilar y correr el kernel bare-metal x86-64 de OsitoK en la máquina
de desarrollo Windows (repo en `D:\projects\osito-k`, msys2 en `C:\msys64`).

| Tarea | Dónde |
|-------|-------|
| Build x86 (`make -C arch/x86 CLANG=1`) | Acá, en msys2/mingw64 |
| Boot en QEMU | Acá, con TCG (sin aceleración por hardware) |
| Tests de host (`arch/x86/scripts/test-*.sh`) | WSL de kerberus, vía `arch/x86/scripts/remote-test.sh` |
| QEMU con KVM | Ninguna máquina lo tiene instalado hoy (ver `docs/windows-qemu-whpx.md`) |

---

## 1. Dependencias

```powershell
.\build-windows.ps1 -Check     # qué falta
.\build-windows.ps1 -Install   # pacman: make, mtools, qemu, clang, lld, llvm-tools
```

No hace falta un cross-gcc `x86_64-elf`: el build usa `clang --target=x86_64-elf`
+ `ld.lld`. Lo único que no está en pacman es **gnu-efi** (para `boot.efi`):
compilar https://github.com/ncroxon/gnu-efi e instalarlo bajo `/usr/local` de
msys2 (`C:\msys64\usr\local`). `arch/x86/Makefile` lo detecta ahí; si no, pasar
`EFI_PREFIX=<ruta>` a make.

---

## 2. Build + boot

Desde PowerShell, parado en la raíz del repo:

```powershell
.\build-windows.ps1                          # build + QEMU con ventana gtk
.\build-windows.ps1 -Display none            # headless: todo va al serial log
.\build-windows.ps1 -Nvme .\nvme_ut99.img    # adjunta la imagen de UT99
.\build-windows.ps1 -BuildOnly               # solo compilar
.\build-windows.ps1 -NoBuild                 # bootear el kernel.elf ya compilado
```

O desde un shell **MSYS2 MINGW64**, con el runner directo:

```bash
OK_BUILD=1 arch/x86/scripts/run-windows.sh               # build + boot headless
OK_DISPLAY=gtk OK_NVME=nvme_ut99.img arch/x86/scripts/run-windows.sh
```

`run-windows.sh` acepta además `OK_MEM` (default 2048M), `OK_CPU` (default
`max`), `OK_MONITOR=1` (monitor QEMU en `127.0.0.1:55555`) y `OK_PCAP=1`
(captura de red en `build/net.pcap`).

- **Serial log**: `arch\x86\build\serial.log`. Sin NVMe, TCG llega al prompt
  `osito>` en menos de un minuto.
- QEMU: `q35`, 4 vCPU (TCG multi-thread), OVMF de edk2 (del qemu de mingw) por
  `-bios`, virtio-net con `hostfwd udp:7778->7777` y `tcp:50052->50052`, xHCI
  con teclado y tablet USB.
- La imagen NVMe se monta **read-write**: el guest puede escribirla. Trabajar
  sobre una copia si importa conservarla.

---

## 3. Por qué no hay aceleración

WHPX (Windows Hypervisor Platform) no puede bootear OVMF: rechaza un `WRMSR`
temprano en `PlatformPei` e inyecta un `#GP` antes de llegar a `boot.efi`. Es
una limitación de QEMU sin workaround por línea de comandos. Detalle y plan de
patch en `docs/windows-qemu-whpx.md`.

Para velocidad de TCG en runs largos (UT99/Steam) conviene compilar con
`QUIET=1` (menos serial, que bajo emulación es caro) y `SKIP_MODEL=1` (no
carga el modelo GGUF al boot).

---

## 4. Tests de host en kerberus

Los tests de host compilan con gcc y clang de Linux, casi siempre con
ASan/UBSan, cosa que msys2 no da. Corren en la WSL (Ubuntu-24.04) de
`xasko@kerberus`, por SSH con clave:

```bash
arch/x86/scripts/remote-test.sh boot-diag gdi-screen      # sincroniza y corre test-boot-diag.sh + test-gdi-screen.sh
arch/x86/scripts/remote-test.sh dos-fpu-compare -- /tmp/fpu.log   # argumentos para un test
arch/x86/scripts/remote-test.sh --exec 'make -C arch/x86 serial-host-test'
arch/x86/scripts/remote-test.sh --no-sync boot-diag       # re-run sin sincronizar
arch/x86/scripts/remote-test.sh --full --sync             # borra la copia remota y la manda entera
```

- Sincroniza `arch/x86`, `include/common`, `gui` y `tools/ositofs` a
  `~/osito-k` en la WSL remota. Después de la primera vez manda solo lo
  modificado; los borrados locales no se propagan (usar `--full`).
- Paquetes que hoy faltan en kerberus (los tiene que instalar alguien con sudo):
  `sudo apt install -y libmpfr-dev libgmp-dev lld llvm`. MPFR/GMP los usan los
  tests `dos-fpu-*`; `lld` y `llvm-dlltool` los tests que arman binarios PE.
- Algunos tests esperan fixtures pregenerados en `arch/x86/build/` (por ejemplo
  `test-intel-d3d9-shader.sh`), que no se sincronizan.
- El shell SSH de kerberus es PowerShell 5.1. Para comandos a mano, desde Git
  Bash: `ssh xasko@kerberus "wsl -d Ubuntu-24.04 -e bash -lc '<cmd>'"`, sin
  comillas simples ni dobles dentro de `<cmd>`. Para cualquier cosa más
  compleja, `remote-test.sh --exec`.

---

## 5. Gotchas

- **Symlinks creados desde WSL**: el `clang.exe` nativo no los puede abrir.
  Pasó con `arch/x86/lib/tinyssh/upstream/*_optblocker.c`, que se reemplazaron
  por copias del archivo destino.
- **Finales de línea**: `.gitattributes` fuerza LF. Un editor que guarde CRLF
  genera diffs de archivo entero.
- **Memorias de Claude**: las de la etapa UT99 quedaron bajo el slug viejo
  `C:\Users\xasko\.claude\projects\C--Users-xasko-osito-k\memory\`. Abriendo el
  repo desde `D:\projects\osito-k` no se cargan solas.
