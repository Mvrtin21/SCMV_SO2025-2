// Autores:
//   - Martin Araya 21.781.369-7
//   - Benjamin Letelier 21.329.678-7
//
// =====================================================================
// paginacion.c — Lógica de paginación
//
// Contiene:
//   - init_page_table / free_page_table
//   - traducir_pagina: el flujo completo TLB → PageTable → PageFault
//
// El flujo de traducción es:
//   1. Buscar en TLB (si está habilitada) — acceso rápido (hardware)
//   2. Si TLB MISS → buscar en tabla de páginas (acceso a RAM, ~100ns)
//   3. Si página inválida → PAGE FAULT:
//      a. Pedir frame al frame_allocator
//      b. Si hubo eviction → verificar dirty bit de la víctima
//         - Si dirty → writeback a disco (nanosleep extra 2-5ms)
//         - Invalidar tabla de páginas + TLB de la víctima
//      c. Simular carga desde disco (nanosleep 1-5 ms)
//      d. Actualizar tabla de páginas (dirty = 0)
//      e. Si es escritura → marcar dirty = 1
//      f. Actualizar TLB
//   4. Calcular PA = frame × page_size + offset
// =====================================================================
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "../include/paginacion.h"
#include "../include/simulator.h"

// Delay simulado de acceso a RAM para consulta de tabla de páginas (~100ns)
// Esto modela que la TLB está en hardware (instantánea) mientras que la
// tabla de páginas reside en memoria principal (más lenta).
#define PAGE_TABLE_ACCESS_DELAY_NS 100

// =====================================================================
// init_page_table: Crea la tabla de páginas para un thread
//
// Todas las páginas empiezan INVÁLIDAS (valid = 0, frame = INVALID_FRAME).
// Esto significa que el primer acceso a cualquier página genera page fault.
// =====================================================================
page_table* init_page_table(int num_pages) {
    page_table *pt = malloc(sizeof(page_table));
    pt->num_pages = num_pages;
    pt->entries = malloc(num_pages * sizeof(page_table_entry));

    for (int i = 0; i < num_pages; i++) {
        pt->entries[i].frame_number = INVALID_FRAME;
        pt->entries[i].valid = 0;
        pt->entries[i].dirty = 0;
    }
    return pt;
}

void free_page_table(page_table *pt) {
    if (pt) {
        free(pt->entries);
        free(pt);
    }
}

// Función helper: simula un delay de acceso a RAM (tabla de páginas)
static void simulate_ram_delay(void) {
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = PAGE_TABLE_ACCESS_DELAY_NS;
    nanosleep(&delay, NULL);
}

