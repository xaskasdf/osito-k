# Guía de Portabilidad para Osito-K (x86-64)

Este documento recopila las convenciones, APIs y la estructura del sistema descubiertas durante el port de DOOM al entorno de usuario (userspace) x86-64 del kernel **Osito-K**. Su propósito es servir como guía de referencia rápida para adaptar futuras aplicaciones y juegos POSIX al sistema.

---

## 1. Entorno de Ejecución (Userspace)

El entorno x86-64 de Osito-K soporta aplicaciones en "userspace" utilizando binarios tipo **ELF (Executable and Linkable Format)** estándar de 64 bits.

### 1.1 Librería C (libc) y Archivos Objeto C Clave
A diferencia de los sistemas UNIX donde el enlazador llama transparentemente a `/lib/libc.so` y `/lib/crt1.o`, en Osito-K el runtime de C se provee compilando e incluyendo estáticamente objetos internos.

Para portar un programa estándar, la inclusión de cabeceras se simplifica a una sola:
```c
#include <ositok.h> // Define printf, malloc, fopen, open, etc.
```

**Dependencias de Enlazado:**
Todo ejecutable `userspace` debe ser enlazado manualmente (`ld -nostdlib`) con estos objetos generados por la rama `arch/x86/build/libc/` de Osito-K:
- `crt.o`: C Runtime startup (contiene el punto de entrada `_start` que prepara la pila y llama a `main`).
- `syscall.o`: Trampolines y stubs en ensamblador de las llamadas del sistema.
- `tcclib.o`: Implementaciones nativas y funciones de utilidad matemática y de memoria.

### 1.2 Compilación Recomendada
Debido a la ausencia de memoria dinámica arbitraria y soporte de dinámicas complejas durante el startup, los binarios **no deben ser compilados como Position Independent Executables (-fPIE / -pie)**. Usa `-fno-pie` o remueve las banderas de PIE si tu toolchain las inyecta.

**Banderas de GCC base:**
```makefile
CFLAGS = -O2 -Wall -ffreestanding -fno-stack-protector -mno-red-zone -std=gnu89 
LDFLAGS = -nostdlib 
```

---

## 2. Compatibilidad con el Estándar POSIX

Aunque Osito-K cuenta con la cabecera `ositok.h` que contiene el corazón de POSIX, los juegos nativos para UNIX asumen cabeceras estandarizadas (ej. `<fcntl.h>`, `<sys/stat.h>`, `<unistd.h>`).

### Estrategia: Capa de Compatibilidad / Redirección (`ositok_compat.h`)

No alteres todo el código del puerto original eliminando `#include <stdio.h>`. Es preferible interceptarlos:
1. Crea un directorio de dependencias mock (ej. `compat/`).
2. Genera archivos vacíos con los nombres de todas las cabeceras solicitadas (ej. `compat/stdio.h`, `compat/sys/types.h`) que simplemente dirijan la inclusión a una cabecera global de tu puerto:

```c
// compat/stdio.h
#include "ositok_compat.h"
```

3. Construye el "Polyfill" (`ositok_compat.h`):
Allí es donde mapeas los tipos y funciones faltantes a implementaciones de Osito-K o a "stubs" para pasar la compilación.

### Problema de Conflictos de Tipos
Dado que `ositok.h` arroja las declaraciones directas de los llamados al sistema de forma global, puede entrar en conflicto con enumeraciones o macros internos del código original.
Por ejemplo: DOOM utiliza las palabras clave `open` y `close` como estados en el motor lógico de sus puertas (Door). Esto entra en conflicto directo con `int open(const char*, int)` y `int close(int)` de Osito-K. Fue necesario un renombrado interno de las macros locales (`door_open`, `door_close`).

---

## 3. Subsistema Gráfico: X-RETINA (Memoria Compartida Zero-Copy)

Este es el mecanismo más moderno del kernel para interactuar con la pantalla en userspace. No se comunica escribiendo directamente a los registros físicos, sino a través de un "Surface" (Superficie) gestionado por el Compositor.

### 3.1 Llamadas al Sistema Requeridas

Para crear una pantalla hay que invocar directamente a las _Syscalls_:

- `#define SYS_SHM_MKSURFACE 506`
- `#define SYS_SHM_MAP       501`

*(Se acceden a través de las firmas abstractas inyectadas por `syscall.o`: `__syscall1`, `__syscall2`, `__syscall3`, etc.)*

### 3.2 Proceso de Inicialización Gráfica

1. **Crear el Surface:** Se pide al kernel reservar un segmento de memoria para el framebuffer de la aplicación.
   ```c
   // Flags de Surface
   #define SHM_FLAG_CPU_WRITE    (1 << 0)  // Aplicación escribe aquí
   #define SHM_FLAG_GPU_SCANOUT  (1 << 2)  // Compositor lee de aquí
   
   // Retorna un int (Handle) identificador
   long handle = __syscall3(SYS_SHM_MKSURFACE, ANCHO, ALTO, 
                            SHM_FLAG_CPU_WRITE | SHM_FLAG_GPU_SCANOUT);
   ```
2. **Mapear el Surface en Userspace:** Tras obtener el Handle, este debe ser proyectado a un puntero virtual en el proceso del juego:
   ```c
   // Retorna el puntero virtual hacia el Framebuffer local
   uint32_t *framebuffer = (uint32_t *)__syscall1(SYS_SHM_MAP, handle);
   ```

### 3.3 El Framebuffer

El puntero `framebuffer` es una cuadrícula secuencial de píxeles lineales orientada a una arquitectura de **32-bits (XRGB / ARGB)**.
- DOOM (o juegos DOS de los 90s) típicamente dibujan en buffers de 8-bits indexados a una tabla de colores 256. 
- La traducción debe hacerse localmente de la paleta interna de la aplicación al `framebuffer` mapeado por la Syscall antes de "despertar" al compositor para el cambio de frame.

---

## 4. Tiempos del Sistema

Para el ritmo de los motores de juego se requiere lectura de relojes monotónicos.
La Syscall encargada es `SYS_CLOCK_GETTIME` (Syscall #228). 

```c
#define SYS_CLOCK_GETTIME 228

// Se invoca pasando la constante "1" (CLOCK_MONOTONIC en UNIX) 
// y el puntero a una estructura estándar tv_sec y tv_nsec.
struct { long tv_sec; long tv_nsec; } ts;
__syscall2(SYS_CLOCK_GETTIME, 1, (long)&ts);
```

---

## 5. Notas Importantes para Código Legacy (64-bits)

El sistema de compilación de Osito-K x86-64 está configurado con un estricto modelo de ABI de 64-bits y sin *Red Zone* (`-mno-red-zone`).
- **Punteros como Enteros**: Muchos juegos antiguos escritos en C89 (ej. DOOM) empaquetan punteros, cast de callbacks y cadenas de texto dentro de variables declaradas como de tipo `int` (32-bits).
- Al compilar en x86-64, los punteros tienen 64-bits. Este truncamiento destruirá la ejecución en Osito-K y en GCC causará errores fatales de `initializer element is not constant` o alertas cast puntero-a-entero.
- **Solución rápida**: Elevar todos los depósitos genéricos (`void*`, `int`) antiguos a macros genéricas o a tipos de 64-bits (como `long`). Modifica structs como los sistemas de configuración estáticos donde los punteros se igualaban a campos `int` a campos explícitos de `long`.
