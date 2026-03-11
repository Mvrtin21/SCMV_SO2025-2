// =====================================================================
// test_concurrencia.c — Tests de concurrencia (SAFE vs UNSAFE)
//
// Verifica:
//   1. Integridad de métricas con múltiples threads en modo SAFE
//   2. Detección de inconsistencias en modo UNSAFE
//   3. Cada thread genera secuencia reproducible con su propia semilla
//   4. Frame Allocator bajo contención (múltiples threads compitiendo)
//   5. Eviction cross-thread (invalidar tabla de otro thread)
//
// Compilar:
//   gcc -std=c11 -pthread -Wall -Wextra -Iinclude -o test_conc \
//       tests/test_concurrencia.c src/paginacion.c src/tlb.c \
//       src/frame_allocator.c src/workloads.c src/segmentacion.c -lm
//
// Ejecutar:
//   ./test_conc
// =====================================================================
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "../include/paginacion.h"
#include "../include/simulator.h"
#include "../include/segmentacion.h"

#define GREEN "\033[0;32m"
#define RED   "\033[0;31m"
#define YELLOW "\033[0;33m"
#define RESET "\033[0m"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  TEST: %-55s ", name)
#define PASS() do { printf(GREEN "[PASS]" RESET "\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf(RED "[FAIL] %s" RESET "\n", msg); tests_failed++; } while(0)
#define WARN(msg) do { printf(YELLOW "[WARN] %s" RESET "\n", msg); tests_passed++; } while(0)

// Variables globales para tests
static uint64_t test_global_ok = 0;
static uint64_t test_global_faults = 0;
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;

extern v_addr_seg generate_address_seg(sim_config *conf, segment_table *tabla, unsigned int *seed_local);
extern int generate_vpn_page(sim_config *conf, unsigned int *seed_local);
extern uint64_t traducir_pagina(uint64_t vpn, uint64_t offset,
                                page_table *pt, tlb *mi_tlb,
                                frame_allocator *fa,
                                int thread_id,
                                page_table **all_pts, tlb **all_tlbs,
                                int num_threads,
                                int page_size, int use_lock,
                                int is_write,
                                int *was_tlb_hit, int *was_page_fault,
                                int *was_eviction, int *was_dirty_eviction);

// =====================================================================
// Test 1: Métricas SAFE — la suma siempre cuadra
//
// Ejecutamos N threads, cada uno incrementa contadores locales,
// al final suma con mutex. El total debe ser exacto.
// =====================================================================
typedef struct {
    int thread_id;
    int ops;
    uint64_t local_ok;
    uint64_t local_fail;
    int use_lock;
} counter_args;

void* counter_thread(void *arg) {
    counter_args *a = (counter_args *)arg;
    a->local_ok = 0;
    a->local_fail = 0;

    unsigned int seed = 42 + a->thread_id;

    for (int i = 0; i < a->ops; i++) {
        if (rand_r(&seed) % 2 == 0) {
            a->local_ok++;
        } else {
            a->local_fail++;
        }
    }

    // Sumar a global
    if (a->use_lock) pthread_mutex_lock(&test_lock);
    test_global_ok += a->local_ok;
    test_global_faults += a->local_fail;
    if (a->use_lock) pthread_mutex_unlock(&test_lock);

    return NULL;
}

void test_metricas_safe(void) {
    printf("\n=== Métricas SAFE (con mutex) ===\n");

    int num_threads = 8;
    int ops = 100000;

    test_global_ok = 0;
    test_global_faults = 0;

    pthread_t threads[num_threads];
    counter_args args[num_threads];

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].ops = ops;
        args[i].use_lock = 1;
        pthread_create(&threads[i], NULL, counter_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t expected = (uint64_t)num_threads * ops;
    uint64_t actual = test_global_ok + test_global_faults;

    TEST("SAFE: ok + fail == total esperado");
    if (actual == expected) {
        PASS();
    } else {
        printf(RED "[FAIL] " RESET "esperado=%lu, actual=%lu\n", expected, actual);
        tests_failed++;
    }

    // Verificar que locales suman al global
    uint64_t sum_ok = 0, sum_fail = 0;
    for (int i = 0; i < num_threads; i++) {
        sum_ok += args[i].local_ok;
        sum_fail += args[i].local_fail;
    }

    TEST("SAFE: suma locales == globals");
    if (sum_ok == test_global_ok && sum_fail == test_global_faults) {
        PASS();
    } else {
        FAIL("locales no suman a globals");
    }
}

