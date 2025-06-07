#ifndef PERF_METRICS_H
#define PERF_METRICS_H

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>

// 시간 및 자원 사용량 측정용 구조체 정의
typedef struct {
    struct timeval start_time;
    struct timeval end_time;
    struct rusage usage;  // 누적 자원 (child 또는 self)
} PerfMetrics;

// 시간 정규화 함수
static inline void normalize_time(struct timeval* t) {
    if (t->tv_usec >= 1000000) {
        t->tv_sec += t->tv_usec / 1000000;
        t->tv_usec %= 1000000;
    }
}

// 측정 시작
static inline void start_perf(PerfMetrics* m) {
    gettimeofday(&m->start_time, NULL);
    memset(&m->usage, 0, sizeof(struct rusage));
}

// 측정 종료: 자식 or self 자원 수집
static inline void end_perf(PerfMetrics* m, int child_count) {
    memset(&m->usage, 0, sizeof(struct rusage));

    if (child_count > 0) {
        for (int i = 0; i < child_count; i++) {
            struct rusage temp;
            memset(&temp, 0, sizeof(temp));

            pid_t pid = wait4(-1, NULL, 0, &temp);
            if (pid == -1) {
                if (errno == ECHILD) break;
                perror("wait4 failed");
                continue;
            }

            // 개별 자식 프로세스의 CPU 사용량 출력
            double user = temp.ru_utime.tv_sec * 1000.0 + temp.ru_utime.tv_usec / 1000.0;
            double sys = temp.ru_stime.tv_sec * 1000.0 + temp.ru_stime.tv_usec / 1000.0;
            printf("[PID %d] User Time: %.3f ms, Sys Time: %.3f ms\n", pid, user, sys);

            m->usage.ru_utime.tv_sec += temp.ru_utime.tv_sec;
            m->usage.ru_utime.tv_usec += temp.ru_utime.tv_usec;
            normalize_time(&m->usage.ru_utime);

            m->usage.ru_stime.tv_sec += temp.ru_stime.tv_sec;
            m->usage.ru_stime.tv_usec += temp.ru_stime.tv_usec;
            normalize_time(&m->usage.ru_stime);

            m->usage.ru_nvcsw += temp.ru_nvcsw;
            m->usage.ru_nivcsw += temp.ru_nivcsw;

            if (temp.ru_maxrss > m->usage.ru_maxrss)
                m->usage.ru_maxrss = temp.ru_maxrss;
        }

        if (m->usage.ru_utime.tv_sec == 0 && m->usage.ru_utime.tv_usec == 0) {
            getrusage(RUSAGE_SELF, &m->usage);
        }
    }
    else {
        getrusage(RUSAGE_SELF, &m->usage);
    }

    gettimeofday(&m->end_time, NULL);
}

// 출력 함수
static inline void print_perf_summary(const PerfMetrics* m) {
    double wall = (m->end_time.tv_sec - m->start_time.tv_sec) * 1000.0 +
        (m->end_time.tv_usec - m->start_time.tv_usec) / 1000.0;

    double user = m->usage.ru_utime.tv_sec * 1000.0 + m->usage.ru_utime.tv_usec / 1000.0;
    double sys = m->usage.ru_stime.tv_sec * 1000.0 + m->usage.ru_stime.tv_usec / 1000.0;
    double cpu_total = user + sys;

    long mem_kb = m->usage.ru_maxrss;
    long vctx = m->usage.ru_nvcsw;
    long ivctx = m->usage.ru_nivcsw;

    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    double avg_core_util_percent = (wall > 0.0 && num_cores > 0) ?
        ((cpu_total / wall) / num_cores) * 100.0 : 0.0;

    double cpu_idle_percent = (wall > 0.0) ? fmax(0.0, 100.0 * (1.0 - (cpu_total / wall))) : 0.0;

    printf("\nTotal compression time: %.3f ms\n", wall);
    printf("CPU User time (all):    %.3f ms\n", user);
    printf("CPU System time (all):  %.3f ms\n", sys);
    printf("Max Memory Usage:       %ld KB\n", mem_kb);
    printf("Voluntary Ctx Switches:   %ld\n", vctx);
    printf("Involuntary Ctx Switches: %ld\n", ivctx);
    printf("Total Context Switches:   %ld\n", vctx + ivctx);
    printf("\nCPU Idle Percent:       %.2f %%\n", cpu_idle_percent);
    printf("Avg CPU Core Usage:     %.2f %%\n", avg_core_util_percent);
}

#endif
