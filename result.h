#ifndef PERF_METRICS_H
#define PERF_METRICS_H

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

// 시간 및 자원 사용량 측정용 구조체 정의
typedef struct {
    struct timeval start_time;
    struct timeval end_time;
    struct rusage child_usage;  // 모든 자식의 누적 자원
} PerfMetrics;

// 측정 시작: 시작 시간 기록
static inline void start_perf(PerfMetrics* m) {
    gettimeofday(&m->start_time, NULL);
    memset(&m->child_usage, 0, sizeof(struct rusage));
}

// 자식의 timeval 누적 시 오버플로우 처리
static inline void normalize_time(struct timeval* t) {
    if (t->tv_usec >= 1000000) {
        t->tv_sec += t->tv_usec / 1000000;
        t->tv_usec %= 1000000;
    }
}

// 측정 종료: 자식 wait + 누적 rusage
static inline void end_perf(PerfMetrics* m, int child_count) {
    if (child_count > 0) {
        // ── 기존 자식 프로세스 사용량 누적 ──
        for (int i = 0; i < child_count; i++) {
            struct rusage temp;
            memset(&temp, 0, sizeof(temp));

            if (wait4(-1, NULL, 0, &temp) == -1) {
                if (errno == ECHILD) break; // 더 이상 자식 없음
                perror("wait4 failed");
                continue;
            }

            // 누적 user time
            m->child_usage.ru_utime.tv_sec += temp.ru_utime.tv_sec;
            m->child_usage.ru_utime.tv_usec += temp.ru_utime.tv_usec;
            normalize_time(&m->child_usage.ru_utime);

            // 누적 system time
            m->child_usage.ru_stime.tv_sec += temp.ru_stime.tv_sec;
            m->child_usage.ru_stime.tv_usec += temp.ru_stime.tv_usec;
            normalize_time(&m->child_usage.ru_stime);

            // 문맥 교환 누적
            m->child_usage.ru_nvcsw += temp.ru_nvcsw;
            m->child_usage.ru_nivcsw += temp.ru_nivcsw;

            // 최대 메모리 사용량 추적
            if (temp.ru_maxrss > m->child_usage.ru_maxrss)
                m->child_usage.ru_maxrss = temp.ru_maxrss;
        }
    }
    else {
        // ── 자식이 없으면, 부모(Self) 프로세스(=스레드 포함) 사용량 측정 ──
        struct rusage self_ru;
        if (getrusage(RUSAGE_SELF, &self_ru) == 0) {
            // user time
            m->child_usage.ru_utime = self_ru.ru_utime;
            // system time
            m->child_usage.ru_stime = self_ru.ru_stime;
            // 문맥 교환
            m->child_usage.ru_nvcsw = self_ru.ru_nvcsw;
            m->child_usage.ru_nivcsw = self_ru.ru_nivcsw;
            // 최대 메모리
            m->child_usage.ru_maxrss = self_ru.ru_maxrss;
        }
        else {
            perror("getrusage(RUSAGE_SELF) failed");
        }
    }

    // 공통: 벽시계 시간 측정
    gettimeofday(&m->end_time, NULL);
}

// 성능 출력 함수: 실행 시간, CPU 시간, 메모리 사용량 출력 (단위: ms)
static inline void print_perf_summary(const PerfMetrics* m) {
    double wall = (m->end_time.tv_sec - m->start_time.tv_sec) * 1000.0 +
        (m->end_time.tv_usec - m->start_time.tv_usec) / 1000.0;

    double user = m->child_usage.ru_utime.tv_sec * 1000.0 + m->child_usage.ru_utime.tv_usec / 1000.0;
    double sys = m->child_usage.ru_stime.tv_sec * 1000.0 + m->child_usage.ru_stime.tv_usec / 1000.0;
    long mem_kb = m->child_usage.ru_maxrss;
    long vctx = m->child_usage.ru_nvcsw;
    long ivctx = m->child_usage.ru_nivcsw;

    printf("\nTotal compression time: %.3f ms\n", wall);
    printf("CPU User time (all):    %.3f ms\n", user);
    printf("CPU System time (all):  %.3f ms\n", sys);
    printf("Max Memory Usage:       %ld KB\n", mem_kb);
    printf("Voluntary Ctx Switches:   %ld\n", vctx);
    printf("Involuntary Ctx Switches: %ld\n", ivctx);
    printf("Total Context Switches:   %ld\n", vctx + ivctx);
}

#endif
