// Autores:
//   - Martin Araya 21.781.369-7
//   - Benjamin Letelier 21.329.678-7
//
// =====================================================================
// Generadores de workloads
// 
// Generan direcciones virtuales según un patrón de acceso:
//   - uniform: distribución uniforme (todos los segmentos/páginas igual)
//   - 80-20:   80% de accesos a 20% de los segmentos/páginas (localidad)
//
// Usan rand_r(&seed_local) para ser thread-safe y reproducibles.
// rand_r NO usa estado global, cada hilo tiene su propia semilla.
// =====================================================================
#include <stdlib.h>
#include <string.h>
#include "../include/simulator.h"
#include "../include/segmentacion.h"

// =====================================================================
// generate_address_seg: Genera UNA dirección virtual para segmentación
//
// Retorna un par (seg_id, offset) según el workload:
//   - uniform: segmento aleatorio uniforme, offset aleatorio uniforme
//   - 80-20:   80% de las veces elige de los primeros 20% de segmentos
//
// ¿De dónde salen los segfaults?
//   El offset se genera en [0, max_limit - 1], donde max_limit es el
//   MAYOR límite entre todos los segmentos. Así:
//     - Un segmento con limit=8192 nunca tendrá segfault (todo cabe)
//     - Un segmento con limit=1024 tendrá segfault ~87.5% de las veces
//       (porque 7168 de 8192 offsets posibles exceden su límite)
//   Los segfaults surgen NATURALMENTE de la diferencia entre el offset
//   generado y el límite de cada segmento. Los segmentos con límites
//   pequeños generan más segfaults → eso es lo que se analiza.
// =====================================================================
v_addr_seg generate_address_seg(sim_config *conf, segment_table *tabla, unsigned int *seed_local) {
    v_addr_seg addr;
    int num_seg = conf->segments;

    // Calcular el límite máximo entre todos los segmentos
    // Este será el rango de generación de offsets para TODOS los segmentos
    uint64_t max_limit = 0;
    for (int i = 0; i < num_seg; i++) {
        if (tabla->segments[i].limit > max_limit) {
            max_limit = tabla->segments[i].limit;
        }
    }

    if (strcmp(conf->workload, "80-20") == 0) {
        // --- Workload 80-20: simula ley de Pareto ---
        // 80% del tiempo accedemos al 20% de segmentos con MAYOR límite.
        // Esto modela que los segmentos más usados son los más grandes,
        // lo que reduce los segfaults respecto a uniform.
        int hot_count = num_seg / 5;             // 20% de los segmentos
        if (hot_count < 1) hot_count = 1;        // Al menos 1 segmento "caliente"

        int r = rand_r(seed_local) % 100;        // Número del 0 al 99
        if (r < 80) {
            // 80% → elegir de los segmentos "calientes" (últimos 20%, los más grandes)
            addr.seg_id = (num_seg - hot_count) + rand_r(seed_local) % hot_count;
        } else {
            // 20% → elegir del resto de segmentos (los más pequeños)
            if (num_seg - hot_count > 0) {
                addr.seg_id = rand_r(seed_local) % (num_seg - hot_count);
            } else {
                addr.seg_id = 0;
            }
        }
    } else {
        // --- Workload uniform: todos los segmentos igual de probables ---
        addr.seg_id = rand_r(seed_local) % num_seg;
    }

    // Generar offset en [0, max_limit - 1]
    // Si el offset generado >= limit del segmento elegido → segfault natural
    addr.offset = rand_r(seed_local) % max_limit;

    return addr;
}

// =====================================================================
// generate_vpn_page: Genera UN VPN (Virtual Page Number) para paginación
//
// Retorna un VPN en [0, num_pages-1] según el workload:
//   - uniform: todas las páginas igual de probables
//   - 80-20:   80% accesos al primer 20% de páginas
//
// El offset se genera por separado: rand_r(seed) % page_size
// =====================================================================
int generate_vpn_page(sim_config *conf, unsigned int *seed_local) {
    int num_pages = conf->pages;

    if (strcmp(conf->workload, "80-20") == 0) {
        int hot_count = num_pages / 5;
        if (hot_count < 1) hot_count = 1;

        int r = rand_r(seed_local) % 100;
        if (r < 80) {
            return rand_r(seed_local) % hot_count;
        } else {
            if (num_pages - hot_count > 0) {
                return hot_count + rand_r(seed_local) % (num_pages - hot_count);
            }
            return 0;
        }
    }

    // uniform
    return rand_r(seed_local) % num_pages;
}