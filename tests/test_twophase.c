/* test_twophase.c - fq_adopt / fq_take: 큐 밖 파일과 큐 사이의 원자적 이동(멱등) */
#include "fq.h"
#include "fq_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

static int want_k2(const void *data, size_t len, void *ud)
{
    (void)ud;
    return len == 2 && memcmp(data, "k2", 2) == 0;
}

static int count_cb(const char *name, void *ud)
{
    (void)name;
    (*(int *)ud)++;
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "_twophase_test";
    printf("== filequeue 2단계 발행/꺼내기 (%s) ==\n", root);

    char qroot[512], stage[512], f1[600], f2[600];
    snprintf(qroot, sizeof(qroot), "%s/q", root);
    snprintf(stage, sizeof(stage), "%s/stage", root);
    snprintf(f1, sizeof(f1), "%s/e1", stage);
    snprintf(f2, sizeof(f2), "%s/d1", stage);

    fq_queue *q = NULL;
    CHECK(fq_open(qroot, "nodeT", &q) == FQ_OK && q, "fq_open");
    if (!q) return 1;
    CHECK(fq_fs_mkdirs(stage) == FQ_OK, "보관 디렉터리");

    printf("[1] fq_adopt\n");
    CHECK(fq_fs_write_sync(f1, "staged", 6) == FQ_OK, "보관 파일 기록");
    fq_msg *m = NULL;
    CHECK(fq_consume(q, &m) == FQ_EEMPTY, "adopt 전: 큐는 비어 있음");
    CHECK(fq_adopt(q, f1, NULL) == FQ_OK, "adopt");
    CHECK(fq_fs_exists(f1) == 0, "원본은 옮겨져 사라짐");
    CHECK(fq_adopt(q, f1, NULL) == FQ_ENOENT, "다시 adopt: FQ_ENOENT (멱등, 중복 없음)");

    printf("[2] fq_take\n");
    CHECK(fq_consume(q, &m) == FQ_OK && m && m->len == 6 && memcmp(m->data, "staged", 6) == 0,
          "adopt한 메시지를 consume");
    CHECK(m && fq_take(q, m, f2) == FQ_OK, "take: 큐 밖 파일로 꺼냄");
    CHECK(fq_fs_exists(f2) == 1, "꺼낸 파일이 보관 디렉터리에 있음");
    fq_msg *m2 = NULL;
    CHECK(fq_consume(q, &m2) == FQ_EEMPTY, "take 뒤: 큐는 비어 있음 (inflight에도 없음)");
    fq_lease lease;
    CHECK(fq_acquire_leadership(q, &lease) == FQ_OK && fq_recover_stale(q, &lease) >= 0, "stale 복구");
    CHECK(fq_consume(q, &m2) == FQ_EEMPTY, "꺼낸 메시지는 stale 복구로도 되살아나지 않음");
    /* 저수준으로 얻은 리스는 직접 반납한다. fq_close는 fq_consume의 리스(token이 다름)만 반납하므로,
     * 두지 않으면 리스가 만료될 때까지 남아 같은 디렉터리로 곧바로 다시 돌린 테스트가 리더가 되지 못한다. */
    fq_release_leadership(q, &lease);

    printf("[3] 되돌리기 (rollback = adopt)\n");
    CHECK(fq_adopt(q, f2, NULL) == FQ_OK, "꺼낸 파일을 다시 adopt");
    CHECK(fq_consume(q, &m2) == FQ_OK && m2 && m2->len == 6, "다시 consume 가능");
    if (m2) fq_ack(q, m2);

    printf("[4] take 실패 시 claim 유지\n");
    CHECK(fq_publish(q, "x", 1, NULL) == FQ_OK, "publish");
    fq_msg *m3 = NULL;
    CHECK(fq_consume(q, &m3) == FQ_OK && m3, "consume");
    char bad[600];
    snprintf(bad, sizeof(bad), "%s/no/such/dir/x", root);
    CHECK(m3 && fq_take(q, m3, bad) != FQ_OK, "없는 디렉터리로 take: 실패");
    char busy[600];
    snprintf(busy, sizeof(busy), "%s/busy", stage);
    CHECK(fq_fs_write_sync(busy, "keep", 4) == FQ_OK, "이미 있는 대상 파일");
    CHECK(m3 && fq_take(q, m3, busy) == FQ_EEXIST, "있는 파일로 take: FQ_EEXIST (덮어쓰지 않음)");
    {
        void *buf = NULL; size_t len = 0;
        CHECK(fq_fs_read_file(busy, &buf, &len) == FQ_OK && len == 4 && memcmp(buf, "keep", 4) == 0,
              "대상 파일 내용 그대로");
        free(buf);
        fq_fs_unlink(busy);
    }
    CHECK(m3 && fq_nack(q, m3) == FQ_OK, "m은 claim 상태 그대로: nack 가능");
    CHECK(fq_consume(q, &m3) == FQ_OK && m3 && fq_ack(q, m3) == FQ_OK, "nack한 것을 다시 받아 ack");

    printf("[5] fq_return: nack처럼 attempt+1, 한도면 dead/\n");
    /* 같은 디렉터리로 재실행해도 통과하도록 dead/는 전후 차이로 센다 (이전 실행 잔여물 무시) */
    char dead[600];
    snprintf(dead, sizeof(dead), "%s/dead", qroot);
    int ndead_before = 0;
    fq_fs_list(dead, count_cb, &ndead_before);
    CHECK(fq_publish(q, "r", 1, NULL) == FQ_OK, "publish");
    char f3[600];
    snprintf(f3, sizeof(f3), "%s/d2", stage);
    uint32_t seen = 0;
    int rounds = 0, ok = 1;
    for (;;) {
        fq_msg *mr = NULL;
        int rc = fq_consume(q, &mr);
        if (rc == FQ_EEMPTY) break;
        if (rc != FQ_OK || !mr) { ok = 0; break; }
        if (mr->attempt != seen) ok = 0;         /* 되돌릴 때마다 attempt가 하나씩 는다 */
        uint32_t a = mr->attempt;
        if (fq_take(q, mr, f3) != FQ_OK || fq_return(q, f3, a) != FQ_OK) { ok = 0; break; }
        seen = a + 1;
        if (++rounds > FQ_MAX_ATTEMPTS + 1) { ok = 0; break; }
    }
    CHECK(ok && rounds == FQ_MAX_ATTEMPTS, "FQ_MAX_ATTEMPTS번 되돌린 뒤 큐에서 사라짐");
    int ndead = 0;
    CHECK(fq_fs_list(dead, count_cb, &ndead) == FQ_OK && ndead == ndead_before + 1, "dead/에 하나 추가");
    CHECK(fq_return(q, f3, 0) == FQ_ENOENT, "없는 파일을 되돌림: FQ_ENOENT (멱등)");

    printf("[6] fq_consume_if / fq_release\n");
    CHECK(fq_publish(q, "k1", 2, NULL) == FQ_OK && fq_publish(q, "k2", 2, NULL) == FQ_OK &&
          fq_publish(q, "k3", 2, NULL) == FQ_OK, "publish k1 k2 k3");
    fq_msg *s = NULL;
    CHECK(fq_consume_if(q, want_k2, NULL, &s) == FQ_OK && s && memcmp(s->data, "k2", 2) == 0,
          "k2만 골라 claim");
    if (s) fq_ack(q, s);
    CHECK(fq_consume_if(q, want_k2, NULL, &s) == FQ_EEMPTY, "k2는 더 없음: FQ_EEMPTY");
    fq_msg *p = NULL;
    CHECK(fq_consume(q, &p) == FQ_OK && p && memcmp(p->data, "k1", 2) == 0, "들여다보기: k1 claim");
    uint32_t pa = p ? p->attempt : 99;
    CHECK(p && fq_release(q, p) == FQ_OK, "release");
    CHECK(fq_consume(q, &p) == FQ_OK && p && memcmp(p->data, "k1", 2) == 0 && p->attempt == pa,
          "release 뒤에도 k1이 맨 앞, attempt 그대로");
    if (p) fq_ack(q, p);
    CHECK(fq_consume(q, &p) == FQ_OK && p && memcmp(p->data, "k3", 2) == 0, "그다음 k3");
    if (p) fq_ack(q, p);

    printf("[7] fq_consume_if: 읽을 수 없는 메시지는 시도로 쳐서 결국 dead/\n");
    {
        char junk[700];
        /* 실행마다 다른 이름: 이전 실행이 dead/에 남긴 같은 이름의 빈 디렉터리는 rename이 덮어써 개수가 늘지 않는다 */
        uint64_t stamp = 0;
        fq_fs_now_ms(stage, "nodeT", &stamp);
        snprintf(junk, sizeof(junk), "%s/incoming/0000000000000000-0000000000-x-junk%llu.msg",
                 qroot, (unsigned long long)stamp);
        int nd0 = 0;
        fq_fs_list(dead, count_cb, &nd0);
        CHECK(fq_fs_mkdirs(junk) == FQ_OK, "incoming에 일반 파일이 아닌 항목(디렉터리)");
        int rc = FQ_OK;
        for (int i = 0; i < FQ_MAX_ATTEMPTS + 2; i++) {
            fq_msg *jm = NULL;
            rc = fq_consume_if(q, want_k2, NULL, &jm);
            if (rc == FQ_OK) fq_ack(q, jm);
        }
        CHECK(rc == FQ_EEMPTY, "건너뛰기를 반복하지 않고 큐에서 빠짐");
        int nd = 0;
        CHECK(fq_fs_list(dead, count_cb, &nd) == FQ_OK && nd == nd0 + 1, "dead/로 격리");
    }

    fq_close(q);
    printf("%s (%d fail)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