// =====================================================================
// Test 2: Métricas UNSAFE — puede tener inconsistencias
//
// No forzamos un fallo, pero verificamos que al menos ejecuta
// sin crash. En la práctica, con muchas iteraciones puede perder
// actualizaciones.
// =====================================================================
void test_metricas_unsafe(void) {
    printf("\n=== Métricas UNSAFE (sin mutex) ===\n");

    int num_threads = 8;
    int ops = 100000;

    test_global_ok = 0;
    test_global_faults = 0;

    pthread_t threads[num_threads];
    counter_args args[num_threads];

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].ops = ops;
        args[i].use_lock = 0;  // Sin mutex
        pthread_create(&threads[i], NULL, counter_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t expected = (uint64_t)num_threads * ops;
    uint64_t actual = test_global_ok + test_global_faults;

    TEST("UNSAFE: ejecuta sin crash");
    PASS();

    TEST("UNSAFE: posible inconsistencia en contadores");
    if (actual != expected) {
        printf(YELLOW "[WARN] " RESET "esperado=%lu, actual=%lu (diferencia=%ld) — data race detectada\n",
               expected, actual, (long)(expected - actual));
        tests_passed++;
    } else {
        printf(GREEN "[PASS] " RESET "(consistente esta vez — depende de scheduling)\n");
        tests_passed++;
    }
}

// =====================================================================
// Test 3: Reproducibilidad entre threads
//
// Cada thread con seed_local = base_seed + thread_id genera secuencia
// determinista. Ejecutar dos veces debe dar el mismo resultado.
// =====================================================================
void test_reproducibilidad_threads(void) {
    printf("\n=== Reproducibilidad Multi-Thread ===\n");

    sim_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.pages = 32;
    strcpy(conf.workload, "uniform");

    // Generar con seed=42+0, 42+1, etc.
    int vpns_run1[4][100];
    int vpns_run2[4][100];

    for (int t = 0; t < 4; t++) {
        unsigned int seed = 42 + t;
        for (int i = 0; i < 100; i++) {
            vpns_run1[t][i] = generate_vpn_page(&conf, &seed);
        }
    }

    for (int t = 0; t < 4; t++) {
        unsigned int seed = 42 + t;
        for (int i = 0; i < 100; i++) {
            vpns_run2[t][i] = generate_vpn_page(&conf, &seed);
        }
    }

    int all_equal = 1;
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 100; i++) {
            if (vpns_run1[t][i] != vpns_run2[t][i]) {
                all_equal = 0;
                break;
            }
        }
    }

    TEST("secuencias idénticas entre dos ejecuciones");
    if (all_equal) { PASS(); } else { FAIL("secuencias difieren"); }

    // Verificar que threads distintos generan secuencias distintas
    int diff_threads = 0;
    for (int i = 0; i < 100; i++) {
        if (vpns_run1[0][i] != vpns_run1[1][i]) {
            diff_threads = 1;
            break;
        }
    }

    TEST("threads distintos generan secuencias distintas");
    if (diff_threads) { PASS(); } else { FAIL("misma secuencia para threads distintos"); }
}

// =====================================================================
// Test 4: Frame Allocator bajo contención
//
// Múltiples threads compiten por un pool reducido de frames.
// En modo SAFE, cada frame debe asignarse exactamente una vez.
// =====================================================================
typedef struct {
    frame_allocator *fa;
    int thread_id;
    int ops;
    int allocated_frames[1000];  // Frames obtenidos
    int evictions;
} fa_thread_args;

void* fa_thread(void *arg) {
    fa_thread_args *a = (fa_thread_args *)arg;
    a->evictions = 0;

    for (int i = 0; i < a->ops; i++) {
        int v_thread; uint64_t v_vpn; int was_dirty;
        a->allocated_frames[i] = allocate_frame(
            a->fa, a->thread_id, (uint64_t)i,
            &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1  // use_lock=1 (SAFE)
        );
        if (v_thread >= 0) a->evictions++;
    }

    return NULL;
}

