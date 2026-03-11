// Autores:
//   - Martin Araya 21.781.369-7
//   - Benjamin Letelier 21.329.678-7
//
// =====================================================================
// test_paginacion.c — Tests unitarios para paginación, TLB y Frame Allocator
//
// Verifica:
//   1. Tabla de páginas: creación e invalidez inicial
//   2. TLB: lookup, insert, FIFO replacement, invalidate
//   3. Frame Allocator: asignación, eviction FIFO
//   4. Traducción completa: TLB hit, page table hit, page fault
//   5. Dirty bit: marcado en escritura, writeback en eviction
//   6. Workload de paginación (uniform y 80-20)
//
// Compilar:
//   gcc -std=c11 -pthread -Wall -Wextra -Iinclude -o test_pag \
//       tests/test_paginacion.c src/paginacion.c src/tlb.c \
//       src/frame_allocator.c src/workloads.c src/segmentacion.c -lm
//
// Ejecutar:
//   ./test_pag
// =====================================================================
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../include/paginacion.h"
#include "../include/simulator.h"

#define GREEN "\033[0;32m"
#define RED   "\033[0;31m"
#define RESET "\033[0m"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  TEST: %-55s ", name)
#define PASS() do { printf(GREEN "[PASS]" RESET "\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf(RED "[FAIL] %s" RESET "\n", msg); tests_failed++; } while(0)

// Declaraciones externas
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
// Test 1: Tabla de páginas
// =====================================================================
void test_page_table(void) {
    printf("\n=== Tabla de Páginas ===\n");

    page_table *pt = init_page_table(16);

    TEST("tabla no es NULL");
    if (pt != NULL) { PASS(); } else { FAIL("NULL"); return; }

    TEST("num_pages == 16");
    if (pt->num_pages == 16) { PASS(); } else { FAIL("incorrecto"); }

    TEST("todas las páginas empiezan inválidas");
    int all_invalid = 1;
    for (int i = 0; i < 16; i++) {
        if (pt->entries[i].valid != 0 || pt->entries[i].frame_number != INVALID_FRAME) {
            all_invalid = 0;
            break;
        }
    }
    if (all_invalid) { PASS(); } else { FAIL("alguna página no está inválida"); }

    TEST("dirty bit empieza en 0");
    int all_clean = 1;
    for (int i = 0; i < 16; i++) {
        if (pt->entries[i].dirty != 0) { all_clean = 0; break; }
    }
    if (all_clean) { PASS(); } else { FAIL("alguna página empieza dirty"); }

    // Simular que cargamos una página manualmente
    pt->entries[5].frame_number = 3;
    pt->entries[5].valid = 1;

    TEST("página 5 marcada como válida en frame 3");
    if (pt->entries[5].valid == 1 && pt->entries[5].frame_number == 3) {
        PASS();
    } else {
        FAIL("no se asignó correctamente");
    }

    free_page_table(pt);
}

// =====================================================================
// Test 2: TLB — Lookup y Insert
// =====================================================================
void test_tlb_basic(void) {
    printf("\n=== TLB Básica ===\n");

    tlb *t = init_tlb(4);

    TEST("TLB no es NULL");
    if (t != NULL) { PASS(); } else { FAIL("NULL"); return; }

    TEST("TLB size == 4");
    if (t->size == 4) { PASS(); } else { FAIL("size incorrecto"); }

    // Lookup en TLB vacía → MISS
    uint64_t frame;
    TEST("lookup en TLB vacía → MISS");
    if (tlb_lookup(t, 10, &frame) == 0) { PASS(); } else { FAIL("debería ser MISS"); }

    // Insert y luego lookup → HIT
    tlb_insert(t, 10, 5);
    TEST("insert VPN=10,frame=5 → lookup HIT");
    if (tlb_lookup(t, 10, &frame) == 1 && frame == 5) { PASS(); } else { FAIL("debería ser HIT con frame=5"); }

    // Lookup de VPN no insertado → MISS
    TEST("lookup VPN=20 (no insertado) → MISS");
    if (tlb_lookup(t, 20, &frame) == 0) { PASS(); } else { FAIL("debería ser MISS"); }

    // Insert más entradas
    tlb_insert(t, 20, 6);
    tlb_insert(t, 30, 7);
    tlb_insert(t, 40, 8);

    TEST("TLB llena, todos los lookups dan HIT");
    int all_hit = 1;
    all_hit &= (tlb_lookup(t, 10, &frame) == 1);
    all_hit &= (tlb_lookup(t, 20, &frame) == 1);
    all_hit &= (tlb_lookup(t, 30, &frame) == 1);
    all_hit &= (tlb_lookup(t, 40, &frame) == 1);
    if (all_hit) { PASS(); } else { FAIL("algún lookup falló"); }

    free_tlb(t);
}

