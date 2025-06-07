#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "result.h"

#define TOTAL_FILES 60      	// 전체 가상 파일 개수
#define MAX_TASKS 100       	// 큐에 넣을 수 있는 최대 작업 수
#define TIME_MULTIPLIER 10000  // 알고리즘 시간 조절용 배수
#define MAX_FILES_PER_PROC 60

// 파일의 처리 단계 정의
typedef enum { RAW, BWT_DONE, MTF_DONE } Stage;

// 작업 구조체: 파일 이름, 데이터, 현재 단계 포함
typedef struct {
	char* name;
	char  data[256];
	Stage stage;
	int size;  // 파일 크기
} Task;

typedef struct {
	int indices[MAX_FILES_PER_PROC];
	int count;
	int total_size;
} ProcessLoad;

// 내부에서 사용될 작업 구조체
typedef struct {
	int index;
	int size;
} FileEntry;

// 단계별 큐
Task* raw_queue[MAX_TASKS], * bwt_queue[MAX_TASKS], * mtf_queue[MAX_TASKS];
int raw_head = 0, raw_tail = 0;
int bwt_head = 0, bwt_tail = 0;
int mtf_head = 0, mtf_tail = 0;

// 동기화 변수
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;

int completed_tasks = 0;
int task_target = 0;

// cpu 돌리는 시간
pthread_mutex_t complete_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t all_done = PTHREAD_COND_INITIALIZER;

// 파일 크기 배열 (1~100 범위의 임의 수)
int file_sizes[TOTAL_FILES] = {
	73, 18, 94, 26, 51, 62, 37, 89, 5, 43,
	77, 14, 35, 68, 92, 10, 23, 81, 6, 57,
	49, 87, 30, 1, 99, 64, 12, 46, 91, 28,
	39, 83, 7, 58, 100, 22, 75, 33, 9, 67,
	29, 56, 44, 15, 79, 2, 88, 11, 93, 16,
	84, 31, 21, 60, 70, 4, 95, 36, 47, 8
};

// 내림차순 정렬용 비교 함수
int cmp_desc(const void* a, const void* b) {
	return ((FileEntry*)b)->size - ((FileEntry*)a)->size;
}

// Greedy 분배 함수
void assign_files_greedy(int P, ProcessLoad buckets[P]) {
	FileEntry files[TOTAL_FILES];
	for (int i = 0; i < TOTAL_FILES; i++) {
    	files[i].index = i;
    	files[i].size = file_sizes[i];
	}

	qsort(files, TOTAL_FILES, sizeof(FileEntry), cmp_desc); // 내림차순 정렬

	for (int i = 0; i < P; i++) {
    	buckets[i].count = 0;
    	buckets[i].total_size = 0;
	}

	for (int i = 0; i < TOTAL_FILES; i++) {
    	// 가장 적은 작업량을 가진 프로세스 찾기
    	int min_idx = 0;
    	for (int j = 1; j < P; j++) {
        	if (buckets[j].total_size < buckets[min_idx].total_size)
            	min_idx = j;
    	}

    	// 배정
    	int proc = min_idx;
    	buckets[proc].indices[buckets[proc].count++] = files[i].index;
    	buckets[proc].total_size += files[i].size;
	}

	// 로그 출력
	printf("\n[파일 분배 결과 - Greedy 방식]\n");
	for (int i = 0; i < P; i++) {
    	printf("프로세스 %d: 총 작업량 = %d (파일 %d개)\n ", i, buckets[i].total_size, buckets[i].count);
    	for (int j = 0; j < buckets[i].count; j++) {
        	int idx = buckets[i].indices[j];
        	printf("file_%02d(%d) ", idx, file_sizes[idx]);
       	 
        	if ((j+1) % 6 == 0)
            	printf("\n  ");
    	}
    	printf("\n");
	}
}

// CPU 부하 시뮬레이션 함수
void run_cpu_for(int times) {
	volatile double dummy = 0;
	for (int i = 0; i < times * TIME_MULTIPLIER; i++) {
    	dummy += i * 0.000001;
	}
}

