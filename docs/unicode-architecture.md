# Arquitectura Unicode de OsitoK

> **Estado**: diseño formalizado y validado end-to-end en el fork
> experimental `osito-x` (branch `experiment-x`, commits `c801064` →
> `195503b`).  Pendiente de port a la mainline.  Este documento
> describe la arquitectura completa para futuras sesiones de mainline.

## Por qué esto es un problema arquitectural, no cosmético

OsitoK es un OS bare-metal pensado para correr inferencia LLM y
videojuegos.  Los modelos modernos están entrenados en datasets
multilingües (chino, ruso, japonés, árabe...), y los videojuegos
sobre Win32/Wine producen strings en cualquier idioma.  Renderear `?`
en lugar del codepoint real significa que el OS pierde información
visible al usuario.

La pregunta de fondo: **¿debe el kernel saber Unicode, o debe
delegar?**.  Plan 9, Linux console y NetBSD wsdisplay convergieron
todos en una respuesta de cuatro capas que combina lo mínimo en el
kernel con datos cargables desde FS.  OsitoK adopta el mismo modelo.

## Las cuatro capas (más una capa cero)

```
┌── Capa 0 — Boot font (link-time, ~3 KB) ──────────────────────────┐
│  ASCII 32-126   →  gui_font8x16        (95 glifos × 16 B = 1.5 KB) │
│  Latin-1 160-255 →  gui_latin1_glyphs  (96 glifos × 16 B = 1.5 KB) │
│  Garantía: español/francés/alemán SIEMPRE legible, sin FS.         │
└────────────────────────────────────────────────────────────────────┘
                            ▲ fallback
┌── Capa 1 — Codec UTF-8 (link-time, ~200 LOC) ─────────────────────┐
│  libutf de Plan 9 — Pike & Thompson, dominio público.              │
│  Tipo Rune = uint32_t, Runeerror=0xFFFD, Runemax=0x10FFFF.         │
│  API: chartorune, runetochar, runelen, fullrune, utflen,           │
│        utfnlen, utfrune, utfrrune, utfecpy, rune_width (UAX #11).  │
└────────────────────────────────────────────────────────────────────┘
                            ▼ decoder
┌── Capa 2-3 — Glyph data + Cache (FS-loaded, lazy) ────────────────┐
│  Formato:  PSF2 (consola Linux estándar).                          │
│  Origen:   GNU Unifont 15 (.hex, public domain) → tools/font/      │
│              unifont2psf.py emite tres tiers en /fonts/:           │
│  Tiers:                                                            │
│    basic.psf  ~32 KB  Latin/Cyrillic/Greek/Box/Symbols (1964 g.)   │
│    cjk.psf   ~590 KB  CJK Unif. + Hiragana + Hangul   (18430 g.)   │
│    full.psf ~1.4 MB  full BMP CJK + extensiones      (40667 g.)    │
│  In-memory: subfont_t por archivo, runs[] ordenados (binary search │
│  O(log n)), bitmap blob.  LRU hooks (last_used_tick, hits, misses, │
│  ram_bytes, ram_budget) listos para evicción cuando se exceda      │
│  budget — no-op en v1 porque basic+cjk caben holgadamente.         │
└────────────────────────────────────────────────────────────────────┘
                            ▼ glyph
┌── Capa 4 — Renderer (width-aware) ────────────────────────────────┐
│  fb_render_rune (framebuffer console) y gui_draw_text (GUI)        │
│  consultan via gui_font_lookup_ext, soportan glifos de cualquier   │
│  ancho.  CJK 16×16 avanza 2 cells (wcwidth==2 implícito por gw).   │
│  Cadena de fallback explícita en cada renderer:                    │
│    1. system_font (FS-loaded)                                      │
│    2. ASCII embebido (cp ∈ [32, 126])                              │
│    3. Latin-1 embebido (cp ∈ [160, 255])                           │
│    4. tofu '?' (cualquier otro)                                    │
└────────────────────────────────────────────────────────────────────┘
```

### Por qué este diseño y no embed-todo

La alternativa "embebido" (compilar todos los glifos en kernel.elf
como arrays C) es atajo:

- Acopla el binario del kernel a una decisión de fuente.  Cambiar
  Unifont por Misaki Gothic = recompilar kernel.
- Hardcodea cobertura de scripts en build-time.  Agregar tibetano
  post-deploy = imposible sin recompilar.
- Mezcla **datos** (glifos) con **código** (kernel) — dos cosas con
  ciclos de vida muy distintos.

El diseño 4-capas separa concerns:

- Capa 0 (link-time): mínimo absoluto para arrancar y mostrar errores
  legibles antes de montar FS.  Solo ASCII + Latin-1 = ~3 KB.
- Capas 2-3 (FS-loaded): cualquier cobertura adicional vive como
  archivo en `/fonts/`.  El usuario suelta `cjk.psf` y el sistema lo
  consume; lo borra y vuelve al boot font.

Costo total para inferencia AI multilingüe usable:
**~225 KB de fonts en disco** (basic + cjk común).  Eso desbloquea
español, ruso, griego, chino simplificado, japonés, coreano sin
tocar el kernel.

### Por qué Plan 9 y no Linux console

Linux console funciona bien pero tiene legacy: Unicode mapping vía
ioctl `KDFONTOP`, console map en kernel, PSF1+PSF2 ambos soportados.
Plan 9 es más limpio: kernel pasa bytes UTF-8 transparente,
decodificación solo para render o comparación de paths.  Mismo
diseño que OsitoK adoptó para `serial.c` y todos los TTY.