// =====================================================================
// Test 3: TLB — Reemplazo FIFO
// =====================================================================
void test_tlb_fifo_replacement(void) {
    printf("\n=== TLB FIFO Replacement ===\n");

    tlb *t = init_tlb(3);  // TLB pequeña de 3 entradas

    // Llenar: [A=10, B=20, C=30]
    tlb_insert(t, 10, 1);
    tlb_insert(t, 20, 2);
    tlb_insert(t, 30, 3);

    uint64_t frame;

    // Insertar D=40 → debería reemplazar A=10 (la más antigua)
    tlb_insert(t, 40, 4);

    TEST("después de reemplazo FIFO, VPN=10 → MISS");
    if (tlb_lookup(t, 10, &frame) == 0) { PASS(); } else { FAIL("10 debería estar evictado"); }

    TEST("VPN=40 → HIT");
    if (tlb_lookup(t, 40, &frame) == 1 && frame == 4) { PASS(); } else { FAIL("40 no encontrado"); }

    TEST("VPN=20 sigue en TLB → HIT");
    if (tlb_lookup(t, 20, &frame) == 1) { PASS(); } else { FAIL("20 debería seguir"); }

    TEST("VPN=30 sigue en TLB → HIT");
    if (tlb_lookup(t, 30, &frame) == 1) { PASS(); } else { FAIL("30 debería seguir"); }

    // Insertar E=50 → reemplaza B=20
    tlb_insert(t, 50, 5);
    TEST("segundo reemplazo FIFO, VPN=20 → MISS");
    if (tlb_lookup(t, 20, &frame) == 0) { PASS(); } else { FAIL("20 debería estar evictado"); }

    free_tlb(t);
}

// =====================================================================
// Test 4: TLB — Invalidación
// =====================================================================
void test_tlb_invalidate(void) {
    printf("\n=== TLB Invalidación ===\n");

    tlb *t = init_tlb(4);
    tlb_insert(t, 10, 1);
    tlb_insert(t, 20, 2);
    tlb_insert(t, 30, 3);

    uint64_t frame;

    TEST("VPN=20 está en TLB antes de invalidar");
    if (tlb_lookup(t, 20, &frame) == 1) { PASS(); } else { FAIL("no encontrado"); }

    tlb_invalidate_vpn(t, 20);

    TEST("VPN=20 MISS después de invalidar");
    if (tlb_lookup(t, 20, &frame) == 0) { PASS(); } else { FAIL("debería ser MISS"); }

    TEST("VPN=10 sigue válido (no afectado)");
    if (tlb_lookup(t, 10, &frame) == 1) { PASS(); } else { FAIL("10 debería seguir"); }

    TEST("VPN=30 sigue válido (no afectado)");
    if (tlb_lookup(t, 30, &frame) == 1) { PASS(); } else { FAIL("30 debería seguir"); }

    free_tlb(t);
}

// =====================================================================
// Test 5: TLB deshabilitada (size=0)
// =====================================================================
void test_tlb_disabled(void) {
    printf("\n=== TLB Deshabilitada ===\n");

    tlb *t = init_tlb(0);

    TEST("TLB de tamaño 0 no es NULL");
    if (t != NULL) { PASS(); } else { FAIL("NULL"); return; }

    TEST("entries es NULL (no hay arreglo)");
    if (t->entries == NULL) { PASS(); } else { FAIL("debería ser NULL"); }

    uint64_t frame;
    TEST("lookup siempre MISS con TLB disabled");
    if (tlb_lookup(t, 10, &frame) == 0) { PASS(); } else { FAIL("debería ser MISS"); }

    // Insert no debería crashear
    TEST("insert con TLB disabled no crashea");
    tlb_insert(t, 10, 5);
    PASS();

    TEST("invalidate con TLB disabled no crashea");
    tlb_invalidate_vpn(t, 10);
    PASS();

    free_tlb(t);
}

