// =====================================================================
// test_segmentacion.c — Tests unitarios para el módulo de segmentación
//
// Verifica:
//   1. Creación correcta de la tabla de segmentos
//   2. Traducción de direcciones válidas
//   3. Detección de segfaults (offset >= limit)
//   4. Múltiples segmentos con límites distintos
//   5. Generación de workloads (uniform y 80-20)
//
// Compilar:
//   gcc -std=c11 -Wall -Wextra -Iinclude -o test_seg tests/test_segmentacion.c \
//       src/segmentacion.c src/workloads.c -lm
//
// Ejecutar:
//   ./test_seg
// =====================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../include/segmentacion.h"
#include "../include/simulator.h"

// Colores para la salida
#define GREEN "\033[0;32m"
#define RED   "\033[0;31m"
#define RESET "\033[0m"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  TEST: %-50s ", name)
#define PASS() do { printf(GREEN "[PASS]" RESET "\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf(RED "[FAIL] %s" RESET "\n", msg); tests_failed++; } while(0)

// Declaración de funciones de workload (definidas en workloads.c)
extern v_addr_seg generate_address_seg(sim_config *conf, segment_table *tabla, unsigned int *seed_local);

// =====================================================================
// Test 1: Creación de tabla de segmentos
// =====================================================================
void test_init_segment_table(void) {
    printf("\n=== Tabla de Segmentos ===\n");

    // Test 1a: Crear tabla con 4 segmentos
    uint64_t limits[] = {1024, 2048, 4096, 8192};
    segment_table *tabla = init_segment_table(4, limits);

    TEST("tabla no es NULL");
    if (tabla != NULL) { PASS(); } else { FAIL("tabla es NULL"); return; }

    TEST("num_segments == 4");
    if (tabla->num_segments == 4) { PASS(); } else { FAIL("num_segments incorrecto"); }

    // Test 1b: Verificar que los límites se asignaron correctamente
    TEST("límite segmento 0 == 1024");
    if (tabla->segments[0].limit == 1024) { PASS(); } else { FAIL("limit[0] incorrecto"); }

    TEST("límite segmento 3 == 8192");
    if (tabla->segments[3].limit == 8192) { PASS(); } else { FAIL("limit[3] incorrecto"); }

    // Test 1c: Verificar que las bases son contiguas
    // base[0] = 0, base[1] = 1024, base[2] = 1024+2048 = 3072, base[3] = 3072+4096 = 7168
    TEST("base segmento 0 == 0");
    if (tabla->segments[0].base == 0) { PASS(); } else { FAIL("base[0] incorrecto"); }

    TEST("base segmento 1 == 1024");
    if (tabla->segments[1].base == 1024) { PASS(); } else { FAIL("base[1] incorrecto"); }

    TEST("base segmento 2 == 3072");
    if (tabla->segments[2].base == 3072) { PASS(); } else { FAIL("base[2] incorrecto"); }

    TEST("base segmento 3 == 7168");
    if (tabla->segments[3].base == 7168) { PASS(); } else { FAIL("base[3] incorrecto"); }

    free(tabla->segments);
    free(tabla);
}

// =====================================================================
// Test 2: Traducción de direcciones válidas
// =====================================================================
void test_traduccion_valida(void) {
    printf("\n=== Traducción Válida ===\n");

    uint64_t limits[] = {1024, 2048, 4096};
    segment_table *tabla = init_segment_table(3, limits);

    // PA = base + offset
    TEST("seg=0, offset=0 → PA=0");
    uint64_t pa = traducir_direccion(tabla, 0, 0);
    if (pa == 0) { PASS(); } else { FAIL("PA incorrecto"); }

    TEST("seg=0, offset=500 → PA=500");
    pa = traducir_direccion(tabla, 0, 500);
    if (pa == 500) { PASS(); } else { FAIL("PA incorrecto"); }

    TEST("seg=1, offset=0 → PA=1024");
    pa = traducir_direccion(tabla, 1, 0);
    if (pa == 1024) { PASS(); } else { FAIL("PA incorrecto"); }

    TEST("seg=1, offset=2047 → PA=3071 (al límite)");
    pa = traducir_direccion(tabla, 1, 2047);
    if (pa == 3071) { PASS(); } else { FAIL("PA incorrecto"); }

    TEST("seg=2, offset=0 → PA=3072");
    pa = traducir_direccion(tabla, 2, 0);
    if (pa == 3072) { PASS(); } else { FAIL("PA incorrecto"); }

    free(tabla->segments);
    free(tabla);
}

