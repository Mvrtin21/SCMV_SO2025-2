#ifndef SEGMENTACION_H
#define SEGMENTACION_H

#include <stdint.h>

typedef struct segment_entry {
    uint64_t base;
    uint64_t limit;
} segment_entry;

typedef struct segment_table {
    segment_entry *segments;
    int num_segments;
} segment_table;


uint64_t traducir_direccion(segment_table *tabla, int seg_id, uint64_t offset);
segment_table* init_segment_table(int num_segments, uint64_t *limits);

typedef struct {
    int seg_id;
    uint64_t offset;
} v_addr_seg;
#endif