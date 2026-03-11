# Simulador Concurrente de Memoria Virtual (SCMV)

**Laboratorio 3 — Sistemas Operativos 2/2025 — USACH**

**Autores:**
- Martin Araya 21.781.369-7
- Benjamin Letelier 21.329.678-7

---

## 1. Descripción del Proyecto

Simulador que modela dos esquemas de memoria virtual de forma **concurrente** con pthreads:

- **Segmentación**: cada thread tiene tabla de segmentos propia. Traducción: `PA = base[seg_id] + offset`, con verificación de límites (segfaults).
- **Paginación**: cada thread tiene tabla de páginas y TLB propias. Frames físicos compartidos con Frame Allocator global. Incluye page faults con delay simulado, eviction FIFO, y **dirty bit** (escrituras marcan páginas como modificadas → writeback en eviction).

Ambos modos soportan **SAFE** (mutex) y **UNSAFE** (sin locks, para observar carreras de datos).

---

## 2. Arquitectura y Flujo de Datos

### 2.1 Modo Segmentación

```
    Thread (proceso simulado)
           │
           ▼
    Genera dirección virtual: (seg_id, offset)
           │
           ▼
    ┌─────────────────────────┐
    │   Tabla de Segmentos    │  ← privada por thread
    │   (seg_id → base,limit) │
    └─────────────────────────┘
           │
           ├── offset < limit? ──► SÍ: PA = base + offset (traducción OK)
           │
           └── offset >= limit? ─► segfault simulado (violación de límite)
```

**Puntos clave:**
- Cada thread crea su propia tabla de segmentos (memoria privada).
- No hay TLB, page faults, ni frames. Es el esquema más simple.
- La contención entre threads ocurre SOLO al actualizar contadores globales de métricas.

### 2.2 Modo Paginación

```
    Thread (proceso simulado)
           │
           ▼
    Genera dirección virtual: VPN + offset
           │
           ▼
    ┌──────────────┐     HIT (instantáneo, hardware)
    │  1. TLB      │─────────────────────────────────┐
    │  (privada)   │                                 │
    └──────────────┘                                 │
           │ MISS                                    │
           ▼                                         │
    ┌──────────────┐     VÁLIDA (acceso a RAM ~100ns)│
    │  2. Tabla de │─────────────────────────────┐   │
    │  Páginas     │                             │   │
    │  (privada)   │                             │   │
    └──────────────┘                             │   │
           │ INVÁLIDA → PAGE FAULT               │   │
           ▼                                     │   │
    ┌──────────────┐                             │   │
    │  3. Frame    │ ← ¿Hay frames libres?       │   │
    │  Allocator   │   SÍ → asignar frame        │   │
    │  (GLOBAL,    │   NO → eviction FIFO:       │   │
    │  con mutex)  │   ┌─ dirty? → writeback     │   │
    │              │   │   (nanosleep 2-5ms)     │   │
    │              │   └─ clean? → evicción      │   │
    │              │       inmediata             │   │
    └──────────────┘                             │   │
           │                                     │   │
           ▼                                     │   │
    ┌──────────────┐                             │   │
    │  4. nanosleep│ ← Simula carga desde        │   │
    │  (1-5 ms)    │   disco                     │   │
    └──────────────┘                             │   │
           │                                     │   │
           ▼                                     │   │
    Actualizar tabla de páginas + TLB            │   │
    (dirty = 1 si es escritura)                  │   │
           │                                     │   │
           ▼                    ◄────────────────┘   │
    PA = frame × page_size + offset  ◄───────────────┘
```

**Puntos clave:**
- **TLB**: arreglo circular FIFO, privada por thread. TLB hit evita acceso a RAM.
- **Tabla de páginas**: arreglo indexado por VPN, privada. Acceso simula delay de RAM (~100ns).
- **Frame Allocator**: ÚNICA estructura compartida → protegida con mutex en modo SAFE.
- **Consistencia cross-thread**: la lectura de la tabla de páginas y la asignación de frames ocurren bajo el **mismo lock** del FA, evitando que un thread lea `valid=1` para una página que otro thread ya evictó.
- **Dirty bit**: las escrituras marcan `dirty=1`. En eviction, si `dirty` → writeback a disco (2-5ms extra).
- **Eviction cross-thread**: invalida tabla de páginas Y TLB del thread víctima DENTRO del lock.

### 2.3 Gestión de Hilos (pthreads)

