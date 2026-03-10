// Lógica de segmentación
#include "../include/segmentacion.h"
#include <stdlib.h>

uint64_t traducir_direccion(struct segment_table *tabla, int seg_id, uint64_t offset) {
    if (offset >= tabla->segments[seg_id].limit) {
        return -1; // Segfault simulado
    }
    return tabla->segments[seg_id].base + offset;
}

segment_table* init_segment_table(int num_segments) {
    // 1. Reservamos la memoria para la estructura de la tabla
    segment_table *tabla = malloc(sizeof(segment_table));
    tabla->num_segments = num_segments;

    // 2. Reservamos la memoria para el arreglo dinámico de segmentos
    tabla->segments = malloc(num_segments * sizeof(segment_entry));

    // (Aquí asignaremos 'base' y 'limit' a cada segmento con un ciclo for)

    return tabla;
}