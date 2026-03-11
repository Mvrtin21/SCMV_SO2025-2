// Autores:
//   - Martin Araya 21.781.369-7
//   - Benjamin Letelier 21.329.678-7
//
// Programa principal
#define _POSIX_C_SOURCE 199309L  // Necesario para clock_gettime y nanosleep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>      // Para clock_gettime
#include <pthread.h>
#include "../include/simulator.h"
#include "../include/segmentacion.h"
#include "../include/paginacion.h"

// =====================================================================
// Variables globales de métricas (definidas aquí, declaradas extern en .h)
// =====================================================================
uint64_t global_translations_ok = 0;
uint64_t global_segfaults = 0;
uint64_t global_tlb_hits = 0;
uint64_t global_tlb_misses = 0;
uint64_t global_page_faults = 0;
uint64_t global_evictions = 0;
uint64_t global_dirty_evictions = 0;
pthread_mutex_t lock_metricas = PTHREAD_MUTEX_INITIALIZER;

extern v_addr_seg generate_address_seg(sim_config *conf, segment_table *tabla, unsigned int *seed_local);
extern int generate_vpn_page(sim_config *conf, unsigned int *seed_local);

// Declaración de traducir_pagina (definida en paginacion.c)
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
// Argumentos extra para el thread de paginación
// Necesita acceso a las estructuras de TODOS los threads (para eviction)
// =====================================================================
typedef struct {
    thread_args base;           // Argumentos base (thread_id, config, métricas)
    page_table *pt;             // Tabla de páginas de ESTE thread
    tlb *mi_tlb;                // TLB de ESTE thread
    frame_allocator *fa;        // Frame Allocator global (compartido)
    page_table **all_pts;       // Array de punteros a TODAS las tablas de páginas
    tlb **all_tlbs;             // Array de punteros a TODAS las TLBs
    int num_threads;            // Número total de threads
} page_thread_args;

// =====================================================================
// Rutina que ejecuta cada hilo en modo SEGMENTACIÓN
// Cada hilo:
//   1. Crea su propia tabla de segmentos (privada)
//   2. Genera ops_per_thread direcciones virtuales (seg_id, offset)
//   3. Traduce cada una a dirección física
//   4. Cuenta traducciones exitosas y segfaults
//   5. Mide el tiempo de cada traducción
// =====================================================================
void* run_segmentation_thread(void* arg) {
    thread_args *t_args = (thread_args*) arg;
    sim_config *conf = t_args->config;

    // Semilla local: base_seed + thread_id → secuencia distinta pero reproducible
    unsigned int seed_local = conf->seed + t_args->thread_id;

    // Cada hilo inicializa su PROPIA tabla de segmentos (privada, no compartida)
    segment_table *mi_tabla = init_segment_table(conf->segments, conf->seg_limits);

    // Ciclo principal: simular accesos a memoria
    for (int i = 0; i < conf->ops_per_thread; i++) {
        // Medir tiempo de traducción individual
        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        // Generar dirección virtual según workload
        v_addr_seg v_addr = generate_address_seg(conf, mi_tabla, &seed_local);
        // Traducir: PA = base[seg_id] + offset (o -1 si segfault)
        uint64_t p_addr = traducir_direccion(mi_tabla, v_addr.seg_id, v_addr.offset);

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double elapsed_ns = (t_end.tv_sec - t_start.tv_sec) * 1e9
                          + (t_end.tv_nsec - t_start.tv_nsec);
        t_args->local_total_translation_time_ns += elapsed_ns;

        // Actualizar métricas locales (sin lock, son privadas del hilo)
        if (p_addr == (uint64_t)-1) {
            t_args->local_segfaults++;
        } else {
            t_args->local_translations_ok++;
        }
    }

    // Sumar métricas locales a las globales (sección crítica)
    if (!conf->unsafe) pthread_mutex_lock(&lock_metricas);
    global_translations_ok += t_args->local_translations_ok;
    global_segfaults += t_args->local_segfaults;
    if (!conf->unsafe) pthread_mutex_unlock(&lock_metricas);

    // Limpieza: liberar la tabla de segmentos privada
    free(mi_tabla->segments);
    free(mi_tabla);

    return NULL;
}

