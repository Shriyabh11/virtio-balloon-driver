/*
 * workload_sim.c — CPU + GPU workload simulator for memory pressure testing
 *
 * Generates realistic memory pressure inside a guest VM by simulating:
 *   - CPU tasks that allocate and hold memory buffers
 *   - GPU-style warp kernel launches with divergence and coalescing analysis
 *
 * The combined memory footprint triggers the balloon driver's pressure-aware
 * deflation logic, demonstrating the full feedback loop:
 *   workload starts → free memory drops → balloon deflates → VM stabilizes
 *
 * Usage:
 *   ./workload_sim                    # default: 8 CPU tasks, 4 GPU kernels
 *   ./workload_sim --cpu-tasks 16     # heavier CPU pressure
 *   ./workload_sim --gpu-kernels 8    # more GPU launches
 *   ./workload_sim --alloc-mb 64      # memory per CPU task (MB)
 *
 * Author: Shriya Bharadwaj
 * License: GPL-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

/* ────────────────────────────────────────────────────────────────────────────
 * GPU Simulation Parameters
 * ──────────────────────────────────────────────────────────────────────────── */

#define WARP_SIZE          32     /* threads per warp (NVIDIA standard)       */
#define MAX_WARPS_PER_SM   48     /* max concurrent warps per SM              */
#define CACHE_LINE_BYTES   128    /* L2 cache line size for coalescing model  */
#define GPU_CLOCK_MHZ      1500   /* simulated GPU clock for cycle estimates  */

/* Simulated GPU kernel descriptor */
struct gpu_kernel {
    const char *name;
    uint32_t    total_threads;
    uint32_t    shared_mem_bytes;
    float       branch_divergence;  /* 0.0 = no divergence, 1.0 = worst case */
    uint32_t    mem_access_stride;  /* bytes between thread accesses          */
};

/* GPU execution metrics for a single kernel launch */
struct gpu_metrics {
    uint32_t warps_launched;
    uint32_t active_warps;
    uint32_t total_mem_transactions;
    uint32_t coalesced_transactions;
    uint32_t uncoalesced_transactions;
    float    divergence_ratio;
    float    coalescing_efficiency;
    uint64_t estimated_cycles;
    double   estimated_time_us;
};

/* ────────────────────────────────────────────────────────────────────────────
 * CPU Task Parameters
 * ──────────────────────────────────────────────────────────────────────────── */

struct cpu_task {
    int      id;
    size_t   alloc_bytes;
    void    *buffer;
    double   exec_time_ms;
};

/* ────────────────────────────────────────────────────────────────────────────
 * Global State
 * ──────────────────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Memory Pressure Reporting
 * ──────────────────────────────────────────────────────────────────────────── */

static void report_memory_state(const char *label) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) return;

    uint64_t total_mb = (uint64_t)si.totalram * si.mem_unit / (1024 * 1024);
    uint64_t free_mb  = (uint64_t)si.freeram  * si.mem_unit / (1024 * 1024);
    uint64_t used_mb  = total_mb - free_mb;

    printf("  [mem] %-20s  total=%4lu MB  used=%4lu MB  free=%4lu MB  (%.1f%% used)\n",
           label, (unsigned long)total_mb, (unsigned long)used_mb,
           (unsigned long)free_mb, 100.0 * used_mb / total_mb);
}

/* ────────────────────────────────────────────────────────────────────────────
 * GPU Warp Simulator
 *
 * Models warp-based GPU execution with:
 *   - Thread → warp mapping (ceil(threads / WARP_SIZE) warps)
 *   - Branch divergence: fraction of warps that require re-execution
 *   - Memory coalescing: adjacent threads accessing adjacent addresses
 *     produce fewer memory transactions (coalesced), while strided access
 *     produces more (uncoalesced)
 *   - Cycle estimation based on transaction count and divergence overhead
 * ──────────────────────────────────────────────────────────────────────────── */

