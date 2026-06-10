/* fq_util.c - 플랫폼 독립 유틸 */
#include "fq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
#  include <time.h>
#endif

uint64_t fq_now_wall_ms(void)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 1601-01-01 -> 1970-01-01, 100ns 단위 */
    return (t - 116444736000000000ULL) / 10000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

uint32_t fq_pid(void)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

void fq_gen_id(char *buf, size_t n)
{
    static uint32_t ctr = 0;
    uint64_t t = fq_now_wall_ms();
    uint32_t c = ++ctr;
    /* 진짜 UUID는 아니지만 pid+seq+time 조합으로 노드 내/간 충돌 회피 (스캐폴드) */
    snprintf(buf, n, "%08x%08x%08x", fq_pid(), c, (uint32_t)(t & 0xffffffffu));
}

char *fq_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void fq_path(char *out, size_t n, const char *root, const char *sub, const char *name)
{
    if (name && name[0])
        snprintf(out, n, "%s/%s/%s", root, sub, name);
    else
        snprintf(out, n, "%s/%s", root, sub);
}

void fq_parse_attempt(const char *name, char *base_out, size_t base_n,
                      uint32_t *attempt_out)
{
    /* ".a<digits>" 접미사 탐지 */
    const char *p = strstr(name, ".a");
    uint32_t attempt = 0;
    size_t base_len = strlen(name);

    /* 마지막 ".a<digits>" 만 인정 (이름 끝이 숫자로 끝나야 함) */
    const char *cand = NULL;
    const char *s = name;
    while ((s = strstr(s, ".a")) != NULL) {
        const char *d = s + 2;
        if (*d >= '0' && *d <= '9') {
            const char *e = d;
            while (*e >= '0' && *e <= '9') e++;
            if (*e == '\0') cand = s;  /* 문자열 끝까지 숫자 */
        }
        s += 2;
    }
    if (cand) {
        attempt = (uint32_t)strtoul(cand + 2, NULL, 10);
        base_len = (size_t)(cand - name);
    }
    (void)p;
    if (base_len >= base_n) base_len = base_n - 1;
    memcpy(base_out, name, base_len);
    base_out[base_len] = '\0';
    if (attempt_out) *attempt_out = attempt;
}

uint64_t fq_parse_inflight(const char *name, char *logical_out, size_t logical_n)
{
    const char *sep = strstr(name, "__t");
    if (!sep) {
        snprintf(logical_out, logical_n, "%s", name);
        return 0;
    }
    uint64_t token = strtoull(sep + 3, NULL, 10);
    size_t llen = (size_t)(sep - name);
    if (llen >= logical_n) llen = logical_n - 1;
    memcpy(logical_out, name, llen);
    logical_out[llen] = '\0';
    return token;
}
