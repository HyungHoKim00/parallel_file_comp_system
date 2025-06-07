// main_semaphore.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "result.h"

#define TOTAL_FILES 60
#define MAX_TASKS 100
#define TIME_MULTIPLIER 10000

typedef enum { RAW, BWT_DONE, MTF_DONE, FINISHED = -1 } Stage;

typedef struct {
    char* name;
    char  data[256];
    Stage stage;
    int size;
} Task;

Task* raw_queue[MAX_TASKS], * bwt_queue[MAX_TASKS], * mtf_queue[MAX_TASKS];
int raw_head = 0, raw_tail = 0;
int bwt_head = 0, bwt_tail = 0;
int mtf_head = 0, mtf_tail = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t complete_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_raw, sem_bwt, sem_mtf;

int completed_tasks = 0;
int task_target = 0;

int file_sizes[TOTAL_FILES] = {
    73, 18, 94, 26, 51, 62, 37, 89, 5, 43,
    77, 14, 35, 68, 92, 10, 23, 81, 6, 57,
    49, 87, 30, 1, 99, 64, 12, 46, 91, 28,
    39, 83, 7, 58, 100, 22, 75, 33, 9, 67,
    29, 56, 44, 15, 79, 2, 88, 11, 93, 16,
    84, 31, 21, 60, 70, 4, 95, 36, 47, 8
};

void run_cpu_for(int times) {
    volatile double dummy = 1.0;
    for (int i = 0; i < times * TIME_MULTIPLIER; i++) {
        dummy *= 1.0000001;
    }
}

void apply_bwt(Task* task) {
    run_cpu_for(5 * task->size);
    sprintf(task->data, "bwt(%s)", task->data);
}
void apply_mtf(Task* task) {
    run_cpu_for(3 * task->size);
    sprintf(task->data, "mtf(%s)", task->data);
}
void apply_rle(Task* task) {
    run_cpu_for(2 * task->size);
}

void enqueue(Task* task, Task** queue, int* tail) {
    queue[(*tail)++] = task;
}

Task* dequeue(Task** queue, int* head) {
    return queue[(*head)++];
}

void* worker_thread(void* arg) {
    while (1) {
        Task* task = NULL;

        if (sem_trywait(&sem_mtf) == 0) {
            pthread_mutex_lock(&mutex);
            task = dequeue(mtf_queue, &mtf_head);
            pthread_mutex_unlock(&mutex);

            if (task->stage == FINISHED) {
                free(task);
                break;
            }
            apply_rle(task);
            free(task->name);
            free(task);

            pthread_mutex_lock(&complete_mutex);
            completed_tasks++;
            pthread_mutex_unlock(&complete_mutex);
            continue;
        }

        if (sem_trywait(&sem_bwt) == 0) {
            pthread_mutex_lock(&mutex);
            task = dequeue(bwt_queue, &bwt_head);
            pthread_mutex_unlock(&mutex);

            apply_mtf(task);
            task->stage = MTF_DONE;
            pthread_mutex_lock(&mutex);
            enqueue(task, mtf_queue, &mtf_tail);
            pthread_mutex_unlock(&mutex);
            sem_post(&sem_mtf);
            continue;
        }

        if (sem_trywait(&sem_raw) == 0) {
            pthread_mutex_lock(&mutex);
            task = dequeue(raw_queue, &raw_head);
            pthread_mutex_unlock(&mutex);

            apply_bwt(task);
            task->stage = BWT_DONE;
            pthread_mutex_lock(&mutex);
            enqueue(task, bwt_queue, &bwt_tail);
            pthread_mutex_unlock(&mutex);
            sem_post(&sem_bwt);
            continue;
        }
    }
    return NULL;
}

void run_compressor(int thread_count, int proc_index, int total_proc) {
    pthread_t threads[thread_count];
    sem_init(&sem_raw, 0, 0);
    sem_init(&sem_bwt, 0, 0);
    sem_init(&sem_mtf, 0, 0);

    for (int i = 0; i < thread_count; i++)
        pthread_create(&threads[i], NULL, worker_thread, NULL);

    for (int i = proc_index; i < TOTAL_FILES; i += total_proc) {
        Task* task = malloc(sizeof(Task));
        task->name = strdup("file");
        strcpy(task->data, "data");
        task->stage = RAW;
        task->size = file_sizes[i];
        pthread_mutex_lock(&mutex);
        enqueue(task, raw_queue, &raw_tail);
        pthread_mutex_unlock(&mutex);
        sem_post(&sem_raw);
        pthread_mutex_lock(&complete_mutex);
        task_target++;
        pthread_mutex_unlock(&complete_mutex);
    }

    while (1) {
        pthread_mutex_lock(&complete_mutex);
        if (completed_tasks >= task_target) {
            pthread_mutex_unlock(&complete_mutex);
            break;
        }
        pthread_mutex_unlock(&complete_mutex);
        usleep(1000);
    }

    for (int i = 0; i < thread_count; i++) {
        Task* dummy = malloc(sizeof(Task));
        dummy->stage = FINISHED;
        pthread_mutex_lock(&mutex);
        enqueue(dummy, mtf_queue, &mtf_tail);
        pthread_mutex_unlock(&mutex);
        sem_post(&sem_mtf);
    }

    for (int i = 0; i < thread_count; i++)
        pthread_join(threads[i], NULL);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <process_count> <thread_count>\n", argv[0]);
        return 1;
    }
    int P = atoi(argv[1]), T = atoi(argv[2]);
    PerfMetrics m;
    start_perf(&m);
    for (int i = 0; i < P; i++) {
        if (fork() == 0) {
            run_compressor(T, i, P);
            exit(0);
        }
    }
    end_perf(&m, P);
    print_perf_summary(&m);
    return 0;
}