static struct gpu_metrics simulate_gpu_kernel(const struct gpu_kernel *kernel) {
    struct gpu_metrics m = {0};

    /* Warp count = ceil(total_threads / WARP_SIZE) */
    m.warps_launched = (kernel->total_threads + WARP_SIZE - 1) / WARP_SIZE;
    m.active_warps = m.warps_launched < MAX_WARPS_PER_SM
                   ? m.warps_launched : MAX_WARPS_PER_SM;

    /* Divergence model: fraction of warps that hit divergent branches.
     * Each divergent warp costs ~2x execution (both paths serialized). */
    uint32_t divergent_warps = (uint32_t)(m.warps_launched * kernel->branch_divergence);
    m.divergence_ratio = m.warps_launched > 0
                       ? (float)divergent_warps / m.warps_launched
                       : 0.0f;

    /* Coalescing model: if stride <= cache line, access is coalesced.
     * Coalesced: 1 transaction per warp. Uncoalesced: up to WARP_SIZE. */
    m.total_mem_transactions = m.warps_launched;

    if (kernel->mem_access_stride <= CACHE_LINE_BYTES) {
        /* Good locality — most accesses coalesce into single transactions */
        m.coalesced_transactions   = m.warps_launched;
        m.uncoalesced_transactions = 0;
    } else {
        /* Strided access — each thread may generate a separate transaction */
        uint32_t scatter_factor = kernel->mem_access_stride / CACHE_LINE_BYTES;
        if (scatter_factor > WARP_SIZE) scatter_factor = WARP_SIZE;

        m.coalesced_transactions   = m.warps_launched / scatter_factor;
        m.uncoalesced_transactions = m.warps_launched - m.coalesced_transactions;
        m.total_mem_transactions   = m.coalesced_transactions +
                                     m.uncoalesced_transactions * scatter_factor;
    }

    m.coalescing_efficiency = m.total_mem_transactions > 0
        ? (float)m.coalesced_transactions / m.total_mem_transactions
        : 1.0f;

    /* Cycle estimation:
     *   base = warps * 4 cycles/warp (instruction issue)
     *   + divergence overhead: divergent_warps * 8 extra cycles
     *   + memory latency: uncoalesced_transactions * 200 cycles (L2 miss) */
    m.estimated_cycles = (uint64_t)m.warps_launched * 4
                       + (uint64_t)divergent_warps * 8
                       + (uint64_t)m.uncoalesced_transactions * 200;

    m.estimated_time_us = (double)m.estimated_cycles / GPU_CLOCK_MHZ;

    return m;
}

static void print_gpu_metrics(const struct gpu_kernel *kernel,
                              const struct gpu_metrics *m) {
    printf("\n  ┌─ GPU Kernel: %s\n", kernel->name);
    printf("  │  threads: %-6u   warps: %-4u   active: %u\n",
           kernel->total_threads, m->warps_launched, m->active_warps);
    printf("  │  divergence:  %.1f%%  (%u/%u warps divergent)\n",
           m->divergence_ratio * 100.0f,
           (uint32_t)(m->warps_launched * m->divergence_ratio),
           m->warps_launched);
    printf("  │  coalescing:  %.1f%%  (coalesced=%u  uncoalesced=%u  total=%u)\n",
           m->coalescing_efficiency * 100.0f,
           m->coalesced_transactions, m->uncoalesced_transactions,
           m->total_mem_transactions);
    printf("  │  est. cycles: %llu  (%.2f µs @ %d MHz)\n",
           (unsigned long long)m->estimated_cycles,
           m->estimated_time_us, GPU_CLOCK_MHZ);
    printf("  └─ shared mem: %u bytes   stride: %u bytes\n",
           kernel->shared_mem_bytes, kernel->mem_access_stride);
}

/* ────────────────────────────────────────────────────────────────────────────
 * CPU Task Executor
 * ──────────────────────────────────────────────────────────────────────────── */

