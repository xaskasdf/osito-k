# Guía de Rendering para Juegos Portados a OsitoK

## Problema: Dos rutas de rendering que compiten

El kernel tiene DOS caminos para mostrar frames de un juego fullscreen:

### Ruta A: `shm_flush_surface()` (ROTA — no usar)
```
Game → SYS_GUI_FLIP(507) → shm_flush_surface() → blit DIRECTO al GOP framebuffer
```
- **Hardcoded 320×200** como resolución fuente (shm.c línea 277)
- Escribe directamente al GOP FB, **bypassing el compositor**
- Sin double buffering → **tearing**
- Sin VBlank sync
- Si el juego NO es 320×200 → **solo se ve la mitad**

### Ruta B: Compositor fullscreen (CORRECTA)
```
Game → escribe a SHM surface → compositor detecta WND_FULLSCREEN
    → compositor_render_frame() lee SHM → escala → back buffer → display_flip()
```
- Soporta **cualquier resolución** (320×200, 640×480, 800×600, etc.)
- **Double buffer** con VBlank sync
- **Pixel-perfect integer scaling** con letterboxing
- Sin tearing

## Solución: Cómo debe renderizar un juego

### 1. Crear surface (una vez al inicio)

```c
// Llamar desde el juego:
int width = 800;   // resolución del juego
int height = 600;
int flags = 4;     // SHM_FLAG_GPU_SCANOUT → fullscreen

// Syscall 506: SYS_SHM_MKSURFACE
uint32_t shm_handle = syscall(506, width, height, flags);

// Syscall 501: SYS_SHM_MAP → obtener puntero a pixels
uint32_t *pixels = (uint32_t *)syscall(501, shm_handle);
```

Esto automáticamente:
- Crea un buffer de `width × height × 4` bytes
- Registra una ventana en el compositor (`compositor_create_window`)
- La marca como fullscreen (`compositor_set_fullscreen`)
- El compositor entra en "game mode" (keyboard bypass)

### 2. Renderizar cada frame

```c
// Dibujar directamente en el buffer de pixels
for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
        pixels[y * width + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}
```

El formato es **ARGB8888** (alpha en byte alto, siempre 0xFF para opaco).

### 3. Señalar frame completo

**NO usar SYS_GUI_FLIP (507).** En su lugar, simplemente no hacer nada extra.

El compositor lee el SHM surface cada frame (60fps) y lo blitea automáticamente.
Si quieres forzar un redraw inmediato:

```c
// Opcional: no es necesario si el compositor ya corre a 60fps
// El compositor detecta WND_DIRTY automáticamente
```

### 4. Leer input

```c
// Estructura de 24 bytes
struct input_event {
    uint8_t  type;       // 1=KEY_DOWN, 2=KEY_UP, 3=MOUSE_MOVE, 4=MOUSE_BTN
    uint8_t  scancode;   // HID keycode (0x04='a', 0x1E='1', etc.)
    uint8_t  buttons;    // Mouse buttons (bit 0=left, 1=right, 2=middle)
    uint8_t  flags;      // Modifiers (bit 0=shift, 1=ctrl, 2=alt)
    int16_t  dx, dy;     // Mouse delta (for MOUSE_MOVE)
    int16_t  wheel;      // Mouse wheel delta
    uint16_t _pad;
    uint64_t timestamp;  // TSC timestamp
};

struct input_event evt;
while (syscall(512, &evt) == 1) {
    // Procesar evento
    if (evt.type == 1) {  // KEY_DOWN
        handle_key(evt.scancode);
    }
    if (evt.type == 3) {  // MOUSE_MOVE
        handle_mouse(evt.dx, evt.dy);
    }
}
```

### 5. Cleanup al salir

No se necesita cleanup explícito. `compositor_cleanup_process(pid)` se llama automáticamente al terminar el proceso, destruyendo la ventana y liberando el SHM.

---

## Flujo correcto completo

```
┌─────────────────────────────────────────────────────┐
│ GAME PROCESS                                        │
│                                                     │
│ 1. shm_handle = syscall(506, W, H, 4)  ← INIT      │
│ 2. pixels = syscall(501, shm_handle)                │
│                                                     │
│ GAME LOOP:                                          │
│ 3. while (syscall(512, &evt)) handle_input(evt)     │
│ 4. render_frame(pixels, W, H)                       │
│ 5. goto 3  (no flip syscall needed!)                │
└─────────────────────────────────────────────────────┘
         │ SHM buffer (shared memory)
         ▼
┌─────────────────────────────────────────────────────┐
│ COMPOSITOR THREAD (60fps)                           │
│                                                     │
│ 1. xhci_poll() — USB input                          │
│ 2. input_drain_coalesced() — game_mode=true         │
│ 3. compositor_render_frame():                       │
│    - Detecta WND_FULLSCREEN                         │
│    - Lee pixels del SHM                             │
│    - Calcula escala: min(screen_w/W, screen_h/H)    │
│    - Pixel-perfect upscale al back buffer           │
│    - Letterboxing (bordes negros si aspect mismatch)│
│ 4. display_flip() — VBlank sync                     │
└─────────────────────────────────────────────────────┘
```

## Resolución del bug "solo mitad visible"

Si el juego usa 640×480 pero `shm_flush_surface()` asume 320×200:
- Solo se leen 320×200 pixels de un buffer de 640×480
- Se muestran en la mitad superior (200 filas de 768)
- Las 568 filas restantes son basura o el desktop

**Fix:** No usar `SYS_GUI_FLIP(507)`. El compositor maneja la escala correctamente para CUALQUIER resolución.

## Resolución del tearing

`shm_flush_surface()` escribe al GOP FB sin VBlank sync.
El compositor usa `display_flip()` que espera VBlank → sin tearing.

**Fix:** Dejar que el compositor maneje el flip. No escribir al GOP FB directamente.

## Resolución del partial read

`shm_flush_surface()` y el compositor leen el mismo SHM buffer simultáneamente.

**Fix:** Solo el compositor lee el SHM. El juego escribe, el compositor lee, el display_flip sincroniza. Un solo consumidor = sin race conditions.

## Referencia: Quake 2 (patrón funcional)

```c
// rw_ositok.c — Quake 2 port (funciona correctamente)
void SWimp_Init(void) {
    shm_handle = syscall(506, vid.width, vid.height, 4);  // SHM_MKSURFACE
    shm_pixels = (uint32_t *)syscall(501, shm_handle);     // SHM_MAP
}

void SWimp_EndFrame(void) {
    // Convertir palette → ARGB directamente en shm_pixels
    for (int i = 0; i < vid.width * vid.height; i++)
        shm_pixels[i] = palette[backbuffer[i]];
    // NO llama SYS_GUI_FLIP — el compositor lee automáticamente
}
```
