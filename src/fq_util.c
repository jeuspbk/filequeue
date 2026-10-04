/* fq_util.c - 플랫폼 독립 유틸 */
#include "fq_internal.h"
#include "fq_fs.h"

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

uint64_t fq_now_mono_ms(void)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

uint64_t fq_lease_ms(void)
{
    /* 테스트에서 FQ_LEASE_MS_OVERRIDE로 단축 가능 */
    const char *e = getenv("FQ_LEASE_MS_OVERRIDE");
    if (e && e[0]) {
        unsigned long long v = strtoull(e, NULL, 10);
        if (v > 0) return (uint64_t)v;
    }
    return FQ_LEASE_MS;
}

uint64_t fq_election_stale_ms(void)
{
    /* 하트비트도 election.lock이 필요하므로, 락을 쥔 채 죽은 노드가 남긴 락은 리더의 리스가 끝나기
     * 전에 회수돼야 한다(아니면 리더가 갱신을 못 해 내려놓고, 회수 시점까지 아무도 리더가 아니다).
     * 리스의 1/3이면 하트비트 주기 안에 회수된다. 임계 구역은 ms 단위라 정상 보유자를 회수하지 않는다. */
    uint64_t v = fq_lease_ms() / 3;
    return v < FQ_ELECTION_STALE_MS ? v : FQ_ELECTION_STALE_MS;
}

int fq_read_replaced(const char *path, void **out, size_t *out_len)
{
    /* 일부 플랫폼(Cygwin/NTFS 실측 약 0.4%)에서는 교체형 rename 순간 읽는 쪽이 ENOENT를 본다.
     * 원래 있던 파일이 교체 중일 뿐이므로 잠깐 기다려 다시 읽는다. 정말 없을 때만 ENOENT. */
    int rc = fq_fs_read_file(path, out, out_len);
    for (int i = 0; rc == FQ_ENOENT && i < FQ_REPLACE_READ_RETRIES; i++) {
        fq_sleep_ms(1);
        rc = fq_fs_read_file(path, out, out_len);
    }
    return rc;
}

int fq_valid_ident(const char *s, size_t max_len)
{
    if (!s || !s[0]) return 0;
    size_t n = 0;
    for (const char *p = s; *p; p++, n++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return 0;
        if (c == '_' && p[1] == '_') return 0;
    }
    return n <= max_len;
}

uint32_t fq_pid(void)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

void fq_sleep_ms(unsigned ms)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* 프로세스 전역 카운터. 스레드마다 핸들을 따로 열어 발행해도 id가 겹치지 않도록 원자적으로 늘린다.
 * (node_id가 같으므로 id가 겹치면 tmp 이름이 같아져 한쪽 메시지가 조용히 사라진다.) */
#if defined(_WIN32) && !defined(__CYGWIN__)
static volatile LONG gen_ctr = 0;
#  define GEN_CTR_NEXT() ((uint32_t)InterlockedIncrement(&gen_ctr))
#else
static uint32_t gen_ctr = 0;
#  define GEN_CTR_NEXT() (__atomic_add_fetch(&gen_ctr, 1u, __ATOMIC_RELAXED))
#endif

void fq_gen_id(char *buf, size_t n)
{
    static uint32_t salt = 0;   /* 동시 초기화로 값이 갈려도 카운터가 유일하므로 무해 */
    uint64_t t = fq_now_wall_ms();
    uint32_t c = GEN_CTR_NEXT();
    /* pid+카운터+ms만으로는 pid가 같은 두 노드가 같은 ms에 같은 번째 id를 만들 수 있다
     * (공유 tmp/·control/에서 파일명 충돌 → 덮어쓰기). 프로세스마다 한 번 정하는 salt
     * (첫 호출 시각 해시 ^ 스택 주소(ASLR) ^ pid 해시)를 섞어 그 확률을 없앤다. */
    if (salt == 0) {
        uintptr_t a = (uintptr_t)&c;
        salt = (uint32_t)(t * 2654435761ULL) ^ (uint32_t)(a >> 4) ^ (fq_pid() * 0x9E3779B1u);
        if (salt == 0) salt = 1;
    }
    snprintf(buf, n, "%08x%08x%08x%08x", fq_pid(), salt, c, (uint32_t)(t & 0xffffffffu));
}

void fq_default_node_id(char *buf, size_t n)
{
    /* pid만 쓰면 컨테이너마다 pid 1이라 모든 노드가 "node-1"이 된다. 호스트명을 붙인다.
     * 식별자 규칙([A-Za-z0-9._-], "__" 금지)에 맞지 않는 문자는 '-'로 바꾼다. */
    char host[256] = "";
#if defined(_WIN32) && !defined(__CYGWIN__)
    DWORD hn = (DWORD)sizeof(host);
    if (!GetComputerNameA(host, &hn)) host[0] = '\0';
#else
    if (gethostname(host, sizeof(host)) != 0) host[0] = '\0';
    host[sizeof(host) - 1] = '\0';
#endif
    char clean[41];
    size_t k = 0;
    for (const char *p = host; *p && k < sizeof(clean) - 1; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok || (c == '_' && k > 0 && clean[k - 1] == '_')) c = '-';
        clean[k++] = c;
    }
    clean[k] = '\0';
    snprintf(buf, n, "%s-%u", k ? clean : "node", fq_pid());
}

size_t fq_path_root_len(const char *p)
{
#define IS_SEP(c) ((c) == '/' || (c) == '\\')
    if (IS_SEP(p[0]) && IS_SEP(p[1])) {
        /* UNC: \\server\share 또는 //server/share → share 뒤 구분자(또는 끝)까지 */
        size_t i = 2;
        int parts = 0;
        for (; p[i]; i++)
            if (IS_SEP(p[i]) && ++parts == 2) break;
        return i;
    }
    if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':')
        return 2;   /* 드라이브 문자 */
    return 0;
#undef IS_SEP
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
