/* test_consume.c - fq_consume() 통합 wrapper 동작 검증
 *
 * 검증:
 *   1) 활성 리더가 fq_consume 으로 큐를 끝까지 비운다 (유실 없음).
 *   2) 큐가 비면 FQ_EEMPTY 를 반환한다.
 *   3) 다른 노드(별도 핸들)는 활성 리더가 살아있는 동안 FQ_ELOCKED 를 받는다(대기).
 *   4) fq_heartbeat: 리더는 즉시 갱신(FQ_OK), 비리더는 FQ_ELOCKED.
 *   5) 리스 자체 만료 감지: 마지막 갱신 뒤 리스 길이가 지나면 내려놓고 다시 얻는다(token +1).
 *   6) 하트비트마다 stale inflight 회수: 옛 token의 inflight가 failover 없이도 되살아난다.
 *   7) fq_takeover: 같은 node_id의 죽은 핸들 리스를 만료 전에 인수, 다른 node_id 리스는 그대로.
 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"   /* 리더십 내부 상태를 조작해 시나리오를 만든다 (테스트 전용) */

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

    /* fq_heartbeat */
    printf("[3] fq_heartbeat\n");
    {
        uint64_t tok = qa->lead_lease.token;
        CHECK(fq_heartbeat(qa) == FQ_OK, "리더 A: 즉시 갱신 FQ_OK");
        CHECK(qa->lead_lease.token == tok, "하트비트는 token을 바꾸지 않음");
        CHECK(fq_heartbeat(qb) == FQ_ELOCKED, "비리더 B: FQ_ELOCKED");
    }

    /* 리스 자체 만료 감지: A가 리스 길이보다 오래 멈췄다고 가정(마지막 갱신 시각을 과거로) */
    printf("[4] 리스 자체 만료 감지\n");
    {
        uint64_t tok = qa->lead_lease.token;
        qa->lead_last_renew_ms = 0;            /* 갱신한 지 아주 오래됨 */
        fq_msg *m = NULL;
        int rc = fq_consume(qa, &m);           /* 내려놓고 election.lock을 거쳐 다시 얻어야 함 */
        CHECK(rc == FQ_EEMPTY, "다시 얻은 뒤 빈 큐 → FQ_EEMPTY");
        CHECK(qa->lead_held == 1, "리더십 보유 상태로 복귀");
        CHECK(qa->lead_lease.token == tok + 1, "재획득이라 token이 +1 (옛 token으로 claim 안 함)");
    }

    /* 하트비트마다 stale 회수: 옛 token으로 찍힌 inflight 파일을 심고 하트비트 시각을 당긴다 */
    printf("[5] 하트비트마다 stale inflight 회수\n");
    {
        char inf[1408];
        char name[256];
        snprintf(name, sizeof(name), "0000000000000001-000001-ghost-deadbeef.msg__t%llu",
                 (unsigned long long)(qa->lead_lease.token - 1));
        fq_path(inf, sizeof(inf), root, FQ_DIR_INFLIGHT, name);
        CHECK(fq_fs_write_sync(inf, "zombie", 6) == FQ_OK, "옛 token의 inflight 심기");
        qa->lead_next_renew_ms = 0;            /* 다음 fq_consume에서 하트비트가 돌도록 */
        fq_msg *m = NULL;
        int rc = fq_consume(qa, &m);
        CHECK(rc == FQ_OK && m && m->len == 6 && memcmp(m->data, "zombie", 6) == 0,
              "failover 없이 하트비트에서 회수되어 소비됨");
        if (rc == FQ_OK) { CHECK(m->attempt == 1, "회수 시 attempt +1"); fq_ack(qa, m); }
    }

    printf("[6] fq_takeover: 같은 node_id의 죽은 핸들 리스를 즉시 인수\n");
    {
        /* qa가 리더로 메시지를 claim한 채 "죽었다"(닫지 않고 버림). 같은 node_id로 새 핸들을 연다. */
        CHECK(fq_publish(qa, "tk", 2, NULL) == FQ_OK, "publish");
        fq_msg *held = NULL;
        CHECK(fq_consume(qa, &held) == FQ_OK && held, "qa가 claim (inflight에 남음)");
        uint64_t old_token = qa->lead_lease.token;

        fq_queue *qa2 = NULL, *qc = NULL;
        CHECK(fq_open(root, "nodeA", &qa2) == FQ_OK && qa2, "같은 node_id로 새 핸들");
        CHECK(fq_open(root, "nodeC", &qc) == FQ_OK && qc, "다른 node_id 핸들");
        fq_msg *m = NULL;
        CHECK(qa2 && fq_consume(qa2, &m) == FQ_ELOCKED, "takeover 전: 리스가 유효해 대기");
        CHECK(qc && fq_takeover(qc) == FQ_ELOCKED, "다른 node_id의 takeover: 유효한 리스는 건드리지 않음");
        CHECK(qa2 && fq_takeover(qa2) == FQ_OK, "같은 node_id의 takeover: 즉시 인수");
        CHECK(qa2 && qa2->lead_lease.token > old_token, "token 증가 (fencing)");
        CHECK(qa2 && fq_takeover(qa2) == FQ_OK, "다시 호출해도 FQ_OK (이미 리더)");
        int rc = qa2 ? fq_consume(qa2, &m) : FQ_ERR;
        CHECK(rc == FQ_OK && m && m->len == 2 && memcmp(m->data, "tk", 2) == 0 && m->attempt == 1,
              "선임자의 inflight가 회수되어 바로 소비됨 (attempt +1)");
        if (rc == FQ_OK) fq_ack(qa2, m);
        CHECK(qa2 && fq_consume(qa2, &m) == FQ_EEMPTY, "그 뒤 큐는 빔 (중복 없음)");
        CHECK(fq_heartbeat(qa) == FQ_ELOCKED, "옛 핸들의 하트비트: 리더십 상실");
        if (held) CHECK(fq_ack(qa, held) == FQ_ENOENT, "옛 핸들의 ack: 이미 회수됨");
        if (qc) fq_close(qc);
        if (qa2) fq_close(qa2);
    }

    fq_close(qa);
    fq_close(qb);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
