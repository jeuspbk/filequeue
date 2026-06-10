/* test_consume.c - fq_consume() 통합 wrapper 동작 검증
 *
 * 검증:
 *   1) 활성 리더가 fq_consume 으로 큐를 끝까지 비운다 (유실 없음).
 *   2) 큐가 비면 FQ_EEMPTY 를 반환한다.
 *   3) 다른 노드(별도 핸들)는 활성 리더가 살아있는 동안 FQ_ELOCKED 를 받는다(대기).
 */
#include "fq.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "_consume_test";
    printf("== fq_consume 통합 wrapper (%s) ==\n", root);

    fq_queue *qa = NULL, *qb = NULL;
    CHECK(fq_open(root, "nodeA", &qa) == FQ_OK && qa, "fq_open A");
    CHECK(fq_open(root, "nodeB", &qb) == FQ_OK && qb, "fq_open B");
    if (!qa || !qb) return 1;

    /* 발행 5건 (발행은 리더십 불필요) */
    const int N = 5;
    for (int i = 0; i < N; i++) {
        char p[32]; int n = snprintf(p, sizeof(p), "c-%d", i);
        fq_publish(qa, p, (size_t)n, NULL);
    }

    /* A: fq_consume 으로 끝까지 소비 */
    printf("[1] A 소비\n");
    int consumed = 0, locked_seen = 0;
    for (;;) {
        fq_msg *m = NULL;
        int rc = fq_consume(qa, &m);
        if (rc == FQ_EEMPTY) break;
        if (rc == FQ_ELOCKED) { locked_seen++; break; }
        CHECK(rc == FQ_OK && m, "fq_consume → 메시지");
        if (rc != FQ_OK) break;
        fq_ack(qa, m);
        consumed++;
    }
    CHECK(consumed == N, "5건 전부 소비");
    CHECK(locked_seen == 0, "A는 락아웃되지 않음");

    /* 빈 큐 */
    {
        fq_msg *m = NULL;
        CHECK(fq_consume(qa, &m) == FQ_EEMPTY, "빈 큐 → FQ_EEMPTY");
    }

    /* B: A의 리스가 유효한 동안 대기(FQ_ELOCKED) */
    printf("[2] B 대기(Active-Passive)\n");
    {
        fq_msg *m = NULL;
        int rc = fq_consume(qb, &m);
        CHECK(rc == FQ_ELOCKED, "B는 FQ_ELOCKED (A가 활성 리더)");
    }

    fq_close(qa);
    fq_close(qb);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
