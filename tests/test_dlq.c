/* test_dlq.c - DLQ: fq_dead_list / fq_redrive / fq_dead_drop */
#include "fq.h"
#include "fq_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <windows.h>
#endif

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

typedef struct { int n; char name[300]; char data[64]; size_t len; uint32_t attempt; int had_data; } seen_t;

static int collect(const char *name, const void *data, size_t len, uint32_t attempt, void *ud)
{
    seen_t *s = ud;
    if (s->n++ == 0) {
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->had_data = data != NULL;
        s->len = len < sizeof(s->data) ? len : sizeof(s->data) - 1;
        if (data) memcpy(s->data, data, s->len);
        s->data[s->len] = '\0';
        s->attempt = attempt;
    }
    return 0;
}

static int stop7(const char *name, const void *data, size_t len, uint32_t attempt, void *ud)
{
    (void)name; (void)data; (void)len; (void)attempt; (void)ud;
    return 7;
}

/* 메시지 하나를 한도까지 nack해 dead/로 보낸다 */
static void poison(fq_queue *q, const char *s)
{
    fq_publish(q, s, strlen(s), NULL);
    fq_msg *m = NULL;
    for (int i = 0; i < FQ_MAX_ATTEMPTS && fq_consume(q, &m) == FQ_OK; i++) fq_nack(q, m);
}

static void drain_dead(fq_queue *q)
{
    for (;;) {
        seen_t s = {0};
        fq_dead_list(q, collect, &s);
        if (!s.n || fq_dead_drop(q, s.name) != FQ_OK) break;
    }
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "_dlq_test";
    printf("== filequeue DLQ (%s) ==\n", root);
    fq_queue *q = NULL;
    CHECK(fq_open(root, "nodeD", &q) == FQ_OK && q, "fq_open");
    if (!q) return 1;
    fq_msg *m = NULL;
    while (fq_consume(q, &m) == FQ_OK) fq_ack(q, m);      /* 이전 실행의 잔여 */
    drain_dead(q);

    printf("[1] 한도까지 실패한 메시지가 보이고, 되살리면 attempt 0으로 다시 전달\n");
    poison(q, "p1");
    seen_t s = {0};
    CHECK(fq_dead_list(q, collect, &s) == FQ_OK && s.n == 1, "dead/에 1건");
    CHECK(s.had_data && strcmp(s.data, "p1") == 0, "  ... 페이로드 p1");
    CHECK(s.attempt == FQ_MAX_ATTEMPTS - 1, "  ... attempt = 한도 - 1");
    CHECK(fq_redrive(q, s.name) == FQ_OK, "fq_redrive");
    m = NULL;
    CHECK(fq_consume(q, &m) == FQ_OK && m && m->len == 2 && memcmp(m->data, "p1", 2) == 0 && m->attempt == 0,
          "  ... 다시 전달됨 (attempt 0)");
    if (m) fq_ack(q, m);
    CHECK(fq_redrive(q, s.name) == FQ_ENOENT, "다시 되살리기: FQ_ENOENT (이미 옮김)");
    seen_t s2 = {0};
    CHECK(fq_dead_list(q, collect, &s2) == FQ_OK && s2.n == 0, "dead/ 비었음");

    printf("[2] 버리기, 콜백 중단 값 전달\n");
    poison(q, "p2");
    CHECK(fq_dead_list(q, stop7, NULL) == 7, "콜백이 7을 돌려주면 7");
    seen_t s3 = {0};
    fq_dead_list(q, collect, &s3);
    CHECK(s3.n == 1 && fq_dead_drop(q, s3.name) == FQ_OK, "fq_dead_drop");
    CHECK(fq_dead_drop(q, s3.name) == FQ_ENOENT, "다시 버리기: FQ_ENOENT");
    CHECK(fq_consume(q, &m) == FQ_EEMPTY, "incoming도 비어 있음");

    printf("[3] 잘못된 이름, 되돌려도 claim할 수 없는 긴 이름\n");
    CHECK(fq_redrive(q, "../incoming/x") == FQ_EINVAL, "경로 구분자: FQ_EINVAL");
    CHECK(fq_dead_drop(q, "..") == FQ_EINVAL, "\"..\": FQ_EINVAL");
    CHECK(fq_redrive(q, "") == FQ_EINVAL, "빈 이름: FQ_EINVAL");
    char lname[300], lpath[1600];
    size_t ln = 240;                                     /* POSIX: + claim 접미사가 255자를 넘는다 */
#if defined(_WIN32) && !defined(__CYGWIN__)
    {   /* Win32: dead/에는 들어가지만 incoming/ + claim 접미사로는 260자를 넘는 길이 */
        char dd[1200], full[1200];
        snprintf(dd, sizeof(dd), "%s/dead", root);
        ln = 259 - GetFullPathNameA(dd, sizeof(full), full, NULL) - 1;
    }
#endif
    memset(lname, 'L', ln);
    lname[ln] = '\0';
    snprintf(lpath, sizeof(lpath), "%s/dead/%s", root, lname);
    CHECK(fq_fs_write_sync(lpath, "long", 4) == FQ_OK, "긴 이름 항목을 dead/에 직접 둠");
    {
        CHECK(fq_redrive(q, lname) == FQ_EINVAL, "긴 이름 되살리기: FQ_EINVAL");
        CHECK(fq_fs_exists(lpath) == 1, "  ... dead/에 그대로");
        CHECK(fq_dead_drop(q, lname) == FQ_OK, "  ... 버릴 수는 있다");
    }

    fq_close(q);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