PSF2 lo escogimos porque es el formato de hecho en Linux console y
hay tooling abundante para producirlo desde cualquier BDF (por
ejemplo `bdf2psf` de Debian).  Plan 9 usa `.subf` propietario que
nadie más entiende.

## Layout en el árbol fuente

```
include/common/
  utf8.h                   API libutf, cross-arch (x86 + arm)
  font.h                   API Font/Subfont, cross-arch

arch/x86/lib/utf/
  rune.c                   libutf impl (~225 LOC, freestanding)

arch/x86/kernel/
  font.c                   Font/Subfont + PSF2 loader + LRU + hook
  framebuffer.c            fb_render_rune (Capa 4 console)

gui/
  gui.h                    extern gui_font8x16, gui_latin1_glyphs
  gui_text.c               Boot fonts + gui_draw_text (Capa 4 GUI)

tools/font/
  unifont-15.1.04.hex.gz   Source data (gzip, ~900 KB)
  unifont2psf.py           Generador PSF2 desde unifont.hex

tools/
  build-fonts-img.sh       Empaqueta /fonts/*.psf en nvme.img
```

## Reglas de oro

1. **El kernel pasa bytes UTF-8 transparente.**  Solo decodifica
   para rendering local, contar columnas, o comparar paths case-
   insensitive.  Pipes, sockets, FS son byte-clean.

2. **`char *` siempre es bytes.**  `Rune` (uint32_t) solo aparece
   donde importa el codepoint — renderer, EastAsianWidth, regex.

3. **Secuencias UTF-8 inválidas → 0xFFFD, NUNCA crash.**  El
   consumidor decide si pinta tofu o ignora.

4. **No embebir font data más allá de la Capa 0.**  Latin-1 es el
   límite — todo lo demás vive en `/fonts/*.psf`.

5. **Boot funciona sin /fonts/.**  Si OsitoFS no monta o no tiene
   los .psf, el kernel rendea ASCII + Latin-1 con tofu para el resto.
   El usuario nota el degradation pero el sistema arranca.

## Lo que NO hace OsitoK (y por qué)

- **Shaping de árabe / índico / RTL bidi.**  Plan 9 también lo
  punta.  Para árabe legible: usar Arabic Presentation Forms-B
  (U+FE70-FEFF) pre-shaped — drop una `arabic.psf` con esos
  codepoints y se pinta sin shaping en runtime.  Bidi pleno (UAX
  #9) son ~1500 LOC; queda como follow-up si surge un caso real.

- **Combining marks compuestos.**  Plan 9 los pinta como cells
  separadas (visualmente incorrecto, leíble).  Llevamos el mismo
  comportamiento.

- **East Asian Width auto-detección por terminal capability.**  La
  tabla UAX #11 está hardcoded en `rune_width()` (libutf).  Útil
  para `utfnlen` cuando se cuentan columnas en terminal.

## Dónde está la implementación

Todo el código vive en `osito-x` (fork experimental), branch
`experiment-x`:

| Capa | Commit       | Contenido                                          |
|------|--------------|----------------------------------------------------|
| 1    | `c801064`    | libutf (utf8.h + rune.c) — 442 LOC                 |
| 2    | `aa83f00`    | Migración gui_text + framebuffer a chartorune      |
| 3    | `65946a9`    | font subsystem + PSF2 parser + LRU hooks           |
| 3.5  | `dc2265e`    | NVMe DMA fix (4KB-aligned bounce buffer)           |
| 4    | `52b7f97`    | Renderer width-aware + CJK 16×16                   |
| 5    | `85dc498`    | framebuffer rendea Unicode completo via subsystem  |
| 0    | `195503b`    | Latin-1 boot fallback link-time                    |
| —    | `122c017`    | Cleanup: deuda fantasma de "system_font corrupt"   |
| —    | `2440c83`    | tools/build-gsp-img.sh (firmware GSP en OsitoFS)   |
| —    | `b7009ed`    | smoke-test attach nvme.img si existe (opt-in)      |

Plus tooling:

- `tools/font/unifont2psf.py` — generador PSF2 desde Unifont .hex
- `tools/font/unifont-15.1.04.hex.gz` — source data (12 MB extraido)
- `tools/build-fonts-img.sh` — empaqueta tiers en nvme.img

## Cómo portar a mainline

Cherry-pick los 9 commits anteriores en orden + cherry-pick los dos
de tooling.  El único conflicto plausible es `arch/x86/Makefile`
(adición del target `lib/utf/rune.o`) — resolución obvia.

Cherry-pick:

```bash
git cherry-pick c801064 aa83f00 dc2265e 65946a9 52b7f97 85dc498 \
                195503b 122c017 2440c83 b7009ed
```

Después regenerar imagen y validar:

```bash
make -C arch/x86
tools/build-fonts-img.sh basic cjk
tools/smoke-screenshot.sh
# Verificar visualmente: español + русский + ελληνικά + 中文 日本
```

## Referencias

- Pike, R. & Thompson, K.  *Hello World or Καλημέρα κόσμε or
  こんにちは 世界*.  USENIX Winter 1993.  Origen de UTF-8 + libutf.
- `plan9-foundation/plan9` GitHub mirror, `src/lib9/utf/` — código
  base de libutf, dominio público.
- Linux console PSF format spec:
  `https://www.win.tue.nl/~aeb/linux/kbd/font-formats-1.html`
- GNU Unifont: `https://unifoundry.com/unifont/` (public domain
  CJK glyphs vía Wen Quan Yi GB19966-2005).
- Unicode Standard Annex #11 (East Asian Width) — usado para
  `rune_width()` en libutf.