// BWT 단계 (지연 포함)
void apply_bwt(char* output, const char* input, int size) {
	run_cpu_for(5 * size);
	snprintf(output, 256, "bwt(%s)", input);
}

// MTF 단계 (지연 포함)
void apply_mtf(char* output, const char* input, int size) {
	run_cpu_for(3 * size);
	snprintf(output, 256, "mtf(%s)", input);
}

// RLE + Huffman 단계 (지연 포함)
void apply_rle(const char* input, int size) {
	run_cpu_for(2 * size);
}

// ── Process-only 모드 전용: 동적할당·락 없이 순차 처리 ──
void run_process_only(int P, int idx) {
	char buf1[256], buf2[256];
	for (int i = idx; i < TOTAL_FILES; i += P) {
    	int size = file_sizes[i];
    	apply_bwt(buf1, "content", size);
    	apply_mtf(buf2, buf1, size);
    	apply_rle(buf2, size);
	}
}

// greedy alg 적용된 process-only 모드
void run_process_only_optimized(ProcessLoad* my_bucket) {
	char buf1[256], buf2[256];
	for (int i = 0; i < my_bucket->count; i++) {
    	int idx = my_bucket->indices[i];
    	int size = file_sizes[idx];
    	apply_bwt(buf1, "content", size);
    	apply_mtf(buf2, buf1, size);
    	apply_rle(buf2, size);
	}
}

// ── Thread-only 모드 전용: 뮤텍스 없이 인덱스 분할 ──
typedef struct { int id, T; } ThreadArg;
void* thread_func_opt(void* _a) {
	ThreadArg * a = _a;
	char buf1[256], buf2[256];
	for (int i = a->id; i < TOTAL_FILES; i += a->T) {
    	int size = file_sizes[i];
    	apply_bwt(buf1, "content", size);
    	apply_mtf(buf2, buf1, size);
    	apply_rle(buf2, size);
	}
	return NULL;
}

void run_thread_only(int T) {
	pthread_t th[T];
	ThreadArg args[T];
	for (int t = 0; t < T; t++) {
    	args[t].id = t; args[t].T = T;
    	pthread_create(&th[t], NULL, thread_func_opt, &args[t]);
	}
	for (int t = 0; t < T; t++)
    	pthread_join(th[t], NULL);
}

// 작업 큐에 추가 (단계별 큐 사용)
void enqueue_task(Task* task) {
	pthread_mutex_lock(&queue_mutex);
	switch (task->stage) {
	case RAW:
    	raw_queue[raw_tail++] = task;
    	break;
	case BWT_DONE:
    	bwt_queue[bwt_tail++] = task;
    	break;
	case MTF_DONE:
    	mtf_queue[mtf_tail++] = task;
    	break;
	}
	pthread_cond_signal(&queue_not_empty);
	pthread_mutex_unlock(&queue_mutex);
}

// 우선순위가 가장 높은 작업을 큐에서 꺼냄
Task* dequeue_highest_priority_task() {
	pthread_mutex_lock(&queue_mutex);
	while (raw_head == raw_tail && bwt_head == bwt_tail && mtf_head == mtf_tail) {
    	pthread_cond_wait(&queue_not_empty, &queue_mutex);
	}
	Task* task = NULL;
	if (mtf_head < mtf_tail) {
    	task = mtf_queue[mtf_head++];
	} else if (bwt_head < bwt_tail) {
    	task = bwt_queue[bwt_head++];
	} else if (raw_head < raw_tail) {
    	task = raw_queue[raw_head++];
	}
	pthread_mutex_unlock(&queue_mutex);
	return task;
}