```
    main()
      │
      ├── Parsea argumentos (getopt_long)
      ├── Valida parámetros
      ├── Inicializa estructuras globales
      │
      ├── pthread_create(hilo_0, run_thread, args_0)
      ├── pthread_create(hilo_1, run_thread, args_1)
      ├── ...
      ├── pthread_create(hilo_N, run_thread, args_N)
      │
      │   ┌─── Cada hilo ejecuta independientemente: ────┐
      │   │  1. Crea estructuras privadas                │
      │   │  2. Loop: genera VA → traduce → métricas     │
      │   │     (30% escrituras → dirty bit)             │
      │   │  3. Al final: suma métricas locales → global │
      │   │     (con mutex en modo SAFE)                 │
      │   └──────────────────────────────────────────────┘
      │
      ├── pthread_join(hilo_0) ... pthread_join(hilo_N)
      ├── Calcula métricas finales
      ├── Imprime reporte (si --stats)
      └── Genera out/summary.json
```

**Estrategia de métricas:** Cada hilo acumula métricas locales (sin lock). Al terminar, suma todo a las globales en **una sola sección crítica** → minimiza contención.

---

## 3. Consideraciones de Diseño

### 3.1 Generación de Offsets en Segmentación

**Problema:** Si generamos offset en `[0, limit-1]` del propio segmento, **nunca** habría segfault.

**Solución:** El offset se genera en `[0, max_limit - 1]`, donde `max_limit` es el **mayor límite entre todos los segmentos**.

**Ejemplo con `--seg-limits 1024,2048,4096,8192`:**

| Segmento | Límite | Offsets válidos | Tasa de éxito | Tasa de segfault |
|----------|--------|-----------------|---------------|------------------|
| 0        | 1024   | 0 – 1023        | 12.5%         | **87.5%**        |
| 1        | 2048   | 0 – 2047        | 25.0%         | **75.0%**        |
| 2        | 4096   | 0 – 4095        | 50.0%         | **50.0%**        |
| 3        | 8192   | 0 – 8191        | 100.0%        | **0.0%**         |

Con distribución `uniform`: tasa global = `(12.5 + 25.0 + 50.0 + 100.0) / 4 = 46.9%`

### 3.2 Workloads: `uniform` vs `80-20`

| Workload | Comportamiento | Efecto en segmentación | Efecto en paginación |
|----------|---------------|----------------------|---------------------|
| `uniform` | Todos los segmentos/páginas igual de probables | Accede por igual a seg. grandes y pequeños | Sin localidad → TLB poco efectiva |
| `80-20` | 80% de accesos al 20% más grande/frecuente (ley de Pareto) | Concentra en segmentos con mayor límite → **menos segfaults** | Simula localidad temporal → TLB muy efectiva, **menos page faults** |

### 3.3 Dirty Bit (Bonus)

El 30% de las operaciones son escrituras. Al escribir, se marca `dirty=1` en la página. En eviction:
- **Página dirty** → writeback a disco (nanosleep 2-5ms extra) antes de liberar el frame.
- **Página clean** → eviction inmediata, sin writeback.

Esto modela el comportamiento real: las páginas modificadas deben guardarse antes de ser reemplazadas.

### 3.4 Simulación de Jerarquía de Memoria

Para modelar la diferencia real entre TLB (hardware) y tabla de páginas (RAM):
- **TLB lookup**: acceso instantáneo (sin delay) — simula hardware asociativo.
- **Page table lookup**: delay de ~100ns — simula acceso a RAM.
- **Page fault**: nanosleep 1-5ms — simula carga desde disco.
- **Dirty writeback**: nanosleep 2-5ms — simula escritura a disco.

Esto hace que los TLB hits sean significativamente más rápidos que los page table hits, reflejando el comportamiento real.

### 3.5 Reproducibilidad

- Cada hilo usa `rand_r(&seed_local)` con `seed_local = base_seed + thread_id`.
- `rand_r` es thread-safe (no usa estado global).
- Con misma semilla y configuración → resultados idénticos.

### 3.6 Concurrencia: SAFE vs UNSAFE

| Aspecto | SAFE | UNSAFE |
|---------|------|--------|
| Contadores globales | Protegidos con `pthread_mutex` | Sin protección |
| Frame Allocator | Lock antes de asignar/evictar | Sin lock |
| Resultado | Métricas correctas y consistentes | Posibles carreras de datos |

### 3.7 Validación de Entrada