static int run_cpu_task(struct cpu_task *task) {
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Allocate and touch memory to create real pressure */
    task->buffer = malloc(task->alloc_bytes);
    if (!task->buffer) {
        fprintf(stderr, "  [cpu] task %d: allocation failed (%zu MB)\n",
                task->id, task->alloc_bytes / (1024 * 1024));
        return -1;
    }

    /* Touch every page to force physical allocation (prevent lazy mapping) */
    memset(task->buffer, 0xAB, task->alloc_bytes);

    clock_gettime(CLOCK_MONOTONIC, &end);

    task->exec_time_ms = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("  [cpu] task %d: allocated %zu MB in %.1f ms\n",
           task->id, task->alloc_bytes / (1024 * 1024), task->exec_time_ms);

    return 0;
}

static void release_cpu_task(struct cpu_task *task) {
    if (task->buffer) {
        free(task->buffer);
        task->buffer = NULL;
        printf("  [cpu] task %d: released %zu MB\n",
               task->id, task->alloc_bytes / (1024 * 1024));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Predefined GPU Kernels (representative workloads)
 * ──────────────────────────────────────────────────────────────────────────── */

static const struct gpu_kernel default_kernels[] = {
    {
        .name              = "matmul_tiled_256x256",
        .total_threads     = 65536,
        .shared_mem_bytes  = 49152,
        .branch_divergence = 0.05f,    /* minimal divergence — uniform work */
        .mem_access_stride = 4,        /* float32, coalesced row access     */
    },
    {
        .name              = "conv2d_3x3_relu",
        .total_threads     = 32768,
        .shared_mem_bytes  = 16384,
        .branch_divergence = 0.15f,    /* ReLU causes some divergence       */
        .mem_access_stride = 16,       /* strided filter access              */
    },
    {
        .name              = "sparse_spmv_csr",
        .total_threads     = 8192,
        .shared_mem_bytes  = 8192,
        .branch_divergence = 0.45f,    /* irregular row lengths → high div  */
        .mem_access_stride = 512,      /* scattered column indices           */
    },
    {
        .name              = "reduce_sum_global",
        .total_threads     = 131072,
        .shared_mem_bytes  = 32768,
        .branch_divergence = 0.02f,    /* nearly uniform reduction tree     */
        .mem_access_stride = 4,        /* sequential reduction, coalesced   */
    },
};

#define NUM_DEFAULT_KERNELS (sizeof(default_kernels) / sizeof(default_kernels[0]))

/* ────────────────────────────────────────────────────────────────────────────
 * Main — Scenario Runner
 * ──────────────────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --cpu-tasks N     Number of CPU tasks to run (default: 8)\n");
    printf("  --gpu-kernels N   Number of GPU kernels to simulate (default: 4)\n");
    printf("  --alloc-mb N      Memory per CPU task in MB (default: 64)\n");
    printf("  --hold-sec N      Seconds to hold memory before release (default: 10)\n");
    printf("  --help            Show this help message\n");
}

int main(int argc, char **argv) {
    int num_cpu_tasks  = 8;
    int num_gpu_kernels = (int)NUM_DEFAULT_KERNELS;
    int alloc_mb       = 64;
    int hold_sec       = 10;

    static struct option long_opts[] = {
        {"cpu-tasks",   required_argument, NULL, 'c'},
        {"gpu-kernels", required_argument, NULL, 'g'},
        {"alloc-mb",    required_argument, NULL, 'm'},
        {"hold-sec",    required_argument, NULL, 's'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:g:m:s:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': num_cpu_tasks   = atoi(optarg); break;
            case 'g': num_gpu_kernels = atoi(optarg); break;
            case 'm': alloc_mb       = atoi(optarg); break;
            case 's': hold_sec       = atoi(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("══════════════════════════════════════════════════════════════\n");
    printf("  Workload Simulator — Memory Pressure + GPU Metrics\n");
    printf("  CPU tasks: %d × %d MB   GPU kernels: %d   hold: %ds\n",
           num_cpu_tasks, alloc_mb, num_gpu_kernels, hold_sec);
    printf("══════════════════════════════════════════════════════════════\n\n");

    /* ── Phase 1: Baseline ── */
    printf("── Phase 1: Baseline Memory State ──\n");
    report_memory_state("baseline");

    /* ── Phase 2: GPU Kernel Simulation ── */
    printf("\n── Phase 2: GPU Workload Simulation ──\n");

    struct gpu_metrics totals = {0};
    for (int i = 0; i < num_gpu_kernels && !g_stop; i++) {
        const struct gpu_kernel *k = &default_kernels[i % NUM_DEFAULT_KERNELS];
        struct gpu_metrics m = simulate_gpu_kernel(k);
        print_gpu_metrics(k, &m);

        totals.warps_launched         += m.warps_launched;
        totals.coalesced_transactions += m.coalesced_transactions;
        totals.uncoalesced_transactions += m.uncoalesced_transactions;
        totals.total_mem_transactions += m.total_mem_transactions;
        totals.estimated_cycles       += m.estimated_cycles;
    }

    if (num_gpu_kernels > 0) {
        printf("\n  ── GPU Summary ──\n");
        printf("  total warps:        %u\n", totals.warps_launched);
        printf("  total transactions: %u  (coalesced=%u  uncoalesced=%u)\n",
               totals.total_mem_transactions,
               totals.coalesced_transactions,
               totals.uncoalesced_transactions);
        printf("  overall coalescing: %.1f%%\n",
               totals.total_mem_transactions > 0
               ? 100.0f * totals.coalesced_transactions / totals.total_mem_transactions
               : 0.0f);
        printf("  total est. cycles:  %llu (%.2f µs)\n",
               (unsigned long long)totals.estimated_cycles,
               (double)totals.estimated_cycles / GPU_CLOCK_MHZ);
    }

    /* ── Phase 3: CPU Tasks (Memory Pressure) ── */
    printf("\n── Phase 3: CPU Memory Pressure ──\n");

    struct cpu_task *tasks = calloc(num_cpu_tasks, sizeof(struct cpu_task));
    if (!tasks) {
        fprintf(stderr, "failed to allocate task array\n");
        return 1;
    }

    for (int i = 0; i < num_cpu_tasks && !g_stop; i++) {
        tasks[i].id = i;
        tasks[i].alloc_bytes = (size_t)alloc_mb * 1024 * 1024;
        run_cpu_task(&tasks[i]);
    }

    report_memory_state("under pressure");

    /* ── Phase 4: Hold (let balloon driver react) ── */
    printf("\n── Phase 4: Holding Memory (%d seconds) ──\n", hold_sec);
    printf("  Watch balloon driver react: sudo dmesg -w | grep vballoon_lab\n");

    for (int i = 0; i < hold_sec && !g_stop; i++) {
        sleep(1);
        if ((i + 1) % 5 == 0 || i == hold_sec - 1) {
            char label[32];
            snprintf(label, sizeof(label), "hold +%ds", i + 1);
            report_memory_state(label);
        }
    }

    /* ── Phase 5: Release and Recovery ── */
    printf("\n── Phase 5: Release and Recovery ──\n");

    for (int i = 0; i < num_cpu_tasks; i++) {
        release_cpu_task(&tasks[i]);
    }
    free(tasks);

    report_memory_state("after release");

    /* Brief pause to let balloon re-inflate */
    printf("\n  Waiting 5s for balloon stabilization...\n");
    for (int i = 0; i < 5 && !g_stop; i++) sleep(1);
    report_memory_state("stabilized");

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  Workload simulation complete.\n");
    printf("══════════════════════════════════════════════════════════════\n");

    return 0;
}