// =====================================================================
// Rutina que ejecuta cada hilo en modo PAGINACIÓN
//
// Flujo por cada acceso a memoria:
//   1. Generar VPN y offset según workload
//   2. Llamar a traducir_pagina() que internamente hace:
//      TLB lookup → Page Table lookup → Page Fault (si necesario)
//   3. Registrar métricas (hits, misses, faults, evictions)
//   4. Medir tiempo de traducción
// =====================================================================
void* run_paginacion_thread(void* arg) {
    page_thread_args *p_args = (page_thread_args*) arg;
    thread_args *t_args = &p_args->base;
    sim_config *conf = t_args->config;

    // Semilla local reproducible
    unsigned int seed_local = conf->seed + t_args->thread_id;

    for (int i = 0; i < conf->ops_per_thread; i++) {
        // Medir tiempo
        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        // Generar VPN y offset
        int vpn = generate_vpn_page(conf, &seed_local);
        uint64_t offset = rand_r(&seed_local) % conf->page_size;

        // Determinar si es escritura (~30% de las operaciones son writes)
        int is_write = (rand_r(&seed_local) % 100) < 30;

        // Traducir
        int was_tlb_hit, was_page_fault, was_eviction, was_dirty_eviction;
        uint64_t pa = traducir_pagina(
            (uint64_t)vpn, offset,
            p_args->pt, p_args->mi_tlb,
            p_args->fa,
            t_args->thread_id,
            p_args->all_pts, p_args->all_tlbs,
            p_args->num_threads,
            conf->page_size,
            !conf->unsafe,    // use_lock = 1 en modo SAFE
            is_write,
            &was_tlb_hit, &was_page_fault, &was_eviction,
            &was_dirty_eviction
        );
        (void)pa;  // No usamos la dirección física, solo la calculamos

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double elapsed_ns = (t_end.tv_sec - t_start.tv_sec) * 1e9
                          + (t_end.tv_nsec - t_start.tv_nsec);
        t_args->local_total_translation_time_ns += elapsed_ns;

        // Métricas locales
        if (was_tlb_hit) {
            t_args->local_tlb_hits++;
        } else {
            t_args->local_tlb_misses++;
        }
        if (was_page_fault) t_args->local_page_faults++;
        if (was_eviction) t_args->local_evictions++;
        if (was_dirty_eviction) t_args->local_dirty_evictions++;
        t_args->local_translations_ok++;
    }

    // Sumar métricas locales a las globales
    if (!conf->unsafe) pthread_mutex_lock(&lock_metricas);
    global_tlb_hits += t_args->local_tlb_hits;
    global_tlb_misses += t_args->local_tlb_misses;
    global_page_faults += t_args->local_page_faults;
    global_evictions += t_args->local_evictions;
    global_dirty_evictions += t_args->local_dirty_evictions;
    global_translations_ok += t_args->local_translations_ok;
    if (!conf->unsafe) pthread_mutex_unlock(&lock_metricas);

    return NULL;
}

