/* test_batch.c - fq_publish_batch: 순서, 검증 실패 시 아무것도 안 씀, 기록 실패 시 큐 그대로, 잔재 없음 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"   /* fq_path */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

static int count_cb(const char *name, void *ud) { (void)name; (*(int *)ud)++; return 0; }
static int count_dir(fq_queue *q, const char *sub)
{
    char d[1280];
    int n = 0;
    fq_path(d, sizeof(d), q->root, sub, NULL);
    fq_fs_list(d, count_cb, &n);
    return n;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "_batch_test";
    printf("== fq_publish_batch (%s) ==\n", root);
    fq_queue *q = NULL;
    CHECK(fq_open(root, "nodeBt", &q) == FQ_OK && q, "fq_open");
    if (!q) return 1;
    fq_msg *m = NULL;
    while (fq_consume(q, &m) == FQ_OK) fq_ack(q, m);      /* 이전 실행의 잔여 */

    printf("[1] 항목 순서대로 모두 발행, 꼬리에\n");
    {
        enum { N = 40 };
        char pay[N][16];
        fq_item it[N];
        for (int i = 0; i < N; i++) {
            snprintf(pay[i], sizeof(pay[i]), "b%02d", i);
            it[i].data = pay[i];
            it[i].len = strlen(pay[i]);
            it[i].stream_key = (i % 3 == 0) ? "s1" : NULL;
        }
        CHECK(fq_publish(q, "first", 5, NULL) == FQ_OK, "배치 전에 하나 발행");
        size_t pub = 99;
        CHECK(fq_publish_batch(q, it, N, &pub) == FQ_OK && pub == N, "fq_publish_batch 40건");
        CHECK(fq_publish(q, "last", 4, NULL) == FQ_OK, "배치 뒤에 하나 발행");
        CHECK(count_dir(q, FQ_DIR_TMP) == 0, "tmp/에 잔재 없음");
        int ok = 1, i = -1;
        while (fq_consume(q, &m) == FQ_OK) {
            char want[16];
            if (i == -1) snprintf(want, sizeof(want), "first");
            else if (i == N) snprintf(want, sizeof(want), "last");
            else snprintf(want, sizeof(want), "b%02d", i);
            if (m->len != strlen(want) || memcmp(m->data, want, m->len) != 0) ok = 0;
            fq_ack(q, m);
            i++;
        }
        CHECK(ok && i == N + 1, "꺼낸 순서 = first, 항목 순서, last");
    }

    printf("[2] 빈 배치, 잘못된 인자\n");
    {
        size_t pub = 99;
        CHECK(fq_publish_batch(q, NULL, 0, &pub) == FQ_OK && pub == 0, "n = 0: FQ_OK, 0건");
        CHECK(fq_publish_batch(q, NULL, 3, &pub) == FQ_EINVAL, "items NULL: FQ_EINVAL");
        fq_item it[3] = { { "a", 1, NULL }, { "b", 1, "ok-key" }, { "c", 1, "bad key" } };
        pub = 99;
        CHECK(fq_publish_batch(q, it, 3, &pub) == FQ_EINVAL && pub == 0, "셋째 항목 stream_key가 잘못됨: FQ_EINVAL");
        CHECK(count_dir(q, FQ_DIR_INCOMING) == 0 && count_dir(q, FQ_DIR_TMP) == 0, "  ... 아무것도 쓰지 않음");
        char longkey[128];
        memset(longkey, 'k', 127);
        longkey[127] = '\0';
        fq_queue *ql = NULL;
        char nid[64];
        memset(nid, 'n', 63);
        nid[63] = '\0';
        if (fq_open(root, nid, &ql) == FQ_OK) {
            fq_item lit[2] = { { "a", 1, NULL }, { "b", 1, longkey } };   /* 이름이 claim 한도를 넘음 */
            CHECK(fq_publish_batch(ql, lit, 2, &pub) == FQ_EINVAL && pub == 0, "둘째 항목 이름이 너무 김: FQ_EINVAL");
            CHECK(count_dir(q, FQ_DIR_INCOMING) == 0 && count_dir(q, FQ_DIR_TMP) == 0, "  ... 첫째도 쓰지 않음");
            fq_close(ql);
        }
        fq_item z[2] = { { NULL, 0, NULL }, { "", 0, NULL } };
        CHECK(fq_publish_batch(q, z, 2, NULL) == FQ_OK, "빈 페이로드 2건, published NULL");
        int got = 0;
        while (fq_consume(q, &m) == FQ_OK) { if (m->len == 0) got++; fq_ack(q, m); }
        CHECK(got == 2, "  ... 둘 다 빈 메시지로 전달");
    }

    printf("[3] 기록이 실패하면 큐는 그대로 (하나도 발행되지 않음)\n");
    {
        /* tmp/를 치워 기록을 실패시킨다 */
        char tdir[1280], moved[1300];
        fq_path(tdir, sizeof(tdir), q->root, FQ_DIR_TMP, NULL);
        snprintf(moved, sizeof(moved), "%s.away", tdir);
        CHECK(fq_fs_rename(tdir, moved) == FQ_OK, "tmp/ 치움");
        fq_item it[5] = { { "x1", 2, NULL }, { "x2", 2, NULL }, { "x3", 2, NULL }, { "x4", 2, NULL }, { "x5", 2, NULL } };
        size_t pub = 99;
        int rc = fq_publish_batch(q, it, 5, &pub);
        CHECK(rc != FQ_OK && pub == 0, "기록 실패: 오류, 0건");
        CHECK(count_dir(q, FQ_DIR_INCOMING) == 0, "  ... incoming/ 그대로");
        CHECK(fq_fs_rename(moved, tdir) == FQ_OK, "tmp/ 되돌림");
        CHECK(fq_publish_batch(q, it, 5, &pub) == FQ_OK && pub == 5, "다시 하면 5건");
        int got = 0;
        while (fq_consume(q, &m) == FQ_OK) { got++; fq_ack(q, m); }
        CHECK(got == 5, "  ... 5건 전달");
    }

    printf("[4] 큰 배치 (스레드 수보다 많이, 순서 유지)\n");
    {
        enum { N = 300 };
        static char pay[N][12];
        static fq_item it[N];
        for (int i = 0; i < N; i++) {
            snprintf(pay[i], sizeof(pay[i]), "%03d", i);
            it[i].data = pay[i];
            it[i].len = 3;
            it[i].stream_key = NULL;
        }
        size_t pub = 0;
        CHECK(fq_publish_batch(q, it, N, &pub) == FQ_OK && pub == N, "300건");
        int ok = 1, i = 0;
        while (fq_consume(q, &m) == FQ_OK) {
            char want[12];
            snprintf(want, sizeof(want), "%03d", i);
            if (m->len != 3 || memcmp(m->data, want, 3) != 0) ok = 0;
            fq_ack(q, m);
            i++;
        }
        CHECK(ok && i == N, "  ... 순서대로 300건");
        CHECK(count_dir(q, FQ_DIR_TMP) == 0, "  ... tmp/에 잔재 없음");
    }

    printf("[5] 일부 기록만 실패해도 하나도 발행하지 않고, 성공한 tmp도 지운다\n");
    {
        /* 결함 주입: data NULL + len > 0 이면 그 항목의 기록만 실패한다(write의 잘못된 주소) */
        fq_item it[6] = { { "p1", 2, NULL }, { "p2", 2, NULL }, { "p3", 2, NULL },
                          { NULL, 16, NULL }, { "p5", 2, NULL }, { "p6", 2, NULL } };
        size_t pub = 99;
        int rc = fq_publish_batch(q, it, 6, &pub);
        CHECK(rc != FQ_OK && pub == 0, "넷째만 실패: 오류, 0건");
        CHECK(count_dir(q, FQ_DIR_INCOMING) == 0, "  ... 성공한 다섯도 발행되지 않음");
        CHECK(count_dir(q, FQ_DIR_TMP) == 0, "  ... tmp/에 잔재 없음 (성공한 것의 tmp도 지움)");
    }

    fq_close(q);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
