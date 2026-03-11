// Autores:
//   - Martin Araya 21.781.369-7
//   - Benjamin Letelier 21.329.678-7
//
// Lógica de segmentación
#include "../include/segmentacion.h"
#include <stdlib.h>

uint64_t traducir_direccion(struct segment_table *tabla, int seg_id, uint64_t offset) {
    if (offset >= tabla->segments[seg_id].limit) {
        return -1; // Segfault simulado
    }
    return tabla->segments[seg_id].base + offset;
}

segment_table* init_segment_table(int num_segments, uint64_t *limits){
    // 1. Reservamos la memoria para la estructura de la tabla
    segment_table *tabla = malloc(sizeof(segment_table));
    tabla->num_segments = num_segments;

    // 2. Reservamos la memoria para el arreglo dinámico de segmentos
    tabla->segments = malloc(num_segments * sizeof(segment_entry));

    // (Aquí asignaremos 'base' y 'limit' a cada segmento con un ciclo for)
    uint64_t base_actual = 0;
    for (int i = 0; i < num_segments; i++) {
        tabla->segments[i].base = base_actual;
        tabla->segments[i].limit = limits[i]; // Asignamos el límite dinámico
        base_actual += limits[i];             // Calculamos la siguiente base
    }

    return tabla;
}