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

    /* 같은 디렉터리로 곧바로 재실행해도 통과하도록, 이전 실행이 남긴 리스를 만료시킨다
     * (리스 15초 안에 다시 돌리면 nodeA가 리더십을 못 얻는다). 테스트 전용 조작. */
    {
        char li[1280];
        fq_path(li, sizeof(li), root, FQ_LEADER_INFO, NULL);
        fq_fs_write_sync(li, "none 0 0\n", 9);
    }

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

    /* [5] 식별자 검증: 파일명·leader.info를 깨뜨리는 node_id / stream_key는 FQ_EINVAL */
    printf("[5] node_id / stream_key 검증\n");
    {
        fq_queue *bad = NULL;
        CHECK(fq_open(root, "node A", &bad) == FQ_EINVAL, "공백 node_id 거부");
        CHECK(fq_open(root, "a__t5", &bad) == FQ_EINVAL, "\"__\" 포함 node_id 거부");
        CHECK(fq_open(root, "a/b", &bad) == FQ_EINVAL, "'/' 포함 node_id 거부");
        CHECK(fq_open(root, "0123456789012345678901234567890123456789012345678901234567890123",
                      &bad) == FQ_EINVAL, "64자 node_id 거부");
        CHECK(fq_publish(q, "x", 1, "a b") == FQ_EINVAL, "공백 stream_key 거부");
        CHECK(fq_publish(q, "x", 1, "p__t1") == FQ_EINVAL, "\"__\" 포함 stream_key 거부");
        CHECK(fq_publish(q, "x", 1, "order-42.v2") == FQ_OK, "정상 stream_key 허용");
        /* 다음 실행을 위해 방금 발행한 것을 비운다. [4]에서 nodeB가 리스를 쥐고 있으므로 먼저 만료시킨다. */
        char li[1408];
        fq_path(li, sizeof(li), root, FQ_LEADER_INFO, NULL);
        fq_fs_write_sync(li, "none 0 0\n", 9);
        fq_lease l3;
        if (fq_acquire_leadership(q, &l3) == FQ_OK) {
            fq_msg *m = NULL;
            if (fq_claim(q, &l3, &m) == FQ_OK) fq_ack(q, m);   /* 다음 실행을 위해 비움 */
        }
    }

    /* [6] GC: control/의 크래시 잔재만 지우고 leader.info는 남긴다 */
    printf("[6] GC가 control/ 잔재 정리\n");
    {
        const char *junk[] = { ".now-crashed", "leader-nodeZ-1.tmp", "election.lock.stale-1" };
        char p[1408];
        for (size_t i = 0; i < 3; i++) {
            fq_path(p, sizeof(p), root, FQ_DIR_CONTROL, junk[i]);
            fq_fs_write_sync(p, "", 0);
        }
        uint64_t t0 = fq_now_wall_ms();
        while (fq_now_wall_ms() < t0 + 50) { }          /* mtime이 확실히 과거가 되도록 */
        CHECK(fq_gc(q, 0) >= 3, "잔재 3개 이상 삭제");
        int left = 0;
        for (size_t i = 0; i < 3; i++) {
            fq_path(p, sizeof(p), root, FQ_DIR_CONTROL, junk[i]);
            left += fq_fs_exists(p) == 1;
        }
        CHECK(left == 0, "잔재가 남지 않음");
        fq_path(p, sizeof(p), root, FQ_DIR_CONTROL, "leader.info");
        CHECK(fq_fs_exists(p) == 1, "leader.info는 보존");
    }

    /* [7] 리더십 경합 회귀: 같은 node_id 두 핸들, 갱신의 락 직렬화, token high-water */
    printf("[7] 리더십 경합 회귀\n");
    {
        char lr[1100], li[1408], lk[1408], inf[1408];
        snprintf(lr, sizeof(lr), "%s-lead", root);
        fq_queue *d1 = NULL, *d2 = NULL;
        CHECK(fq_open(lr, "dup", &d1) == FQ_OK && fq_open(lr, "dup", &d2) == FQ_OK,
              "같은 node_id로 핸들 2개 open");
        fq_path(li, sizeof(li), lr, FQ_LEADER_INFO, NULL);
        fq_path(lk, sizeof(lk), lr, FQ_ELECTION_LCK, NULL);
        fq_path(inf, sizeof(inf), lr, FQ_DIR_INFLIGHT, "0-0-x-y.msg__t10");
        fq_fs_write_sync(li, "none 0 0\n", 9);        /* 재실행 대비: 이전 리스 무효화 */
        fq_fs_unlink(lk);
        fq_fs_unlink(inf);

        fq_lease l1, l2;
        CHECK(fq_acquire_leadership(d1, &l1) == FQ_OK, "핸들1 인수");
        CHECK(fq_acquire_leadership(d2, &l2) == FQ_ELOCKED,
              "node_id가 같아도 다른 핸들은 유효한 리스를 빼앗지 못함");
        CHECK(fq_renew_lease(d1, &l1) == FQ_OK, "핸들1 갱신 유지");

        /* 선출이 진행 중(election.lock 보유)이면 갱신은 leader.info를 건드리지 않고 ELOCKED */
        CHECK(fq_fs_create_new(lk) == FQ_OK, "election.lock 점유 흉내");
        uint64_t before = l1.lease_expiry_ms;
        CHECK(fq_renew_lease(d1, &l1) == FQ_ELOCKED && l1.lease_expiry_ms == before,
              "락이 잡혀 있으면 갱신은 ELOCKED (leader.info 그대로)");
        fq_fs_unlink(lk);
        CHECK(fq_renew_lease(d1, &l1) == FQ_OK, "락이 풀리면 갱신 성공");

        /* leader.info가 되돌아가도(token 3) inflight에 남은 token 10보다 큰 token을 발급 */
        fq_fs_write_sync(li, "old 3 1\n", 8);
        fq_fs_write_sync(inf, "z", 1);
        CHECK(fq_acquire_leadership(d2, &l2) == FQ_OK && l2.token == 11,
              "새 token = max(leader.info, inflight) + 1");
        CHECK(fq_renew_lease(d1, &l1) == FQ_ENOLEADER, "옛 핸들의 갱신은 ENOLEADER");

        fq_fs_unlink(inf);
        fq_fs_write_sync(li, "none 0 0\n", 9);
        fq_close(d1);
        fq_close(d2);
    }

    /* [8] 경로 접두부: 드라이브·UNC는 mkdirs가 만들지 않는다 */
    printf("[8] 경로 접두부\n");
    CHECK(fq_path_root_len("D:/q") == 2, "드라이브 문자");
    CHECK(fq_path_root_len("//srv/share/q") == 11, "UNC (/)");
    CHECK(fq_path_root_len("\\\\srv\\share\\q") == 11, "UNC (\\)");
    CHECK(fq_path_root_len("/a/b") == 0 && fq_path_root_len("rel/x") == 0, "일반 경로");

    /* [9] 기본 node_id = <호스트명>-<pid>, 식별자 규칙 만족 */
    if (!getenv("FQ_NODE_ID")) {
        printf("[9] 기본 node_id\n");
        fq_queue *dq = NULL;
        CHECK(fq_open(root, NULL, &dq) == FQ_OK && dq, "node_id 없이 open");
        if (dq) {
            CHECK(fq_valid_ident(dq->node_id, 63) && strchr(dq->node_id, '-'), dq->node_id);
            fq_close(dq);
        }
    }

    fq_close(q);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