// 스레드가 수행할 작업 함수
void* worker_thread(void* arg) {
	while (1) {
    	Task* task = dequeue_highest_priority_task();
    	switch (task->stage) {
    	case RAW:
        	apply_bwt(task->data, task->data, task->size);
        	task->stage = BWT_DONE;
        	enqueue_task(task);
        	break;
    	case BWT_DONE:
        	apply_mtf(task->data, task->data, task->size);
        	task->stage = MTF_DONE;
        	enqueue_task(task);
        	break;
    	case MTF_DONE:
        	apply_rle(task->data, task->size);
        	pthread_mutex_lock(&complete_mutex);
        	completed_tasks++;
        	if (completed_tasks == task_target) {
            	pthread_cond_signal(&all_done);
        	}
        	pthread_mutex_unlock(&complete_mutex);
        	free(task->name);
        	free(task);
        	break;
    	}
	}
	return NULL;
}

// 압축 실행 함수: 각 프로세스마다 실행
void run_compressor(int thread_count, int proc_index, int total_proc) {
	if (thread_count <= 0) {
    	fprintf(stderr, "Invalid thread_count (must be ≥ 1).\n");
    	return;
	}
	pthread_t threads[thread_count];
	for (int i = 0; i < thread_count; i++) {
    	pthread_create(&threads[i], NULL, worker_thread, NULL);
	}
	int count = 0;
	for (int i = proc_index; i < TOTAL_FILES; i += total_proc) count++;
	task_target = count;
	for (int i = proc_index; i < TOTAL_FILES; i += total_proc) {
    	Task* task = malloc(sizeof(Task));
    	strncpy(task->data, "content", sizeof(task->data));
    	char name[32];
    	snprintf(name, sizeof(name), "file_%02d", i);
    	task->name = strdup(name);
    	task->stage = RAW;
    	task->size = file_sizes[i];
    	enqueue_task(task);
	}
	pthread_mutex_lock(&complete_mutex);
	while (completed_tasks < task_target)
    	pthread_cond_wait(&all_done, &complete_mutex);
	pthread_mutex_unlock(&complete_mutex);
}

// 메인 함수: 전체 프로세스를 생성하고 성능 측정
int main(int argc, char* argv[]) {
	if (argc != 3) {
    	fprintf(stderr, "Usage: %s <process_count> <thread_count>\n", argv[0]);
    	return 1;
	}
	int P = atoi(argv[1]);	// 자식 프로세스 수
	int T = atoi(argv[2]);	// 워커 스레드 수
	PerfMetrics metrics;

	// ──── 1) 순차(single) 모드 (C0) ───────────────────────────────
	if (P == 0 && T == 0) {
    	start_perf(&metrics);
    	char buf1[256], buf2[256];
    	for (int i = 0; i < TOTAL_FILES; i++) {
        	int size = file_sizes[i];
        	apply_bwt(buf1, "content", size);
        	apply_mtf(buf2, buf1, size);
        	apply_rle(buf2, size);
    	}
    	end_perf(&metrics, 0);
    	print_perf_summary(&metrics);
    	return 0;
	}

	// ──── 2) process-only 모드 (C1~C5) ─────────────────────────── ** 수정됨
	if (T == 0) {
    	start_perf(&metrics);

    	ProcessLoad buckets[P];
    	assign_files_greedy(P, buckets);  // 작업 분배

    	for (int i = 0; i < P; i++) {
        	pid_t pid = fork();
        	if (pid == 0) {
            	run_process_only_optimized(&buckets[i]);
            	exit(0);
        	}
    	}

	end_perf(&metrics, P);
	print_perf_summary(&metrics);
	return 0;
}

	// ──── 3) thread-only 모드 (C6~C9) ────────────────────────────
	if (P == 0) {
    	start_perf(&metrics);
    	run_thread_only(T);
    	end_perf(&metrics, 0);
    	print_perf_summary(&metrics);
    	return 0;
	}

	// ──── 4) hybrid 모드 (C10~C14) ───────────────────────────────
	start_perf(&metrics);
	for (int i = 0; i < P; i++) {
    	pid_t pid = fork();
    	if (pid == 0) {
        	run_compressor(T, i, P);
        	exit(0);
    	}
	}
	end_perf(&metrics, P);
	print_perf_summary(&metrics);
	return 0;
}