// =====================================================================
// Test 3: Detección de segfaults
// =====================================================================
void test_segfaults(void) {
    printf("\n=== Detección de Segfaults ===\n");

    uint64_t limits[] = {1024, 2048};
    segment_table *tabla = init_segment_table(2, limits);

    // Offset justo en el límite → segfault
    TEST("seg=0, offset=1024 → segfault (offset == limit)");
    uint64_t pa = traducir_direccion(tabla, 0, 1024);
    if (pa == (uint64_t)-1) { PASS(); } else { FAIL("no detectó segfault"); }

    // Offset mayor al límite → segfault
    TEST("seg=0, offset=5000 → segfault (offset > limit)");
    pa = traducir_direccion(tabla, 0, 5000);
    if (pa == (uint64_t)-1) { PASS(); } else { FAIL("no detectó segfault"); }

    // Offset=1023 → válido (último byte del segmento)
    TEST("seg=0, offset=1023 → válido (último byte)");
    pa = traducir_direccion(tabla, 0, 1023);
    if (pa != (uint64_t)-1) { PASS(); } else { FAIL("falso segfault"); }

    // Segmento 1 con offset mayor
    TEST("seg=1, offset=2048 → segfault");
    pa = traducir_direccion(tabla, 1, 2048);
    if (pa == (uint64_t)-1) { PASS(); } else { FAIL("no detectó segfault"); }

    TEST("seg=1, offset=2047 → válido");
    pa = traducir_direccion(tabla, 1, 2047);
    if (pa != (uint64_t)-1) { PASS(); } else { FAIL("falso segfault"); }

    free(tabla->segments);
    free(tabla);
}

// =====================================================================
// Test 4: Tasa de segfaults con distribución teórica
//
// Con limits = {1024, 8192} y max_limit = 8192:
//   - seg 0 (limit=1024): éxito = 1024/8192 = 12.5%
//   - seg 1 (limit=8192): éxito = 100%
//   Con uniform: tasa global = (12.5 + 100) / 2 = 56.25%
// =====================================================================
void test_tasa_segfaults_uniform(void) {
    printf("\n=== Tasa de Segfaults (uniform) ===\n");

    uint64_t limits[] = {1024, 8192};
    segment_table *tabla = init_segment_table(2, limits);

    sim_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.segments = 2;
    conf.seg_limits = limits;
    strcpy(conf.workload, "uniform");

    unsigned int seed = 42;
    int ok = 0, fail = 0;
    int N = 100000;

    for (int i = 0; i < N; i++) {
        v_addr_seg addr = generate_address_seg(&conf, tabla, &seed);
        uint64_t pa = traducir_direccion(tabla, addr.seg_id, addr.offset);
        if (pa == (uint64_t)-1) fail++;
        else ok++;
    }

    double tasa_exito = (double)ok / N * 100.0;
    // Teórico: 56.25%, tolerancia ±2%
    TEST("tasa de éxito uniform ≈ 56.25% (±2%)");
    if (tasa_exito > 54.0 && tasa_exito < 58.5) {
        printf(GREEN "[PASS] " RESET "(%.1f%%)\n", tasa_exito);
        tests_passed++;
    } else {
        printf(RED "[FAIL] " RESET "tasa=%.1f%%, esperado ~56.25%%\n", tasa_exito);
        tests_failed++;
    }

    TEST("ok + fail == N");
    if (ok + fail == N) { PASS(); } else { FAIL("suma no cuadra"); }

    free(tabla->segments);
    free(tabla);
}

