# =====================================================================
# Makefile del Simulador Concurrente de Memoria Virtual
# Targets:
#   make / make all    → Compila el proyecto
#   make run           → Ejecuta ejemplo por defecto (segmentación)
#   make reproduce     → Ejecuta los 3 experimentos obligatorios
#   make clean         → Limpia archivos objeto y ejecutable
# =====================================================================

CC = gcc
CFLAGS = -std=c11 -pthread -Wall -Wextra -Iinclude
SRC = src/simulator.c src/segmentacion.c src/paginacion.c src/tlb.c src/frame_allocator.c src/workloads.c
TARGET = simulator

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ -lm

run: $(TARGET)
	./$(TARGET) --mode seg --threads 4 --workload uniform --ops-per-thread 5000 --stats

# --- Experimento 1: Segmentación con Segfaults Controlados ---
# --- Experimento 2: Impacto del TLB (Paginación) ---
# --- Experimento 3: Thrashing con Múltiples Threads ---
reproduce: $(TARGET)
	@mkdir -p out
	@echo "=== Experimento 1: Segmentación con Segfaults ==="
	./$(TARGET) --mode seg --threads 1 --workload uniform \
		--ops-per-thread 10000 --segments 4 \
		--seg-limits 1024,2048,4096,8192 --seed 100 --stats
	@cp out/summary.json out/summary_exp1.json
	@echo ""
	@echo "=== Experimento 2a: Paginación SIN TLB ==="
	./$(TARGET) --mode page --threads 1 --workload 80-20 \
		--ops-per-thread 50000 --pages 128 --frames 64 \
		--page-size 4096 --tlb-size 0 --tlb-policy fifo --seed 200 --stats
	@cp out/summary.json out/summary_exp2a.json
	@echo ""
	@echo "=== Experimento 2b: Paginación CON TLB ==="
	./$(TARGET) --mode page --threads 1 --workload 80-20 \
		--ops-per-thread 50000 --pages 128 --frames 64 \
		--page-size 4096 --tlb-size 32 --tlb-policy fifo --seed 200 --stats
	@cp out/summary.json out/summary_exp2b.json
	@echo ""
	@echo "=== Experimento 3a: Paginación 1 thread (thrashing) ==="
	./$(TARGET) --mode page --threads 1 --workload uniform \
		--ops-per-thread 10000 --pages 64 --frames 8 \
		--page-size 4096 --tlb-size 16 --seed 300 --stats
	@cp out/summary.json out/summary_exp3a.json
	@echo ""
	@echo "=== Experimento 3b: Paginación 8 threads (thrashing) ==="
	./$(TARGET) --mode page --threads 8 --workload uniform \
		--ops-per-thread 10000 --pages 64 --frames 8 \
		--page-size 4096 --tlb-size 16 --seed 300 --stats
	@cp out/summary.json out/summary_exp3b.json
	@echo "=== Todos los experimentos completados ==="

clean:
	rm -f $(TARGET)
	rm -rf out/

.PHONY: all run reproduce clean