void test_fa_contention(void) {
    printf("\n=== Frame Allocator bajo Contención ===\n");

    int num_frames = 8;
    int num_threads = 4;
    int ops_per_thread = 100;

    frame_allocator *fa = init_frame_allocator(num_frames);
    pthread_t threads[num_threads];
    fa_thread_args args[num_threads];

    for (int i = 0; i < num_threads; i++) {
        args[i].fa = fa;
        args[i].thread_id = i;
        args[i].ops = ops_per_thread;
        pthread_create(&threads[i], NULL, fa_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST("todos los threads completaron sin crash");
    PASS();

    // Verificar que los frames asignados están en rango válido
    int all_valid = 1;
    for (int t = 0; t < num_threads; t++) {
        for (int i = 0; i < ops_per_thread; i++) {
            int f = args[t].allocated_frames[i];
            if (f < 0 || f >= num_frames) {
                all_valid = 0;
                break;
            }
        }
    }

    TEST("todos los frames en rango [0, num_frames)");
    if (all_valid) { PASS(); } else { FAIL("frame fuera de rango"); }

    // Contar evictions totales
    int total_evictions = 0;
    for (int i = 0; i < num_threads; i++) {
        total_evictions += args[i].evictions;
    }

    int total_ops = num_threads * ops_per_thread;
    TEST("evictions > 0 (pool saturado)");
    if (total_evictions > 0) {
        printf(GREEN "[PASS] " RESET "(%d evictions de %d ops)\n", total_evictions, total_ops);
        tests_passed++;
    } else {
        FAIL("debería haber evictions");
    }

    // Evictions = total_ops - num_frames (los primeros 8 no evictan)
    int expected_evictions = total_ops - num_frames;
    TEST("evictions == total_ops - num_frames");
    if (total_evictions == expected_evictions) {
        printf(GREEN "[PASS] " RESET "(%d == %d)\n", total_evictions, expected_evictions);
        tests_passed++;
    } else {
        printf(RED "[FAIL] " RESET "esperado=%d, actual=%d\n", expected_evictions, total_evictions);
        tests_failed++;
    }

    free_frame_allocator(fa);
}

// =====================================================================
// Test 5: Paginación multi-thread — eviction cross-thread
//
// 2 threads comparten 4 frames, cada uno tiene 8 páginas.
// Cuando se llenan los frames, la eviction de un thread invalida
// la tabla de páginas del thread víctima.
// =====================================================================
typedef struct {
    int thread_id;
    page_table *pt;
    tlb *mi_tlb;
    frame_allocator *fa;
    page_table **all_pts;
    tlb **all_tlbs;
    int num_threads;
    int ops;
    uint64_t page_faults;
    uint64_t evictions;
    uint64_t dirty_evictions;
} pag_thread_args;

void* pag_thread(void *arg) {
    pag_thread_args *a = (pag_thread_args *)arg;
    a->page_faults = 0;
    a->evictions = 0;
    a->dirty_evictions = 0;

    unsigned int seed = 42 + a->thread_id;

    for (int i = 0; i < a->ops; i++) {
        int vpn = rand_r(&seed) % 8;
        int is_write = (rand_r(&seed) % 100) < 30;
        int was_tlb_hit, was_pf, was_evict, was_dirty;

        traducir_pagina((uint64_t)vpn, 0, a->pt, a->mi_tlb,
                         a->fa, a->thread_id,
                         a->all_pts, a->all_tlbs, a->num_threads,
                         4096, 1, is_write,
                         &was_tlb_hit, &was_pf, &was_evict, &was_dirty);

        if (was_pf) a->page_faults++;
        if (was_evict) a->evictions++;
        if (was_dirty) a->dirty_evictions++;
    }

    return NULL;
}

void test_paginacion_multithread(void) {
    printf("\n=== Paginación Multi-Thread ===\n");

    int num_threads = 2;
    int num_frames = 4;
    int num_pages = 8;
    int ops = 500;

    frame_allocator *fa = init_frame_allocator(num_frames);
    page_table *all_pts[2];
    tlb *all_tlbs[2];

    for (int i = 0; i < num_threads; i++) {
        all_pts[i] = init_page_table(num_pages);
        all_tlbs[i] = init_tlb(4);
    }

    pthread_t threads[2];
    pag_thread_args args[2];

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].pt = all_pts[i];
        args[i].mi_tlb = all_tlbs[i];
        args[i].fa = fa;
        args[i].all_pts = all_pts;
        args[i].all_tlbs = all_tlbs;
        args[i].num_threads = num_threads;
        args[i].ops = ops;
        pthread_create(&threads[i], NULL, pag_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST("multi-thread paginación sin crash");
    PASS();

    uint64_t total_pf = args[0].page_faults + args[1].page_faults;
    uint64_t total_ev = args[0].evictions + args[1].evictions;
    uint64_t total_dirty = args[0].dirty_evictions + args[1].dirty_evictions;

    TEST("hubo page faults (expected con 8 pags y 4 frames)");
    if (total_pf > 0) {
        printf(GREEN "[PASS] " RESET "(total_pf=%lu)\n", total_pf);
        tests_passed++;
    } else {
        FAIL("debería haber page faults");
    }

    TEST("hubo evictions (pool saturado)");
    if (total_ev > 0) {
        printf(GREEN "[PASS] " RESET "(total_ev=%lu)\n", total_ev);
        tests_passed++;
    } else {
        FAIL("debería haber evictions");
    }

    TEST("dirty evictions > 0 (30% writes)");
    if (total_dirty > 0) {
        printf(GREEN "[PASS] " RESET "(dirty_ev=%lu de %lu evictions, %.1f%%)\n",
               total_dirty, total_ev, (double)total_dirty / total_ev * 100);
        tests_passed++;
    } else {
        FAIL("debería haber dirty evictions");
    }

    for (int i = 0; i < num_threads; i++) {
        free_page_table(all_pts[i]);
        free_tlb(all_tlbs[i]);
    }
    free_frame_allocator(fa);
}

// =====================================================================
// Test 6: Segmentación multi-thread SAFE
// =====================================================================
typedef struct {
    int thread_id;
    int ops;
    uint64_t *limits;
    int num_seg;
    uint64_t local_ok;
    uint64_t local_fail;
} seg_args;

void* seg_thread(void *arg) {
    seg_args *a = (seg_args *)arg;
    a->local_ok = 0;
    a->local_fail = 0;

    segment_table *tabla = init_segment_table(a->num_seg, a->limits);
    sim_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.segments = a->num_seg;
    conf.seg_limits = a->limits;
    strcpy(conf.workload, "uniform");

    unsigned int seed = 42 + a->thread_id;

    for (int i = 0; i < a->ops; i++) {
        v_addr_seg addr = generate_address_seg(&conf, tabla, &seed);
        uint64_t pa = traducir_direccion(tabla, addr.seg_id, addr.offset);
        if (pa == (uint64_t)-1) a->local_fail++;
        else a->local_ok++;
    }

    pthread_mutex_lock(&test_lock);
    test_global_ok += a->local_ok;
    test_global_faults += a->local_fail;
    pthread_mutex_unlock(&test_lock);

    free(tabla->segments);
    free(tabla);
    return NULL;
}

void test_segmentacion_multithread(void) {
    printf("\n=== Segmentación Multi-Thread SAFE ===\n");

    int num_threads = 4;
    int ops = 10000;
    uint64_t limits[] = {1024, 2048, 4096, 8192};

    test_global_ok = 0;
    test_global_faults = 0;

    pthread_t threads[num_threads];
    seg_args args[num_threads];

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].ops = ops;
        args[i].limits = limits;
        args[i].num_seg = 4;
        pthread_create(&threads[i], NULL, seg_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t total = test_global_ok + test_global_faults;
    uint64_t expected = (uint64_t)num_threads * ops;

    TEST("SAFE: ok + segfaults == total esperado");
    if (total == expected) {
        printf(GREEN "[PASS] " RESET "(%lu == %lu)\n", total, expected);
        tests_passed++;
    } else {
        printf(RED "[FAIL] " RESET "esperado=%lu, actual=%lu\n", expected, total);
        tests_failed++;
    }

    // Verificar tasa de éxito (teórica ~46.9%)
    double tasa = (double)test_global_ok / total * 100.0;
    TEST("tasa de éxito multi-thread ≈ 46.9% (±3%)");
    if (tasa > 43.9 && tasa < 49.9) {
        printf(GREEN "[PASS] " RESET "(%.1f%%)\n", tasa);
        tests_passed++;
    } else {
        printf(RED "[FAIL] " RESET "tasa=%.1f%%\n", tasa);
        tests_failed++;
    }
}

int main(void) {
    printf("========================================\n");
    printf("  TESTS DE CONCURRENCIA\n");
    printf("========================================\n");

    test_metricas_safe();
    test_metricas_unsafe();
    test_reproducibilidad_threads();
    test_fa_contention();
    test_paginacion_multithread();
    test_segmentacion_multithread();

    printf("\n========================================\n");
    printf("  RESULTADO: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
