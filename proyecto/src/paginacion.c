// =====================================================================
// paginacion.c — Lógica de paginación
//
// Contiene:
//   - init_page_table / free_page_table
//   - traducir_pagina: el flujo completo TLB → PageTable → PageFault
//
// El flujo de traducción es:
//   1. Buscar en TLB (si está habilitada)
//   2. Si TLB MISS → buscar en tabla de páginas
//   3. Si página inválida → PAGE FAULT:
//      a. Pedir frame al frame_allocator
//      b. Si hubo eviction → invalidar tabla de páginas + TLB de la víctima
//      c. Simular carga desde disco (nanosleep 1-5 ms)
//      d. Actualizar tabla de páginas
//      e. Actualizar TLB
//   4. Calcular PA = frame × page_size + offset
// =====================================================================
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "../include/paginacion.h"
#include "../include/simulator.h"

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
    }
    return pt;
}

void free_page_table(page_table *pt) {
    if (pt) {
        free(pt->entries);
        free(pt);
    }
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
//   was_tlb_hit — [out] 1 si fue TLB hit, 0 si miss
//   was_page_fault — [out] 1 si hubo page fault
//   was_eviction   — [out] 1 si hubo eviction
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
                         int *was_tlb_hit, int *was_page_fault,
                         int *was_eviction) {
    uint64_t frame;
    *was_tlb_hit = 0;
    *was_page_fault = 0;
    *was_eviction = 0;

    // ─── PASO 1: Buscar en TLB ───
    if (tlb_lookup(mi_tlb, vpn, &frame)) {
        // TLB HIT → usamos el frame directamente (traducción rápida)
        *was_tlb_hit = 1;
        return frame * page_size + offset;
    }

    // ─── PASO 2: TLB MISS → Buscar en tabla de páginas ───
    if (pt->entries[vpn].valid) {
        // Página está en memoria → actualizar TLB con esta traducción
        frame = pt->entries[vpn].frame_number;
        tlb_insert(mi_tlb, vpn, frame);
        return frame * page_size + offset;
    }

    // ─── PASO 3: PAGE FAULT → La página no está en memoria ───
    *was_page_fault = 1;

    // 3a. Pedir un frame al allocator (puede causar eviction)
    int victim_thread;
    uint64_t victim_vpn;
    int new_frame = allocate_frame(fa, thread_id, vpn,
                                   &victim_thread, &victim_vpn, use_lock);

    // 3b. Si hubo eviction, invalidar la víctima
    if (victim_thread >= 0) {
        *was_eviction = 1;

        // Invalidar entrada en la tabla de páginas de la víctima
        if (victim_thread < num_threads && all_pts[victim_thread] != NULL) {
            all_pts[victim_thread]->entries[victim_vpn].valid = 0;
            all_pts[victim_thread]->entries[victim_vpn].frame_number = INVALID_FRAME;
        }

        // Invalidar en la TLB de la víctima
        if (victim_thread < num_threads && all_tlbs[victim_thread] != NULL) {
            tlb_invalidate_vpn(all_tlbs[victim_thread], victim_vpn);
        }
    }

    // 3c. Simular carga desde disco (nanosleep 1-5 ms)
    //     Esto representa el tiempo que tarda leer del almacenamiento secundario
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000 + (rand() % 4000000);  // 1ms + random(0..4ms) = 1-5ms
    nanosleep(&delay, NULL);

    // 3d. Actualizar tabla de páginas del thread actual
    pt->entries[vpn].frame_number = (uint64_t)new_frame;
    pt->entries[vpn].valid = 1;

    // 3e. Actualizar TLB con la nueva traducción
    tlb_insert(mi_tlb, vpn, (uint64_t)new_frame);

    // 3f. Calcular dirección física: PA = frame × page_size + offset
    return (uint64_t)new_frame * page_size + offset;
}