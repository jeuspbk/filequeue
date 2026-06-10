/* test_basic.c - 발행/리더십/claim/ack 라운드트립 + stale 복구 스모크 테스트 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"  /* fq_parse_inflight (테스트용 내부 헬퍼) */

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
    const char *root = argc > 1 ? argv[1] : "_basic_test";
    printf("== filequeue 기본 라운드트립 (%s) ==\n", root);

    fq_queue *q = NULL;
    CHECK(fq_open(root, "nodeA", &q) == FQ_OK && q != NULL, "fq_open");
    if (!q) return 1;

    /* 발행 3건 */
    printf("[1] 발행\n");
    CHECK(fq_publish(q, "msg-A", 5, NULL) == FQ_OK, "publish A");
    CHECK(fq_publish(q, "msg-B", 5, NULL) == FQ_OK, "publish B");
    CHECK(fq_publish(q, "msg-C", 5, NULL) == FQ_OK, "publish C");

    /* 리더십 획득 */
    printf("[2] 리더십\n");
    fq_lease lease;
    CHECK(fq_acquire_leadership(q, &lease) == FQ_OK, "acquire leadership");
    CHECK(lease.token >= 1, "fencing token 발급");

    /* claim + ack 루프: 3건 모두 소비 */
    printf("[3] claim/ack\n");
    int consumed = 0;
    for (;;) {
        fq_msg *m = NULL;
        int rc = fq_claim(q, &lease, &m);
        if (rc == FQ_EEMPTY) break;
        CHECK(rc == FQ_OK && m != NULL, "claim 성공");
        if (rc != FQ_OK) break;
        consumed++;
        CHECK(fq_ack(q, m) == FQ_OK, "ack 성공");
    }
    CHECK(consumed == 3, "3건 소비");

    /* 빈 큐 claim */
    {
        fq_msg *m = NULL;
        CHECK(fq_claim(q, &lease, &m) == FQ_EEMPTY, "빈 큐 → EEMPTY");
    }

    /* [4] stale 복구: nodeA가 메시지를 claim한 채 "죽었다"고 가정.
     *     (a) nodeA의 inflight 파일을 옛 token으로 위장
     *     (b) nodeA의 리스를 만료시킴 (leader.info를 과거 expiry로 덮어씀)
     *     (c) nodeB가 인수 → recover_stale → 메시지가 다시 소비 가능해야 함 */
    printf("[4] stale inflight 복구 (failover)\n");
    {
        fq_publish(q, "stale-1", 7, NULL);
        fq_msg *m = NULL;
        int rc = fq_claim(q, &lease, &m);   /* nodeA(token=lease.token)가 claim */
        CHECK(rc == FQ_OK, "nodeA가 stale 후보 claim");
        if (rc == FQ_OK) {
            /* (a) inflight 이름의 token을 0(아주 오래된 리더)으로 위장 */
            char src[2048], dst[2048], logical[640];
            fq_parse_inflight(m->name, logical, sizeof(logical));
            snprintf(src, sizeof(src), "%s/inflight/%s", root, m->name);
            snprintf(dst, sizeof(dst), "%s/inflight/%s__t0", root, logical);
            CHECK(fq_fs_rename_noreplace(src, dst) == FQ_OK, "inflight를 옛 token으로 위장");
            free(m->data); free(m);

            /* (b) nodeA 리스 만료 시뮬레이션: leader.info를 과거 expiry(=1ms)로 덮어씀.
             *     token은 유지하여 nodeB가 token+1을 받도록 함. */
            char li[2048], line[128];
            snprintf(li, sizeof(li), "%s/control/leader.info", root);
            snprintf(line, sizeof(line), "nodeA %llu 1\n",
                     (unsigned long long)lease.token);
            fq_fs_unlink(li);
            CHECK(fq_fs_write_sync(li, line, strlen(line)) == FQ_OK, "nodeA 리스 만료시킴");

            /* (c) nodeB 인수 — 별도 노드이므로 node_id "nodeB"로 핸들을 따로 연다 */
            fq_queue *qB = NULL;
            CHECK(fq_open(root, "nodeB", &qB) == FQ_OK && qB, "nodeB 핸들 open");
            fq_lease lease2;
            CHECK(fq_acquire_leadership(qB, &lease2) == FQ_OK, "nodeB 인수");
            CHECK(lease2.token > lease.token, "token 증가 (fencing)");
            CHECK(fq_recover_stale(qB, &lease2) == FQ_OK, "recover_stale 실행");

            /* 복구되어 다시 claim 가능해야 함 */
            fq_msg *m2 = NULL;
            int rc2 = fq_claim(qB, &lease2, &m2);
            CHECK(rc2 == FQ_OK, "복구된 메시지 재claim");
            if (rc2 == FQ_OK) {
                CHECK(m2->attempt >= 1, "attempt 증가됨");
                CHECK(m2->len == 7 && memcmp(m2->data, "stale-1", 7) == 0, "페이로드 보존");
                fq_ack(qB, m2);
            }
            fq_close(qB);
        }
    }

    fq_close(q);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