| Validación | Ejemplo de error |
|-----------|-----------------|
| `--mode` obligatorio, solo `seg` o `page` | `Error: --mode es obligatorio y debe ser 'seg' o 'page'` |
| `--threads >= 1` | `Error: --threads debe ser >= 1 (recibido: 0)` |
| `--ops-per-thread >= 1` | `Error: --ops-per-thread debe ser >= 1` |
| `--workload` solo `uniform` o `80-20` | `Error: --workload debe ser 'uniform' o '80-20'` |
| `--seg-limits` coincide con `--segments` | `Error: --seg-limits tiene 3 valores pero --segments es 4` |
| Límites no pueden ser 0 | `Error: el límite del segmento 2 no puede ser 0` |
| Parámetros de paginación `>= 1` | Mensajes específicos por cada uno |
| Políticas solo `fifo` | `Error: --tlb-policy solo soporta 'fifo'` |

---

## 4. Resultados Experimentales

### 4.1 Experimento 1: Segmentación con Segfaults Controlados

**Config:** `--mode seg --threads 1 --workload uniform --ops-per-thread 10000 --segments 4 --seg-limits 1024,2048,4096,8192 --seed 100`

| Métrica | Valor |
|---------|-------|
| translations_ok | 4,721 |
| segfaults | 5,279 |
| Tasa de éxito | **47.2%**  |
| throughput | 4,831,660 ops/seg |

**Verificación:** `4721 + 5279 = 10,000 = ops_per_thread` ✔️

### 4.2 Segmentación: `uniform` vs `80-20` (4 threads)

**Config base:** `--threads 4 --ops-per-thread 5000 --segments 4 --seg-limits 1024,2048,4096,8192 --seed 42`

| Métrica | uniform | 80-20 |
|---------|---------|-------|
| translations_ok | 9,475 | **17,167** |
| segfaults | **10,525** | 2,833 |
| Tasa de éxito | 47.4% | **85.8%** |
| throughput | 11,097,584 | **11,968,623** |

**Análisis (ley de Pareto):** Con `80-20`, el 80% de accesos va al segmento con **mayor límite** (seg 3, `limit=8192`, 0% segfault rate). Esto reduce drásticamente los segfaults: de 10,525 (uniform) a solo 2,833 (80-20). El throughput también mejora levemente porque hay menos overhead de manejo de segfaults. Esto refleja el principio de Pareto: los recursos más usados (segmentos grandes) generan menos errores.

### 4.3 Experimento 2: Impacto del TLB (Paginación)

**Config:** `--mode page --threads 1 --workload 80-20 --ops-per-thread 50000 --pages 128 --frames 64 --seed 200`

| Métrica | Sin TLB (`--tlb-size 0`) | Con TLB (`--tlb-size 32`) |
|---------|--------------------------|---------------------------|
| tlb_hits | 0 | **28,921** |
| tlb_misses | 50,000 | 21,079 |
| hit_rate | 0.0% | **57.8%** |
| page_faults | 9,379 | 9,379 |
| evictions | 9,315 | 9,315 |
| dirty_evictions | 5,700 | 5,700 |
| throughput | 941 | **976** |
| Tiempo total | 53.13s | 51.25s |

**Nota sobre throughput:** La diferencia es modesta (941 vs 976) porque el tiempo está dominado por los nanosleep de page faults (9,379 faults × ~3ms = ~28s). Para aislar el beneficio real de la TLB, ver la siguiente tabla.

#### 4.3.1 TLB Aislada (sin page faults, `frames == pages`)

**Config:** `--pages 64 --frames 64 --ops-per-thread 50000 --workload 80-20 --seed 200`

| Métrica | Sin TLB | Con TLB (32) |
|---------|---------|--------------|
| tlb_hits | 0 | 41,094 |
| hit_rate | 0% | **82.2%** |
| page_faults | 64 | 64 |
| throughput | 15,091 | **64,738** |
| Speedup | 1x | **4.3x** |

**Análisis:** Sin page faults dominando, la TLB da un **speedup de 4.3x** porque cada TLB hit evita el delay de ~100ns del acceso a la tabla de páginas en RAM.

### 4.4 Paginación: `uniform` vs `80-20` (efecto de localidad)

**Config:** `--threads 1 --ops-per-thread 5000 --pages 64 --frames 32 --tlb-size 16 --seed 42`

