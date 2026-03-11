// Autores:
//   - Martin Araya 21.781.369-7
//   - Benjamin Letelier 21.329.678-7
//
#ifndef SIMULATOR_H
#define SIMULATOR_H
#include <stdint.h>  // Necesario para uint64_t
#include <pthread.h> // Necesario para pthread_mutex_t

// =====================================================================
// Estructura central para almacenar todos los parámetros de ejecución
// Aquí se concentra TODO lo que el usuario pasó por línea de comandos
// =====================================================================
typedef struct {
    // --- Parámetros comunes (ambos modos) ---
    char mode[5];             // "seg" o "page"
    int threads;              // Número de hilos/procesos simulados
    int ops_per_thread;       // Accesos a memoria que hace cada hilo
    char workload[20];        // "uniform" o "80-20"
    unsigned int seed;        // Semilla para reproducibilidad
    int unsafe;               // 1 = sin locks (modo UNSAFE), 0 = con locks (modo SAFE)
    int stats;                // 1 = imprimir reporte detallado al final

    // --- Parámetros de segmentación ---
    int segments;             // Número de segmentos por thread
    char seg_limits_str[256]; // Texto bruto "1024,2048,..." de la CLI
    uint64_t *seg_limits;     // Arreglo numérico de límites ya convertidos

    // --- Parámetros de paginación ---
    int pages;                // Número de páginas virtuales por thread (default: 64)
    int frames;               // Número de frames físicos globales (default: 32)
    int page_size;            // Tamaño de página en bytes (default: 4096)
    int tlb_size;             // Tamaño de TLB por thread, 0 = deshabilitada (default: 16)
    char tlb_policy[10];      // Política de reemplazo TLB: "fifo" (única disponible)
    char evict_policy[10];    // Política de eviction de páginas: "fifo" (única disponible)
} sim_config;

// =====================================================================
// Variables globales de métricas — compartidas por todos los hilos
// En modo SAFE se protegen con lock_metricas
// =====================================================================
extern uint64_t global_translations_ok;
extern uint64_t global_segfaults;

// Métricas de paginación
extern uint64_t global_tlb_hits;
extern uint64_t global_tlb_misses;
extern uint64_t global_page_faults;
extern uint64_t global_evictions;
extern uint64_t global_dirty_evictions;

// Mutex global para proteger las métricas compartidas
extern pthread_mutex_t lock_metricas;

// =====================================================================
// Estructura de argumentos que se le pasa a cada hilo al crearlo
// Contiene todo lo que el hilo necesita para trabajar independientemente
// =====================================================================
typedef struct {
    int thread_id;            // Identificador del hilo (0, 1, 2...)
    sim_config *config;       // Puntero a la configuración global
    // Métricas locales por thread (se suman a las globales al final)
    uint64_t local_translations_ok;
    uint64_t local_segfaults;
    uint64_t local_tlb_hits;
    uint64_t local_tlb_misses;
    uint64_t local_page_faults;
    uint64_t local_evictions;
    uint64_t local_dirty_evictions;
    double local_total_translation_time_ns; // Suma de tiempos de traducción
} thread_args;

#endif