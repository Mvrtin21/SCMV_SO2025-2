// =====================================================================
// frame_allocator.c — Administrador de frames físicos
//
// GLOBAL y COMPARTIDO por todos los threads.
// 
// Usa dos estructuras:
//   1. free_list (stack): frames disponibles para asignar
//   2. fifo_queue (cola circular): orden de carga de páginas
//      → la más antigua se evicta cuando no hay frames libres
//
// En modo SAFE, todo acceso se protege con un mutex.
// En modo UNSAFE, no se usa lock → carreras de datos.
// =====================================================================
#include <stdlib.h>
#include <stdio.h>
#include "../include/paginacion.h"

// =====================================================================
// init_frame_allocator: Crea el administrador con N frames
// Todos empiezan libres.
// =====================================================================
frame_allocator* init_frame_allocator(int total_frames) {
    
    // primero reservamos memoria para la estructura principal, osea 
    // para el frame_allocator en sí, que contiene los punteros a las 
    // otras estructuras y el mutex
    frame_allocator *fa = malloc(sizeof(frame_allocator));

    // Inicializar campos básicos, como el total de frames y el contador de frames libres
    fa->total_frames = total_frames;
    fa->free_count = total_frames;

    // Stack de frames libres, que son simplemente los números de frame (0, 1, 2, ...)
    fa->free_list = malloc(total_frames * sizeof(int));
    for (int i = 0; i < total_frames; i++) {
        fa->free_list[i] = i;
    }

    // Cola FIFO de eviction, que guarda qué thread, VPN y frame tiene cada página cargada
    fa->fifo_queue = malloc(total_frames * sizeof(fifo_entry));
    // todo inicia en cero, porque no hay páginas cargadas, así que head=tail=0 y count=0
    fa->fifo_head = 0;
    fa->fifo_tail = 0;
    fa->fifo_count = 0;

    pthread_mutex_init(&fa->lock, NULL);
    return fa;
}

void free_frame_allocator(frame_allocator *fa) {
    if (fa) {
        pthread_mutex_destroy(&fa->lock);
        free(fa->free_list);
        free(fa->fifo_queue);
        free(fa);
    }
}

// =====================================================================
// allocate_frame: Obtiene un frame físico
//
// Retorna el número de frame asignado (>= 0).
// Si hubo eviction, llena victim_thread y victim_vpn con la víctima.
// Si NO hubo eviction, victim_thread = -1.
//
// use_lock: 1 en modo SAFE, 0 en modo UNSAFE
// =====================================================================
int allocate_frame(frame_allocator *fa, int thread_id, uint64_t vpn,
                   int *victim_thread, uint64_t *victim_vpn, int use_lock) {
    if (use_lock) pthread_mutex_lock(&fa->lock);

    // use_lock o lock se refiere a si estamos en modo SAFE o UNSAFE, para decidir 
    // si usamos el mutex o no.

    int frame;
    *victim_thread = -1;

    if (fa->free_count > 0) {
        // Caso A: Hay frame libre → pop del stack
        fa->free_count--;
        frame = fa->free_list[fa->free_count];
    } else {
        // Caso B: No hay frames libres → eviction FIFO
        // Sacamos la página más antigua
        fifo_entry victim = fa->fifo_queue[fa->fifo_head];
        fa->fifo_head = (fa->fifo_head + 1) % fa->total_frames;
        fa->fifo_count--;

        *victim_thread = victim.thread_id;
        *victim_vpn = victim.vpn;
        frame = victim.frame;  // Reutilizamos su frame
    }

    // Registrar la nueva página en la cola FIFO
    fa->fifo_queue[fa->fifo_tail].thread_id = thread_id;
    fa->fifo_queue[fa->fifo_tail].vpn = vpn;
    fa->fifo_queue[fa->fifo_tail].frame = frame;
    fa->fifo_tail = (fa->fifo_tail + 1) % fa->total_frames;
    fa->fifo_count++;

    if (use_lock) pthread_mutex_unlock(&fa->lock);
    return frame;
}

// register_page_in_fifo ya no es necesaria — se hace dentro de allocate_frame
void register_page_in_fifo(frame_allocator *fa, int frame, int thread_id, uint64_t vpn) {
    (void)fa; (void)frame; (void)thread_id; (void)vpn;
    // No-op: la registración ahora ocurre dentro de allocate_frame
}