| Métrica | uniform | 80-20 |
|---------|---------|-------|
| tlb_hits | 1,135 | **2,963** |
| hit_rate | **22.7%** | **59.3%** |
| page_faults | 2,511 | **934** |
| evictions | 2,479 | 902 |
| dirty_evictions | 1,195 | 536 |
| throughput | 401 | **977** |
| Tiempo total | 12.47s | **5.12s** |

**Análisis:** El workload `80-20` demuestra el **principio de localidad**:
- Hit rate se triplica (22.7% → 59.3%)
- Page faults se reducen a un tercio (2,511 → 934)
- Throughput se duplica (401 → 977)

### 4.5 Experimento 3: Thrashing con Múltiples Threads

**Config:** `--workload uniform --ops-per-thread 10000 --pages 64 --frames 8 --tlb-size 16 --seed 300`

| Métrica | 1 thread | 8 threads |
|---------|----------|-----------|
| Total ops | 10,000 | 80,000 |
| tlb_hits | 1,221 | 8,616 |
| hit_rate | 12.2% | 10.8% |
| **page_faults** | **8,779** | **71,384** |
| **PF por thread (prom.)** | **8,779** | **8,923** |
| evictions | 8,771 | 71,376 |
| dirty_evictions | 2,820 | 21,469 |
| throughput | 263 | **2,088** |
| Tiempo total | 38.07s | 38.31s |

**Análisis del thrashing:**
- **1 thread:** 64 páginas compiten por 8 frames → 87.8% page fault rate (8,779/10,000). Solo el 12.2% de accesos son TLB hits.
- **8 threads:** Cada thread tiene ~8,923 page faults en promedio (**más** que con 1 thread), porque 8 threads compiten por los mismos 8 frames → evicción cruzada constante. El thread víctima pierde su página y genera page fault en su siguiente acceso a esa VPN.
- **TLB hit rate** baja de 12.2% a 10.8% con 8 threads: la evicción cross-thread invalida las TLBs de las víctimas, reduciendo la efectividad.
- **Throughput** sube 8x gracias al paralelismo: mientras un thread espera nanosleep (1-5ms), los demás avanzan. El tiempo total es similar (~38s) pero se procesan 8x más operaciones.

### 4.6 Dirty Bit: Impacto del Writeback

De los experimentos anteriores se observa que las dirty evictions representan consistentemente **~30-60%** del total de evictions (coherente con que ~30% de las operaciones son escrituras, con acumulación por re-escrituras a la misma página). El writeback (2-5ms extra) se suma al costo de cada dirty eviction.

| Experimento | Evictions | Dirty Evictions | % Dirty |
|-------------|-----------|-----------------|---------|
| Exp 2 (TLB, 80-20) | 9,315 | 5,700 | 61.2% |
| Uniform vs 80-20 (uniform) | 2,479 | 1,195 | 48.2% |
| Uniform vs 80-20 (80-20) | 902 | 536 | 59.4% |
| Thrashing 1 thread | 8,771 | 2,820 | 32.2% |
| Thrashing 8 threads | 71,376 | 21,469 | 30.1% |

---

## 5. Tests Unitarios

El proyecto incluye **96 tests** organizados en 3 suites:

### 5.1 `test_segmentacion.c` (26 tests)
- Creación de tabla de segmentos (bases contiguas, límites correctos)
- Traducción de direcciones válidas
- Detección de segfaults (offset == limit, offset > limit, caso borde)
- Tasa de segfaults estadística (verifica ~56.25% ±2%)
- Workload 80-20 (80% accesos a hot zone ±5%)
- Reproducibilidad con semilla
- Tabla con segmento único

### 5.2 `test_paginacion.c` (54 tests)
- Tabla de páginas: creación, invalidez inicial, dirty bit = 0
- TLB: lookup (HIT/MISS), insert, FIFO replacement, invalidate, disabled (size=0), update sin duplicar
- Frame Allocator: asignación secuencial, frames únicos, eviction FIFO
- Traducción completa: page fault → TLB hit en segundo acceso
- Dirty bit: lectura = clean, escritura = dirty, TLB hit + escritura = dirty
- Dirty eviction: página dirty → `was_dirty_eviction=1`, clean → `was_dirty_eviction=0`
- Workload VPN: rango válido, concentración 80-20