// =====================================================================
// traducir_pagina: Traduce una dirección virtual a física en paginación
//
// Parámetros:
//   vpn         — Virtual Page Number (índice en la tabla de páginas)
//   offset      — Desplazamiento dentro de la página
//   pt          — Tabla de páginas del thread (privada)
//   mi_tlb      — TLB del thread (privada)
//   fa          — Frame Allocator (global, compartido)
//   thread_id   — ID del thread actual
//   all_pts     — Array de TODAS las tablas de páginas (para invalidar víctimas)
//   all_tlbs    — Array de TODAS las TLBs (para invalidar víctimas)
//   num_threads — Número total de threads
//   page_size   — Tamaño de página en bytes
//   use_lock    — 1 = modo SAFE, 0 = modo UNSAFE
//   is_write    — 1 = operación de escritura (marca dirty), 0 = lectura
//   was_tlb_hit — [out] 1 si fue TLB hit, 0 si miss
//   was_page_fault — [out] 1 si hubo page fault
//   was_eviction   — [out] 1 si hubo eviction
//   was_dirty_eviction — [out] 1 si la página evictada estaba dirty (writeback)
//
// Retorna: dirección física (PA)
// =====================================================================
uint64_t traducir_pagina(uint64_t vpn, uint64_t offset,
                         page_table *pt, tlb *mi_tlb,
                         frame_allocator *fa,
                         int thread_id,
                         page_table **all_pts, tlb **all_tlbs,
                         int num_threads,
                         int page_size, int use_lock,
                         int is_write,
                         int *was_tlb_hit, int *was_page_fault,
                         int *was_eviction, int *was_dirty_eviction) {
    uint64_t frame;
    *was_tlb_hit = 0;
    *was_page_fault = 0;
    *was_eviction = 0;
    *was_dirty_eviction = 0;

    // ─── PASO 1: Buscar en TLB ───
    // La TLB es hardware → acceso instantáneo (sin delay)
    if (tlb_lookup(mi_tlb, vpn, &frame)) {
        // TLB HIT → usamos el frame directamente (traducción rápida)
        *was_tlb_hit = 1;
        // Si es escritura, marcar la página como dirty
        if (is_write) {
            pt->entries[vpn].dirty = 1;
        }
        return frame * page_size + offset;
    }

    // ─── PASO 2: TLB MISS → Buscar en tabla de páginas (acceso a RAM) ───
    // Simular el costo de acceder a la tabla en memoria principal
    simulate_ram_delay();

    // Para evitar race conditions: tomamos el lock del FA ANTES de leer la
    // page table. Si no lo hacemos, otro thread podría invalidar nuestra
    // entrada entre el momento en que leemos valid=1 y cuando usamos el frame.
    // Esto serializa la lectura de la PT con la invalidación en allocate_frame.
    if (use_lock) pthread_mutex_lock(&fa->lock);

    if (pt->entries[vpn].valid) {
        // Página está en memoria → actualizar TLB con esta traducción
        frame = pt->entries[vpn].frame_number;
        if (use_lock) pthread_mutex_unlock(&fa->lock);

        tlb_insert(mi_tlb, vpn, frame);
        // Si es escritura, marcar dirty
        if (is_write) {
            pt->entries[vpn].dirty = 1;
        }
        return frame * page_size + offset;
    }

    // ─── PASO 3: PAGE FAULT → La página no está en memoria ───
    *was_page_fault = 1;

    // 3a. Pedir un frame al allocator (puede causar eviction)
    //     YA TENEMOS EL LOCK — pasamos use_lock=0 para no hacer double-lock
    //     La invalidación de la víctima ocurre DENTRO del lock del FA
    //     para evitar que el thread víctima lea entradas stale.
    int victim_thread;
    uint64_t victim_vpn;
    int new_frame = allocate_frame(fa, thread_id, vpn,
                                   &victim_thread, &victim_vpn,
                                   was_dirty_eviction,
                                   all_pts, all_tlbs, num_threads,
                                   0);  // use_lock=0 porque YA tenemos el lock

    // 3b. Actualizar tabla de páginas del thread actual (bajo el lock)
    pt->entries[vpn].frame_number = (uint64_t)new_frame;
    pt->entries[vpn].valid = 1;
    pt->entries[vpn].dirty = is_write ? 1 : 0;

    // Liberar el lock ANTES de los nanosleep (no queremos bloquear a todos
    // los threads durante la simulación de I/O)
    if (use_lock) pthread_mutex_unlock(&fa->lock);

    // 3c. Si hubo eviction, registrar y hacer writeback si dirty
    if (victim_thread >= 0) {
        *was_eviction = 1;

        // Si la víctima era dirty, hacer writeback a disco
        if (*was_dirty_eviction) {
            struct timespec wb_delay;
            wb_delay.tv_sec = 0;
            wb_delay.tv_nsec = 2000000 + (rand() % 3000000);  // 2-5ms writeback
            nanosleep(&wb_delay, NULL);
        }
    }

    // 3d. Simular carga desde disco (nanosleep 1-5 ms)
    //     Esto representa el tiempo que tarda leer del almacenamiento secundario
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000 + (rand() % 4000000);  // 1ms + random(0..4ms) = 1-5ms
    nanosleep(&delay, NULL);

    // 3e. Actualizar TLB con la nueva traducción
    tlb_insert(mi_tlb, vpn, (uint64_t)new_frame);

    // 3f. Calcular dirección física: PA = frame × page_size + offset
    return (uint64_t)new_frame * page_size + offset;
}