int main(int argc, char *argv[]) {
    // 1. Valores por defecto según el enunciado del laboratorio
    sim_config config = {
        .mode = "",
        .threads = 1,
        .ops_per_thread = 1000,
        .workload = "uniform",    // Default: distribución uniforme
        .seed = 42,               // Default: semilla 42
        .unsafe = 0,              // Default: modo SAFE (con locks)
        .stats = 0,               // Default: no imprimir reporte detallado
        // Segmentación
        .segments = 4,
        .seg_limits_str = "",
        .seg_limits = NULL,
        // Paginación
        .pages = 64,
        .frames = 32,
        .page_size = 4096,
        .tlb_size = 16,
        .tlb_policy = "fifo",
        .evict_policy = "fifo"
    };

    // 2. Tabla de opciones largas (--nombre) → cada una se mapea a una letra
    struct option long_options[] = {
        // Comunes
        {"mode",           required_argument, 0, 'm'},
        {"threads",        required_argument, 0, 't'},
        {"ops-per-thread", required_argument, 0, 'o'},
        {"workload",       required_argument, 0, 'w'},
        {"seed",           required_argument, 0, 'S'},
        {"unsafe",         no_argument,       0, 'u'},
        {"stats",          no_argument,       0, 'R'},
        // Segmentación
        {"segments",       required_argument, 0, 's'},
        {"seg-limits",     required_argument, 0, 'l'},
        // Paginación
        {"pages",          required_argument, 0, 'p'},
        {"frames",         required_argument, 0, 'f'},
        {"page-size",      required_argument, 0, 'z'},
        {"tlb-size",       required_argument, 0, 'T'},
        {"tlb-policy",     required_argument, 0, 'P'},
        {"evict-policy",   required_argument, 0, 'E'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    // 3. Ciclo de lectura de argumentos
    while ((opt = getopt_long(argc, argv, "m:t:o:w:S:us:l:p:f:z:T:P:E:R", long_options, &option_index)) != -1) {
        switch (opt) {
            // --- Comunes ---
            case 'm': strcpy(config.mode, optarg); break;
            case 't': config.threads = atoi(optarg); break;
            case 'o': config.ops_per_thread = atoi(optarg); break;
            case 'w': strcpy(config.workload, optarg); break;
            case 'S': config.seed = (unsigned int)atoi(optarg); break;
            case 'u': config.unsafe = 1; break;
            case 'R': config.stats = 1; break;
            // --- Segmentación ---
            case 's': config.segments = atoi(optarg); break;
            case 'l': strcpy(config.seg_limits_str, optarg); break;
            // --- Paginación ---
            case 'p': config.pages = atoi(optarg); break;
            case 'f': config.frames = atoi(optarg); break;
            case 'z': config.page_size = atoi(optarg); break;
            case 'T': config.tlb_size = atoi(optarg); break;
            case 'P': strcpy(config.tlb_policy, optarg); break;
            case 'E': strcpy(config.evict_policy, optarg); break;
            default:
                fprintf(stderr, "Opción desconocida: %c\n", opt);
                exit(EXIT_FAILURE);
        }
    }

    // -------------------------------------------------------------------
    // PROCESAMIENTO DE LOS LÍMITES DE SEGMENTOS
    // Se hace DESPUÉS del while porque necesitamos saber config.segments
    // -------------------------------------------------------------------
    config.seg_limits = malloc(config.segments * sizeof(uint64_t));
    for (int i = 0; i < config.segments; i++) {
        config.seg_limits[i] = 4096; // Default: 4096 bytes por segmento
    }
    if (strlen(config.seg_limits_str) > 0) {
        // Primero contamos cuántos valores hay separados por comas
        int count = 1;
        for (int i = 0; config.seg_limits_str[i] != '\0'; i++) {
            if (config.seg_limits_str[i] == ',') count++;
        }

        // Validar que la cantidad de límites coincida con --segments
        if (count != config.segments) {
            fprintf(stderr, "Error: --seg-limits tiene %d valores pero --segments es %d.\n"
                            "       Deben coincidir. Ejemplo: --segments 4 --seg-limits 1024,2048,4096,8192\n",
                    count, config.segments);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }

        int i = 0;
        char *token = strtok(config.seg_limits_str, ",");
        while (token != NULL && i < config.segments) {
            config.seg_limits[i] = strtoull(token, NULL, 10);
            if (config.seg_limits[i] == 0) {
                fprintf(stderr, "Error: el límite del segmento %d no puede ser 0.\n", i);
                free(config.seg_limits);
                exit(EXIT_FAILURE);
            }
            token = strtok(NULL, ",");
            i++;
        }
    }

    // Inicializar semilla global
    srand(config.seed);

    // -------------------------------------------------------------------
    // VALIDACIÓN DE PARÁMETROS — "a prueba de tontos"
    // Verificamos TODOS los parámetros antes de ejecutar cualquier cosa
    // -------------------------------------------------------------------

    // [1] Modo obligatorio: debe ser "seg" o "page"
    if (strcmp(config.mode, "seg") != 0 && strcmp(config.mode, "page") != 0) {
        fprintf(stderr, "Error: --mode es obligatorio y debe ser 'seg' o 'page'.\n"
                        "  Uso: ./simulator --mode seg ... o ./simulator --mode page ...\n");
        free(config.seg_limits);
        exit(EXIT_FAILURE);
    }

    // [2] Threads: debe ser >= 1
    if (config.threads < 1) {
        fprintf(stderr, "Error: --threads debe ser >= 1 (recibido: %d).\n", config.threads);
        free(config.seg_limits);
        exit(EXIT_FAILURE);
    }

    // [3] Operaciones por thread: debe ser >= 1
    if (config.ops_per_thread < 1) {
        fprintf(stderr, "Error: --ops-per-thread debe ser >= 1 (recibido: %d).\n", config.ops_per_thread);
        free(config.seg_limits);
        exit(EXIT_FAILURE);
    }

    // [4] Workload: solo "uniform" o "80-20"
    if (strcmp(config.workload, "uniform") != 0 && strcmp(config.workload, "80-20") != 0) {
        fprintf(stderr, "Error: --workload debe ser 'uniform' o '80-20' (recibido: '%s').\n", config.workload);
        free(config.seg_limits);
        exit(EXIT_FAILURE);
    }

    // [5] Validaciones específicas de segmentación
    if (strcmp(config.mode, "seg") == 0) {
        if (config.segments < 1) {
            fprintf(stderr, "Error: --segments debe ser >= 1 (recibido: %d).\n", config.segments);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
    }

    // [6] Validaciones específicas de paginación
    if (strcmp(config.mode, "page") == 0) {
        if (config.pages < 1) {
            fprintf(stderr, "Error: --pages debe ser >= 1 (recibido: %d).\n", config.pages);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
        if (config.frames < 1) {
            fprintf(stderr, "Error: --frames debe ser >= 1 (recibido: %d).\n", config.frames);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
        if (config.page_size < 1) {
            fprintf(stderr, "Error: --page-size debe ser >= 1 (recibido: %d).\n", config.page_size);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
        if (config.tlb_size < 0) {
            fprintf(stderr, "Error: --tlb-size debe ser >= 0 (recibido: %d). Use 0 para deshabilitar.\n", config.tlb_size);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
        if (strcmp(config.tlb_policy, "fifo") != 0) {
            fprintf(stderr, "Error: --tlb-policy solo soporta 'fifo' (recibido: '%s').\n", config.tlb_policy);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
        if (strcmp(config.evict_policy, "fifo") != 0) {
            fprintf(stderr, "Error: --evict-policy solo soporta 'fifo' (recibido: '%s').\n", config.evict_policy);
            free(config.seg_limits);
            exit(EXIT_FAILURE);
        }
    }

    // -------------------------------------------------------------------
    // DESPACHO POR MODO
    // -------------------------------------------------------------------
    if (strcmp(config.mode, "seg") == 0) {
        printf("\n--- INICIANDO MODO SEGMENTACIÓN ---\n");

        // Medir tiempo total de ejecución
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        pthread_t hilos[config.threads];
        thread_args args[config.threads];

        // Crear los hilos
        for (int i = 0; i < config.threads; i++) {
            args[i].thread_id = i;
            args[i].config = &config;
            args[i].local_translations_ok = 0;
            args[i].local_segfaults = 0;
            args[i].local_total_translation_time_ns = 0;

            if (pthread_create(&hilos[i], NULL, run_segmentation_thread, &args[i]) != 0) {
                fprintf(stderr, "Error al crear el hilo %d\n", i);
                exit(EXIT_FAILURE);
            }
        }

        // Esperar a que todos terminen
        for (int i = 0; i < config.threads; i++) {
            pthread_join(hilos[i], NULL);
        }

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double elapsed_sec = (end_time.tv_sec - start_time.tv_sec)
                           + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

        uint64_t total_ops = (uint64_t)config.threads * config.ops_per_thread;
        double throughput = total_ops / elapsed_sec;

        // Calcular tiempo promedio de traducción
        double total_translation_ns = 0;
        for (int i = 0; i < config.threads; i++) {
            total_translation_ns += args[i].local_total_translation_time_ns;
        }
        double avg_translation_ns = total_translation_ns / total_ops;

        printf("--- TODOS LOS HILOS TERMINARON ---\n");

        // -------------------------------------------------------------------
        // REPORTE --stats (formato exacto del enunciado)
        // -------------------------------------------------------------------
        if (config.stats) {
            printf("========================================\n");
            printf("SIMULADOR DE MEMORIA VIRTUAL\n");
            printf("Modo: SEGMENTACIÓN\n");
            printf("========================================\n");
            printf("Configuración:\n");
            printf("  Threads: %d\n", config.threads);
            printf("  Ops por thread: %d\n", config.ops_per_thread);
            printf("  Workload: %s\n", config.workload);
            printf("  Seed: %u\n", config.seed);
            printf("  Segmentos: %d\n", config.segments);
            printf("  Límites: ");
            for (int i = 0; i < config.segments; i++) {
                printf("%lu%s", config.seg_limits[i], (i < config.segments - 1) ? "," : "");
            }
            printf("\n");
            printf("  Modo: %s\n", config.unsafe ? "UNSAFE" : "SAFE");
            printf("\nMétricas Globales:\n");
            printf("  translations_ok: %lu\n", global_translations_ok);
            printf("  segfaults: %lu\n", global_segfaults);
            printf("  avg_translation_time_ns: %.2f\n", avg_translation_ns);
            printf("  throughput_ops_sec: %.2f\n", throughput);
            printf("\nMétricas por Thread:\n");
            for (int i = 0; i < config.threads; i++) {
                printf("  Thread %d: translations_ok=%lu, segfaults=%lu\n",
                       i, args[i].local_translations_ok, args[i].local_segfaults);
            }
            printf("\nTiempo total: %.2f segundos\n", elapsed_sec);
            printf("Throughput: %.2f ops/seg\n", throughput);
            printf("========================================\n");
        }

        // -------------------------------------------------------------------
        // Generar out/summary.json
        // -------------------------------------------------------------------
        system("mkdir -p out");
        FILE *fp = fopen("out/summary.json", "w");
        if (fp) {
            fprintf(fp, "{\n");
            fprintf(fp, "  \"mode\": \"seg\",\n");
            fprintf(fp, "  \"config\": {\n");
            fprintf(fp, "    \"threads\": %d,\n", config.threads);
            fprintf(fp, "    \"ops_per_thread\": %d,\n", config.ops_per_thread);
            fprintf(fp, "    \"workload\": \"%s\",\n", config.workload);
            fprintf(fp, "    \"seed\": %u,\n", config.seed);
            fprintf(fp, "    \"unsafe\": %s,\n", config.unsafe ? "true" : "false");
            fprintf(fp, "    \"segments\": %d\n", config.segments);
            fprintf(fp, "  },\n");
            fprintf(fp, "  \"metrics\": {\n");
            fprintf(fp, "    \"translations_ok\": %lu,\n", global_translations_ok);
            fprintf(fp, "    \"segfaults\": %lu,\n", global_segfaults);
            fprintf(fp, "    \"avg_translation_time_ns\": %.2f,\n", avg_translation_ns);
            fprintf(fp, "    \"throughput_ops_sec\": %.2f\n", throughput);
            fprintf(fp, "  },\n");
            fprintf(fp, "  \"runtime_sec\": %.3f\n", elapsed_sec);
            fprintf(fp, "}\n");
            fclose(fp);
        }

    }
    else if (strcmp(config.mode, "page") == 0) {
        printf("\n--- INICIANDO MODO PAGINACIÓN ---\n");

        // Medir tiempo total
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // Crear el Frame Allocator global (compartido por todos los threads)
        frame_allocator *fa = init_frame_allocator(config.frames);

        // Arrays globales de tablas de páginas y TLBs (para que eviction pueda
        // invalidar la tabla/TLB de CUALQUIER thread, no solo del actual)
        page_table **all_pts = malloc(config.threads * sizeof(page_table*));
        tlb **all_tlbs = malloc(config.threads * sizeof(tlb*));

        // Crear estructuras privadas de cada thread
        for (int i = 0; i < config.threads; i++) {
            all_pts[i] = init_page_table(config.pages);
            all_tlbs[i] = init_tlb(config.tlb_size);
        }

        // Crear argumentos y threads
        pthread_t hilos[config.threads];
        page_thread_args p_args[config.threads];

        for (int i = 0; i < config.threads; i++) {
            p_args[i].base.thread_id = i;
            p_args[i].base.config = &config;
            p_args[i].base.local_translations_ok = 0;
            p_args[i].base.local_segfaults = 0;
            p_args[i].base.local_tlb_hits = 0;
            p_args[i].base.local_tlb_misses = 0;
            p_args[i].base.local_page_faults = 0;
            p_args[i].base.local_evictions = 0;
            p_args[i].base.local_dirty_evictions = 0;
            p_args[i].base.local_total_translation_time_ns = 0;
            p_args[i].pt = all_pts[i];
            p_args[i].mi_tlb = all_tlbs[i];
            p_args[i].fa = fa;
            p_args[i].all_pts = all_pts;
            p_args[i].all_tlbs = all_tlbs;
            p_args[i].num_threads = config.threads;

            if (pthread_create(&hilos[i], NULL, run_paginacion_thread, &p_args[i]) != 0) {
                fprintf(stderr, "Error al crear el hilo %d\n", i);
                exit(EXIT_FAILURE);
            }
        }

        // Esperar a que todos terminen
        for (int i = 0; i < config.threads; i++) {
            pthread_join(hilos[i], NULL);
        }

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double elapsed_sec = (end_time.tv_sec - start_time.tv_sec)
                           + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

        uint64_t total_ops = (uint64_t)config.threads * config.ops_per_thread;
        double throughput = total_ops / elapsed_sec;

        double total_translation_ns = 0;
        for (int i = 0; i < config.threads; i++) {
            total_translation_ns += p_args[i].base.local_total_translation_time_ns;
        }
        double avg_translation_ns = total_translation_ns / total_ops;

        double hit_rate = (global_tlb_hits + global_tlb_misses > 0)
            ? (double)global_tlb_hits / (global_tlb_hits + global_tlb_misses) : 0.0;

        printf("--- TODOS LOS HILOS TERMINARON ---\n");

        // -------------------------------------------------------------------
        // REPORTE --stats para paginación
        // -------------------------------------------------------------------
        if (config.stats) {
            printf("========================================\n");
            printf("SIMULADOR DE MEMORIA VIRTUAL\n");
            printf("Modo: PAGINACIÓN\n");
            printf("========================================\n");
            printf("Configuración:\n");
            printf("  Threads: %d\n", config.threads);
            printf("  Ops por thread: %d\n", config.ops_per_thread);
            printf("  Workload: %s\n", config.workload);
            printf("  Seed: %u\n", config.seed);
            printf("  Páginas: %d\n", config.pages);
            printf("  Frames: %d\n", config.frames);
            printf("  Page size: %d\n", config.page_size);
            printf("  TLB size: %d\n", config.tlb_size);
            printf("  TLB policy: %s\n", config.tlb_policy);
            printf("  Evict policy: %s\n", config.evict_policy);
            printf("  Modo: %s\n", config.unsafe ? "UNSAFE" : "SAFE");
            printf("\nMétricas Globales:\n");
            printf("  tlb_hits: %lu\n", global_tlb_hits);
            printf("  tlb_misses: %lu\n", global_tlb_misses);
            printf("  hit_rate: %.3f\n", hit_rate);
            printf("  page_faults: %lu\n", global_page_faults);
            printf("  evictions: %lu\n", global_evictions);
            printf("  dirty_evictions: %lu\n", global_dirty_evictions);
            printf("  avg_translation_time_ns: %.2f\n", avg_translation_ns);
            printf("  throughput_ops_sec: %.2f\n", throughput);
            printf("\nMétricas por Thread:\n");
            for (int i = 0; i < config.threads; i++) {
                printf("  Thread %d: tlb_hits=%lu, tlb_misses=%lu, page_faults=%lu, evictions=%lu, dirty_evictions=%lu\n",
                       i,
                       p_args[i].base.local_tlb_hits,
                       p_args[i].base.local_tlb_misses,
                       p_args[i].base.local_page_faults,
                       p_args[i].base.local_evictions,
                       p_args[i].base.local_dirty_evictions);
            }
            printf("\nTiempo total: %.2f segundos\n", elapsed_sec);
            printf("Throughput: %.2f ops/seg\n", throughput);
            printf("========================================\n");
        }

        // Generar out/summary.json para paginación
        system("mkdir -p out");
        FILE *fp = fopen("out/summary.json", "w");
        if (fp) {
            fprintf(fp, "{\n");
            fprintf(fp, "  \"mode\": \"page\",\n");
            fprintf(fp, "  \"config\": {\n");
            fprintf(fp, "    \"threads\": %d,\n", config.threads);
            fprintf(fp, "    \"ops_per_thread\": %d,\n", config.ops_per_thread);
            fprintf(fp, "    \"workload\": \"%s\",\n", config.workload);
            fprintf(fp, "    \"seed\": %u,\n", config.seed);
            fprintf(fp, "    \"unsafe\": %s,\n", config.unsafe ? "true" : "false");
            fprintf(fp, "    \"pages\": %d,\n", config.pages);
            fprintf(fp, "    \"frames\": %d,\n", config.frames);
            fprintf(fp, "    \"page_size\": %d,\n", config.page_size);
            fprintf(fp, "    \"tlb_size\": %d,\n", config.tlb_size);
            fprintf(fp, "    \"tlb_policy\": \"%s\",\n", config.tlb_policy);
            fprintf(fp, "    \"evict_policy\": \"%s\"\n", config.evict_policy);
            fprintf(fp, "  },\n");
            fprintf(fp, "  \"metrics\": {\n");
            fprintf(fp, "    \"tlb_hits\": %lu,\n", global_tlb_hits);
            fprintf(fp, "    \"tlb_misses\": %lu,\n", global_tlb_misses);
            fprintf(fp, "    \"hit_rate\": %.3f,\n", hit_rate);
            fprintf(fp, "    \"page_faults\": %lu,\n", global_page_faults);
            fprintf(fp, "    \"evictions\": %lu,\n", global_evictions);
            fprintf(fp, "    \"dirty_evictions\": %lu,\n", global_dirty_evictions);
            fprintf(fp, "    \"avg_translation_time_ns\": %.2f,\n", avg_translation_ns);
            fprintf(fp, "    \"throughput_ops_sec\": %.2f\n", throughput);
            fprintf(fp, "  },\n");
            fprintf(fp, "  \"runtime_sec\": %.3f\n", elapsed_sec);
            fprintf(fp, "}\n");
            fclose(fp);
        }

        // Limpieza de paginación
        for (int i = 0; i < config.threads; i++) {
            free_page_table(all_pts[i]);
            free_tlb(all_tlbs[i]);
        }
        free(all_pts);
        free(all_tlbs);
        free_frame_allocator(fa);
    }
    else {
        fprintf(stderr, "Error: debe especificar --mode seg o --mode page\n");
        exit(EXIT_FAILURE);
    }

    // Limpieza global final
    free(config.seg_limits);

    return 0;
}