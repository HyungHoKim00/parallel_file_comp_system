// main_spinlock.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "result.h"

#define TOTAL_FILES 60
#define TIME_MULTIPLIER 10000

typedef struct {
    char* name;
    int size;
} FileTask;

FileTask file_tasks[TOTAL_FILES];
int next_index = 0;
volatile int lock = 0;  // spinlock flag

void spin_lock() {
    while (__sync_lock_test_and_set(&lock, 1)) {
        while (lock);  // busy-wait
    }
}

void spin_unlock() {
    __sync_lock_release(&lock);
}

void run_cpu_for(int times) {
    volatile double dummy = 1.0;
    for (int i = 0; i < times * 10000; i++) {
        dummy *= 1.0000001;
    }
}

void* worker_thread(void* arg) {
    while (1) {
        spin_lock();
        int index = next_index++;
        spin_unlock();

        if (index >= TOTAL_FILES) break;

        FileTask* task = &file_tasks[index];
        run_cpu_for(5 * task->size);
        run_cpu_for(3 * task->size);
        run_cpu_for(2 * task->size);
    }
    return NULL;
}

void run_thread_only(int thread_count) {
    pthread_t threads[thread_count];

    for (int i = 0; i < thread_count; i++)
        pthread_create(&threads[i], NULL, worker_thread, NULL);

    for (int i = 0; i < thread_count; i++)
        pthread_join(threads[i], NULL);
}

void run_process_only(int process_count, int proc_index) {
    for (int i = proc_index; i < TOTAL_FILES; i += process_count) {
        run_cpu_for(5 * file_tasks[i].size);
        run_cpu_for(3 * file_tasks[i].size);
        run_cpu_for(2 * file_tasks[i].size);
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num_processes> <num_threads>\n", argv[0]);
        return 1;
    }

    int P = atoi(argv[1]);
    int T = atoi(argv[2]);

    int sizes[60] = {
        73, 18, 94, 26, 51, 62, 37, 89, 5, 43,
        77, 14, 35, 68, 92, 10, 23, 81, 6, 57,
        49, 87, 30, 1, 99, 64, 12, 46, 91, 28,
        39, 83, 7, 58, 100, 22, 75, 33, 9, 67,
        29, 56, 44, 15, 79, 2, 88, 11, 93, 16,
        84, 31, 21, 60, 70, 4, 95, 36, 47, 8
    };
    for (int i = 0; i < TOTAL_FILES; i++) {
        file_tasks[i].name = NULL;
        file_tasks[i].size = sizes[i];
    }

    PerfMetrics metrics;

    if (P == 0 && T == 0) {
        start_perf(&metrics);
        for (int i = 0; i < TOTAL_FILES; i++) {
            run_cpu_for(5 * file_tasks[i].size);
            run_cpu_for(3 * file_tasks[i].size);
            run_cpu_for(2 * file_tasks[i].size);
        }
        end_perf(&metrics, 0);
        print_perf_summary(&metrics);
        return 0;
    }

    if (T == 0) {
        start_perf(&metrics);
        for (int i = 0; i < P; i++) {
            if (fork() == 0) {
                run_process_only(P, i);
                exit(0);
            }
        }
        end_perf(&metrics, P);
        print_perf_summary(&metrics);
        return 0;
    }

    if (P == 0) {
        start_perf(&metrics);
        run_thread_only(T);
        end_perf(&metrics, 0);
        print_perf_summary(&metrics);
        return 0;
    }

    start_perf(&metrics);
    for (int i = 0; i < P; i++) {
        if (fork() == 0) {
            run_thread_only(T);
            exit(0);
        }
    }
    end_perf(&metrics, P);
    print_perf_summary(&metrics);
    return 0;
}
