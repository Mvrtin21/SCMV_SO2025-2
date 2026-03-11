// =====================================================================
// tlb.c — Implementación de la TLB (Translation Lookaside Buffer)
//
// La TLB es un CACHE de traducciones VPN → frame.
// Funciona como un arreglo circular con política FIFO:
//   - Cuando se llena, la entrada más antigua se sobreescribe.
//   - Es PRIVADA de cada thread (no necesita locks).
//
// Flujo:
//   1. tlb_lookup() → ¿esta VPN está en la TLB? → HIT (rápido) o MISS
//   2. Si MISS → consultar tabla de páginas → tlb_insert() con el resultado
//   3. Si se evicta una página → tlb_invalidate_vpn() para limpiar
// =====================================================================
#include <stdlib.h>
#include <string.h>
#include "../include/paginacion.h"

// =====================================================================
// init_tlb: Crea una TLB de tamaño 'size'
//
// Si size == 0, la TLB está deshabilitada (siempre dará MISS).
// Todas las entradas comienzan inválidas (valid = 0).
// =====================================================================
tlb* init_tlb(int size) {
    tlb *t = malloc(sizeof(tlb));
    t->size = size;
    t->next_index = 0;  // FIFO: la próxima inserción va en posición 0
    t->count = 0;

    if (size > 0) {
        t->entries = calloc(size, sizeof(tlb_entry));
        // calloc inicializa todo a 0, así que valid = 0 automáticamente
    } else {
        t->entries = NULL;  // TLB deshabilitada
    }

    return t;
}

// =====================================================================
// free_tlb: Libera la memoria de la TLB
// =====================================================================
void free_tlb(tlb *t) {
    if (t) {
        free(t->entries);
        free(t);
    }
}

// =====================================================================
// tlb_lookup: Busca una VPN en la TLB
//
// Recorre TODAS las entradas buscando una que sea válida Y tenga esa VPN.
// Si la encuentra: escribe el frame en *frame_out y retorna 1 (HIT).
// Si no la encuentra: retorna 0 (MISS).
//
// ¿Por qué recorrer todo? Porque la TLB es pequeña (ej: 16-32 entradas)
// y en hardware real sería búsqueda asociativa (paralela).
// =====================================================================
int tlb_lookup(tlb *t, uint64_t vpn, uint64_t *frame_out) {
    if (t == NULL || t->size == 0) return 0;  // TLB deshabilitada

    for (int i = 0; i < t->size; i++) {
        if (t->entries[i].valid && t->entries[i].vpn == vpn) {
            *frame_out = t->entries[i].frame_number;
            return 1;  // HIT
        }
    }
    return 0;  // MISS
}

// =====================================================================
// tlb_insert: Inserta una traducción VPN → frame en la TLB
//
// Usa política FIFO con arreglo circular:
//   - next_index apunta a la posición donde insertar
//   - Después de insertar, next_index avanza (con módulo para dar vuelta)
//   - Si la TLB está llena, sobreescribe la entrada más antigua
//
// Ejemplo con TLB de tamaño 3:
//   Insertar A → [A, _, _]  next=1
//   Insertar B → [A, B, _]  next=2
//   Insertar C → [A, B, C]  next=0  (llena, da la vuelta)
//   Insertar D → [D, B, C]  next=1  (A fue la más antigua, se reemplaza)
// =====================================================================
void tlb_insert(tlb *t, uint64_t vpn, uint64_t frame) {
    if (t == NULL || t->size == 0) return;  // TLB deshabilitada

    // Primero verificar si ya existe (actualizar en lugar de duplicar)
    for (int i = 0; i < t->size; i++) {
        if (t->entries[i].valid && t->entries[i].vpn == vpn) {
            t->entries[i].frame_number = frame;
            return;
        }
    }

    // Insertar en la posición FIFO actual
    t->entries[t->next_index].vpn = vpn;
    t->entries[t->next_index].frame_number = frame;
    t->entries[t->next_index].valid = 1;

    // Avanzar el índice circular
    t->next_index = (t->next_index + 1) % t->size;

    if (t->count < t->size) t->count++;
}

// =====================================================================
// tlb_invalidate_vpn: Invalida una entrada específica por VPN
//
// Se llama cuando una página es EVICTADA de memoria física.
// Si esa VPN estaba en la TLB, debe marcarse como inválida
// para evitar que el thread use un frame que ya no le pertenece.
// =====================================================================
void tlb_invalidate_vpn(tlb *t, uint64_t vpn) {
    if (t == NULL || t->size == 0) return;

    for (int i = 0; i < t->size; i++) {
        if (t->entries[i].valid && t->entries[i].vpn == vpn) {
            t->entries[i].valid = 0;
            t->count--;
            return;
        }
    }
}