// =====================================================================
// Test 6: Frame Allocator — Asignación básica
// =====================================================================
void test_frame_allocator_basic(void) {
    printf("\n=== Frame Allocator Básico ===\n");

    frame_allocator *fa = init_frame_allocator(4);

    TEST("FA no es NULL");
    if (fa != NULL) { PASS(); } else { FAIL("NULL"); return; }

    TEST("total_frames == 4");
    if (fa->total_frames == 4) { PASS(); } else { FAIL("incorrecto"); }

    TEST("free_count == 4 al inicio");
    if (fa->free_count == 4) { PASS(); } else { FAIL("incorrecto"); }

    // Asignar frames (deben ser únicos)
    int v_thread; uint64_t v_vpn; int was_dirty;
    int f0 = allocate_frame(fa, 0, 0, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    int f1 = allocate_frame(fa, 0, 1, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    int f2 = allocate_frame(fa, 0, 2, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    int f3 = allocate_frame(fa, 0, 3, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);

    TEST("4 frames asignados son únicos");
    if (f0 != f1 && f0 != f2 && f0 != f3 && f1 != f2 && f1 != f3 && f2 != f3) {
        PASS();
    } else {
        FAIL("frames duplicados");
    }

    TEST("free_count == 0 después de asignar 4");
    if (fa->free_count == 0) { PASS(); } else { FAIL("debería ser 0"); }

    TEST("sin eviction, victim_thread == -1");
    // Ya verificamos en las llamadas anteriores, pero revisemos la última
    allocate_frame(fa, 0, 0, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    // Ahora sí hay eviction porque no hay libres
    // Verificaremos en el siguiente test

    free_frame_allocator(fa);
}

// =====================================================================
// Test 7: Frame Allocator — Eviction FIFO
// =====================================================================
void test_frame_allocator_eviction(void) {
    printf("\n=== Frame Allocator Eviction FIFO ===\n");

    frame_allocator *fa = init_frame_allocator(3);

    int v_thread; uint64_t v_vpn; int was_dirty;

    // Llenar: frame para thread 0/vpn 10, thread 1/vpn 20, thread 2/vpn 30
    allocate_frame(fa, 0, 10, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    TEST("primera asignación: sin eviction");
    if (v_thread == -1) { PASS(); } else { FAIL("no debería evictar"); }

    allocate_frame(fa, 1, 20, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    allocate_frame(fa, 2, 30, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);

    // Ahora no hay frames libres → eviction
    allocate_frame(fa, 3, 40, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    TEST("4ta asignación evicta thread=0, vpn=10 (FIFO)");
    if (v_thread == 0 && v_vpn == 10) { PASS(); } else { FAIL("víctima incorrecta"); }

    allocate_frame(fa, 4, 50, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    TEST("5ta asignación evicta thread=1, vpn=20 (FIFO)");
    if (v_thread == 1 && v_vpn == 20) { PASS(); } else { FAIL("víctima incorrecta"); }

    allocate_frame(fa, 5, 60, &v_thread, &v_vpn, &was_dirty, NULL, NULL, 0, 1);
    TEST("6ta asignación evicta thread=2, vpn=30 (FIFO)");
    if (v_thread == 2 && v_vpn == 30) { PASS(); } else { FAIL("víctima incorrecta"); }

    free_frame_allocator(fa);
}

// =====================================================================
// Test 8: Traducción completa — TLB hit, page table hit, page fault
// =====================================================================
void test_traduccion_completa(void) {
    printf("\n=== Traducción Completa ===\n");

    int num_threads = 1;
    page_table *pt = init_page_table(8);
    tlb *mi_tlb = init_tlb(4);
    frame_allocator *fa = init_frame_allocator(4);

    page_table *all_pts[] = { pt };
    tlb *all_tlbs[] = { mi_tlb };

    int was_tlb_hit, was_pf, was_evict, was_dirty_evict;

    // Primera traducción VPN=0 → page fault obligatorio (todo inválido)
    uint64_t pa = traducir_pagina(0, 100, pt, mi_tlb, fa, 0,
                                   all_pts, all_tlbs, num_threads,
                                   4096, 1, 0,
                                   &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("primer acceso VPN=0 → page fault");
    if (was_pf == 1) { PASS(); } else { FAIL("debería ser page fault"); }

    TEST("primer acceso → no es TLB hit");
    if (was_tlb_hit == 0) { PASS(); } else { FAIL("no puede ser TLB hit"); }

    TEST("PA tiene el offset correcto (PA mod 4096 == 100)");
    if (pa % 4096 == 100) { PASS(); } else { FAIL("offset incorrecto en PA"); }

    // Segundo acceso a VPN=0 → debería ser TLB hit (ya se insertó)
    pa = traducir_pagina(0, 200, pt, mi_tlb, fa, 0,
                          all_pts, all_tlbs, num_threads,
                          4096, 1, 0,
                          &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("segundo acceso VPN=0 → TLB hit");
    if (was_tlb_hit == 1) { PASS(); } else { FAIL("debería ser TLB hit"); }

    TEST("segundo acceso → no page fault");
    if (was_pf == 0) { PASS(); } else { FAIL("no debería ser page fault"); }

    TEST("PA con offset 200");
    if (pa % 4096 == 200) { PASS(); } else { FAIL("offset incorrecto"); }

    // Página está válida en tabla de páginas
    TEST("VPN=0 está válida en tabla de páginas");
    if (pt->entries[0].valid == 1) { PASS(); } else { FAIL("debería estar válida"); }

    free_page_table(pt);
    free_tlb(mi_tlb);
    free_frame_allocator(fa);
}

// =====================================================================
// Test 9: Dirty bit
// =====================================================================
void test_dirty_bit(void) {
    printf("\n=== Dirty Bit ===\n");

    int num_threads = 1;
    page_table *pt = init_page_table(4);
    tlb *mi_tlb = init_tlb(4);
    frame_allocator *fa = init_frame_allocator(4);

    page_table *all_pts[] = { pt };
    tlb *all_tlbs[] = { mi_tlb };

    int was_tlb_hit, was_pf, was_evict, was_dirty_evict;

    // Lectura a VPN=0 → page fault, dirty=0
    traducir_pagina(0, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 0,  // is_write=0
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("lectura: dirty == 0");
    if (pt->entries[0].dirty == 0) { PASS(); } else { FAIL("debería ser limpia"); }

    // Escritura a VPN=1 → page fault, dirty=1
    traducir_pagina(1, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 1,  // is_write=1
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("escritura (page fault): dirty == 1");
    if (pt->entries[1].dirty == 1) { PASS(); } else { FAIL("debería ser dirty"); }

    // Escritura a VPN=0 (TLB hit, ya cargada) → dirty=1
    traducir_pagina(0, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 1,  // is_write=1
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("escritura (TLB hit): dirty cambia a 1");
    if (pt->entries[0].dirty == 1) { PASS(); } else { FAIL("debería ser dirty"); }

    TEST("escritura (TLB hit): fue TLB hit");
    if (was_tlb_hit == 1) { PASS(); } else { FAIL("debería ser TLB hit"); }

    free_page_table(pt);
    free_tlb(mi_tlb);
    free_frame_allocator(fa);
}

// =====================================================================
// Test 10: Dirty eviction (writeback)
// =====================================================================
void test_dirty_eviction(void) {
    printf("\n=== Dirty Eviction ===\n");

    int num_threads = 1;
    page_table *pt = init_page_table(8);
    tlb *mi_tlb = init_tlb(0);  // Sin TLB para simplificar
    frame_allocator *fa = init_frame_allocator(2);  // Solo 2 frames → eviction rápida

    page_table *all_pts[] = { pt };
    tlb *all_tlbs[] = { mi_tlb };

    int was_tlb_hit, was_pf, was_evict, was_dirty_evict;

    // Cargar VPN=0 con escritura (dirty)
    traducir_pagina(0, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 1,
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    // Cargar VPN=1 con lectura (limpia)
    traducir_pagina(1, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 0,
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("2 pages cargadas, sin eviction aún");
    if (was_evict == 0) { PASS(); } else { FAIL("no debería evictar"); }

    // Cargar VPN=2 → evicta VPN=0 (FIFO), que es dirty
    traducir_pagina(2, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 0,
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("evicta VPN=0 (dirty) → dirty_eviction=1");
    if (was_evict == 1 && was_dirty_evict == 1) {
        PASS();
    } else {
        FAIL("debería ser dirty eviction");
    }

    // Cargar VPN=3 → evicta VPN=1 (limpia)
    traducir_pagina(3, 0, pt, mi_tlb, fa, 0,
                     all_pts, all_tlbs, num_threads,
                     4096, 1, 0,
                     &was_tlb_hit, &was_pf, &was_evict, &was_dirty_evict);

    TEST("evicta VPN=1 (limpia) → dirty_eviction=0");
    if (was_evict == 1 && was_dirty_evict == 0) {
        PASS();
    } else {
        FAIL("eviction limpia debería tener dirty_evict=0");
    }

    // VPN=0 debe estar inválida después de eviction
    TEST("VPN=0 inválida después de eviction");
    if (pt->entries[0].valid == 0) { PASS(); } else { FAIL("debería estar inválida"); }

    free_page_table(pt);
    free_tlb(mi_tlb);
    free_frame_allocator(fa);
}

// =====================================================================
// Test 11: Workload uniform genera VPNs en rango
// =====================================================================
void test_workload_vpn(void) {
    printf("\n=== Workload VPN ===\n");

    sim_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.pages = 32;
    strcpy(conf.workload, "uniform");

    unsigned int seed = 42;
    int valid = 1;
    int N = 10000;

    for (int i = 0; i < N; i++) {
        int vpn = generate_vpn_page(&conf, &seed);
        if (vpn < 0 || vpn >= 32) { valid = 0; break; }
    }

    TEST("uniform: todos los VPN en [0, 31]");
    if (valid) { PASS(); } else { FAIL("VPN fuera de rango"); }

    // 80-20: verificar concentración
    strcpy(conf.workload, "80-20");
    seed = 42;
    int hot_count = 0;  // VPN 0-5 (20% de 32 ≈ 6)
    int hot_zone = 32 / 5;
    if (hot_zone < 1) hot_zone = 1;

    for (int i = 0; i < N; i++) {
        int vpn = generate_vpn_page(&conf, &seed);
        if (vpn < hot_zone) hot_count++;
    }

    double hot_pct = (double)hot_count / N * 100.0;
    TEST("80-20: ~80% accesos a hot zone (±5%)");
    if (hot_pct > 75.0 && hot_pct < 85.0) {
        printf(GREEN "[PASS] " RESET "(%.1f%%)\n", hot_pct);
        tests_passed++;
    } else {
        printf(RED "[FAIL] " RESET "hot=%.1f%%, esperado ~80%%\n", hot_pct);
        tests_failed++;
    }
}

// =====================================================================
// Test 12: TLB actualización (update existente)
// =====================================================================
void test_tlb_update(void) {
    printf("\n=== TLB Update ===\n");

    tlb *t = init_tlb(4);
    uint64_t frame;

    tlb_insert(t, 10, 5);
    TEST("VPN=10 → frame=5");
    if (tlb_lookup(t, 10, &frame) == 1 && frame == 5) { PASS(); } else { FAIL("incorrecto"); }

    // Actualizar VPN=10 con nuevo frame
    tlb_insert(t, 10, 99);
    TEST("actualizar VPN=10 → frame=99");
    if (tlb_lookup(t, 10, &frame) == 1 && frame == 99) { PASS(); } else { FAIL("no se actualizó"); }

    // Verificar que no se duplicó (count debe ser 1)
    TEST("no se duplicó (count == 1)");
    if (t->count == 1) { PASS(); } else { FAIL("se duplicó la entrada"); }

    free_tlb(t);
}

int main(void) {
    printf("========================================\n");
    printf("  TESTS DE PAGINACIÓN\n");
    printf("========================================\n");

    test_page_table();
    test_tlb_basic();
    test_tlb_fifo_replacement();
    test_tlb_invalidate();
    test_tlb_disabled();
    test_frame_allocator_basic();
    test_frame_allocator_eviction();
    test_traduccion_completa();
    test_dirty_bit();
    test_dirty_eviction();
    test_workload_vpn();
    test_tlb_update();

    printf("\n========================================\n");
    printf("  RESULTADO: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
