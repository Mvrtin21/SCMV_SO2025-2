// =====================================================================
// paginacion.h — Definiciones para el modo de paginación
//
// Contiene las estructuras de:
//   - Tabla de páginas (por thread): mapea VPN → frame
//   - TLB (por thread): cache de traducciones recientes
//   - Frame Allocator (global): gestiona frames físicos compartidos
//   - Cola FIFO de eviction (global): decide qué página sacar
// =====================================================================
#ifndef PAGINACION_H
#define PAGINACION_H

#include <stdint.h>
#include <pthread.h>

// Valor especial que indica "esta página no está en memoria"
#define INVALID_FRAME ((uint64_t)-1)

// =====================================================================
// TABLA DE PÁGINAS (una por thread)
//
// Arreglo indexado por VPN (Virtual Page Number).
// Cada entrada dice en qué frame físico está esa página.
// Si valid == 0, la página NO está en memoria → page fault.
// =====================================================================
typedef struct {
    uint64_t frame_number;  // Frame físico donde está la página
    int valid;              // 1 = en memoria, 0 = no (page fault si se accede)
    int dirty;              // 1 = página modificada (escritura), 0 = limpia
} page_table_entry;

typedef struct {
    page_table_entry *entries;  // Arreglo de entradas, indexado por VPN
    int num_pages;              // Número total de páginas virtuales
} page_table;

// Funciones de tabla de páginas
page_table* init_page_table(int num_pages);
void free_page_table(page_table *pt);

// =====================================================================
// TLB — Translation Lookaside Buffer (una por thread)
//
// Es un arreglo circular que cachea traducciones VPN → frame.
// Política de reemplazo: FIFO (la entrada más antigua se sobreescribe).
// Cuando se evicta una página, hay que invalidar su entrada en la TLB.
// =====================================================================
typedef struct {
    uint64_t vpn;           // Virtual Page Number cacheado
    uint64_t frame_number;  // Frame físico correspondiente
    int valid;              // 1 = entrada activa, 0 = vacía
} tlb_entry;

typedef struct {
    tlb_entry *entries;     // Arreglo circular de entradas
    int size;               // Tamaño máximo de la TLB (0 = deshabilitada)
    int next_index;         // Índice circular para inserción FIFO
    int count;              // Número de entradas válidas actuales
} tlb;

// Funciones de TLB
tlb* init_tlb(int size);
void free_tlb(tlb *t);
int tlb_lookup(tlb *t, uint64_t vpn, uint64_t *frame_out);  // Retorna 1=hit, 0=miss
void tlb_insert(tlb *t, uint64_t vpn, uint64_t frame);       // Inserta con FIFO
void tlb_invalidate_vpn(tlb *t, uint64_t vpn);               // Invalida una entrada específica

// =====================================================================
// FRAME ALLOCATOR (global, compartido por todos los threads)
//
// Gestiona un pool de frames físicos. En modo SAFE se protege con mutex.
// Cuando no hay frames libres, se debe evictar usando la cola FIFO.
// =====================================================================

// Nodo de la cola FIFO de eviction: guarda qué thread, VPN y frame tiene cada página
typedef struct {
    int thread_id;          // Thread dueño de esta página
    uint64_t vpn;           // VPN de la página cargada en este frame
    int frame;              // Frame físico que ocupa esta página
} fifo_entry;

typedef struct {
    int *free_list;         // Arreglo de frames libres (stack simple)
    int free_count;         // Cuántos frames libres quedan
    int total_frames;       // Frames totales en el sistema

    // Cola FIFO para eviction: orden de carga de páginas
    fifo_entry *fifo_queue; // Cola circular
    int fifo_head;          // Índice de la entrada más antigua
    int fifo_tail;          // Índice donde insertar la siguiente
    int fifo_count;         // Cuántas entradas hay en la cola

    pthread_mutex_t lock;   // Mutex para acceso thread-safe (modo SAFE)
} frame_allocator;

// Funciones del frame allocator
frame_allocator* init_frame_allocator(int total_frames);
void free_frame_allocator(frame_allocator *fa);

// allocate_frame: obtiene un frame libre.
//   Si no hay frames libres, evicta la página más antigua (FIFO).
//   La invalidación de la víctima ocurre DENTRO del lock para evitar
//   race conditions donde otro thread lea entradas stale.
//   Retorna el frame asignado y llena victim_thread/victim_vpn.
//   was_dirty_eviction: [out] 1 si la víctima era dirty (necesitó writeback)
int allocate_frame(frame_allocator *fa, int thread_id, uint64_t vpn,
                   int *victim_thread, uint64_t *victim_vpn,
                   int *was_dirty_eviction,
                   page_table **all_pts, tlb **all_tlbs, int num_threads,
                   int use_lock);

// register_page: registra una página en la cola FIFO después de cargarla
void register_page_in_fifo(frame_allocator *fa, int frame, int thread_id, uint64_t vpn);

// Firma extendida de traducir_pagina con soporte dirty bit
uint64_t traducir_pagina(uint64_t vpn, uint64_t offset,
                         page_table *pt, tlb *mi_tlb,
                         frame_allocator *fa,
                         int thread_id,
                         page_table **all_pts, tlb **all_tlbs,
                         int num_threads,
                         int page_size, int use_lock,
                         int is_write,
                         int *was_tlb_hit, int *was_page_fault,
                         int *was_eviction, int *was_dirty_eviction);

#endif