### 5.3 `test_concurrencia.c` (16 tests)
- Métricas SAFE: ok + fail == total esperado (4 y 8 threads)
- Métricas UNSAFE: ejecuta sin crash, detecta posible inconsistencia
- Reproducibilidad multi-thread (misma seed → misma secuencia)
- Frame Allocator bajo contención (4 threads, 100 ops c/u, evictions == total - frames)
- Paginación multi-thread (2 threads, 4 frames, 8 páginas): page faults, evictions, dirty evictions
- Segmentación multi-thread SAFE: integridad de contadores y tasa estadística

```bash
# Ejecutar todos los tests
make test
```

---

## 6. Estructura del Proyecto

```
proyecto/
├── src/
│   ├── simulator.c        # Main: CLI, threads, reporte, JSON
│   ├── segmentacion.c     # init_segment_table(), traducir_direccion()
│   ├── paginacion.c       # init_page_table(), traducir_pagina() + dirty bit
│   ├── tlb.c              # TLB FIFO: lookup, insert, invalidate
│   ├── frame_allocator.c  # Frame pool + eviction FIFO
│   └── workloads.c        # Generadores uniform + 80-20
├── include/
│   ├── simulator.h        # sim_config, thread_args, globals
│   ├── segmentacion.h     # segment_entry, segment_table, v_addr_seg
│   └── paginacion.h       # page_table_entry (con dirty), tlb, frame_allocator
├── tests/
│   ├── test_segmentacion.c  # 26 tests
│   ├── test_paginacion.c    # 54 tests
│   └── test_concurrencia.c  # 16 tests
├── Makefile               # all, run, reproduce, test, clean
└── out/                   # Generado por make reproduce
    └── summary_exp*.json
```

---

## 7. Compilación y Ejecución

```bash
cd proyecto

make              # Compilar
make run           # Ejemplo por defecto (segmentación)
make reproduce     # 5 experimentos obligatorios → out/summary_exp*.json
make test          # 96 tests unitarios
make clean         # Limpiar todo
```

### Flags de compilación
```
gcc -std=c11 -pthread -Wall -Wextra -Iinclude
```

---

## 8. Flags de Línea de Comandos

### Comunes
| Flag | Tipo | Default | Descripción |
|------|------|---------|-------------|
| `--mode` | `seg`/`page` | *(obligatorio)* | Modo de operación |
| `--threads` | INT | 1 | Número de threads |
| `--ops-per-thread` | INT | 1000 | Accesos por thread |
| `--workload` | `uniform`/`80-20` | `uniform` | Patrón de acceso |
| `--seed` | INT | 42 | Semilla para reproducibilidad |
| `--unsafe` | flag | no | Sin locks (modo UNSAFE) |
| `--stats` | flag | no | Reporte detallado |

### Segmentación
| Flag | Tipo | Default | Descripción |
|------|------|---------|-------------|
| `--segments` | INT | 4 | Segmentos por thread |
| `--seg-limits` | CSV | 4096 c/u | Límites por segmento |

### Paginación
| Flag | Tipo | Default | Descripción |
|------|------|---------|-------------|
| `--pages` | INT | 64 | Páginas virtuales por thread |
| `--frames` | INT | 32 | Frames físicos globales |
| `--page-size` | INT | 4096 | Tamaño de página |
| `--tlb-size` | INT | 16 | Entradas TLB (0 = off) |
| `--tlb-policy` | `fifo` | `fifo` | Política reemplazo TLB |
| `--evict-policy` | `fifo` | `fifo` | Política eviction |

---

## 9. Ejemplos de Comandos

```bash
# Segmentación: segfaults controlados
./simulator --mode seg --threads 1 --workload uniform \
    --ops-per-thread 10000 --segments 4 \
    --seg-limits 1024,2048,4096,8192 --seed 100 --stats

# Paginación: comparar TLB
./simulator --mode page --threads 1 --workload 80-20 \
    --ops-per-thread 50000 --pages 128 --frames 64 \
    --tlb-size 0 --seed 200 --stats

./simulator --mode page --threads 1 --workload 80-20 \
    --ops-per-thread 50000 --pages 128 --frames 64 \
    --tlb-size 32 --seed 200 --stats

# Thrashing: 8 threads con pocos frames
./simulator --mode page --threads 8 --workload uniform \
    --ops-per-thread 10000 --pages 64 --frames 8 \
    --tlb-size 16 --seed 300 --stats

# Modo UNSAFE
./simulator --mode seg --threads 4 --workload uniform \
    --ops-per-thread 5000 --seed 42 --unsafe --stats
```