// =====================================================================
// Test 5: Workload 80-20 concentra accesos
//
// Con 10 segmentos y 80-20: el 80% de accesos debería ir a los primeros
// 2 segmentos (20% de 10).
// =====================================================================
void test_workload_80_20(void) {
    printf("\n=== Workload 80-20 ===\n");

    uint64_t limits[10];
    for (int i = 0; i < 10; i++) limits[i] = 4096;
    segment_table *tabla = init_segment_table(10, limits);

    sim_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.segments = 10;
    conf.seg_limits = limits;
    strcpy(conf.workload, "80-20");

    unsigned int seed = 123;
    int hot_count = 0;  // Accesos a segmentos 8-9 (hot zone = últimos 20%)
    int N = 100000;

    for (int i = 0; i < N; i++) {
        v_addr_seg addr = generate_address_seg(&conf, tabla, &seed);
        if (addr.seg_id >= 8) hot_count++;  // Últimos 2 segmentos (20% de 10)
    }

    double hot_pct = (double)hot_count / N * 100.0;
    // Esperado: ~80% de accesos a los últimos 2 segmentos (±5%)
    TEST("80% accesos a hot zone (seg 8-9), (±5%)");
    if (hot_pct > 75.0 && hot_pct < 85.0) {
        printf(GREEN "[PASS] " RESET "(%.1f%%)\n", hot_pct);
        tests_passed++;
    } else {
        printf(RED "[FAIL] " RESET "hot=%.1f%%, esperado ~80%%\n", hot_pct);
        tests_failed++;
    }

    free(tabla->segments);
    free(tabla);
}

// =====================================================================
// Test 6: Reproducibilidad con semilla
// =====================================================================
void test_reproducibilidad(void) {
    printf("\n=== Reproducibilidad ===\n");

    uint64_t limits[] = {1024, 2048, 4096};
    segment_table *tabla = init_segment_table(3, limits);

    sim_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.segments = 3;
    conf.seg_limits = limits;
    strcpy(conf.workload, "uniform");

    // Generar secuencia con seed=42
    unsigned int seed1 = 42;
    v_addr_seg addrs1[100];
    for (int i = 0; i < 100; i++) {
        addrs1[i] = generate_address_seg(&conf, tabla, &seed1);
    }

    // Generar con la misma seed → debe ser idéntica
    unsigned int seed2 = 42;
    int equal = 1;
    for (int i = 0; i < 100; i++) {
        v_addr_seg a = generate_address_seg(&conf, tabla, &seed2);
        if (a.seg_id != addrs1[i].seg_id || a.offset != addrs1[i].offset) {
            equal = 0;
            break;
        }
    }

    TEST("misma seed → misma secuencia");
    if (equal) { PASS(); } else { FAIL("secuencias difieren"); }

    // Generar con seed diferente → debe ser distinta
    unsigned int seed3 = 99;
    int diff = 0;
    for (int i = 0; i < 100; i++) {
        v_addr_seg a = generate_address_seg(&conf, tabla, &seed3);
        if (a.seg_id != addrs1[i].seg_id || a.offset != addrs1[i].offset) {
            diff = 1;
            break;
        }
    }

    TEST("seed distinta → secuencia distinta");
    if (diff) { PASS(); } else { FAIL("secuencias iguales con seeds distintas"); }

    free(tabla->segments);
    free(tabla);
}

// =====================================================================
// Test 7: Tabla con un solo segmento
// =====================================================================
void test_single_segment(void) {
    printf("\n=== Segmento Único ===\n");

    uint64_t limits[] = {256};
    segment_table *tabla = init_segment_table(1, limits);

    TEST("único segmento, offset=0 → PA=0");
    if (traducir_direccion(tabla, 0, 0) == 0) { PASS(); } else { FAIL("PA incorrecto"); }

    TEST("único segmento, offset=255 → PA=255");
    if (traducir_direccion(tabla, 0, 255) == 255) { PASS(); } else { FAIL("PA incorrecto"); }

    TEST("único segmento, offset=256 → segfault");
    if (traducir_direccion(tabla, 0, 256) == (uint64_t)-1) { PASS(); } else { FAIL("no detectó segfault"); }

    free(tabla->segments);
    free(tabla);
}

int main(void) {
    printf("========================================\n");
    printf("  TESTS DE SEGMENTACIÓN\n");
    printf("========================================\n");

    test_init_segment_table();
    test_traduccion_valida();
    test_segfaults();
    test_tasa_segfaults_uniform();
    test_workload_80_20();
    test_reproducibilidad();
    test_single_segment();

    printf("\n========================================\n");
    printf("  RESULTADO: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
