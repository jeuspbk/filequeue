/* test_batch.c - fq_publish_batch: 순서, 검증 실패 시 아무것도 안 씀, 기록 실패 시 큐 그대로, 잔재 없음,
 *                청크 경계(오류면 앞쪽 *published건만), rename 실패, 스레드 없이
 *                fq_consume_batch / fq_ack_batch: 순서, 상한, 읽기 실패 건너뜀, 들고 있는 것 보호, ack 오류 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"   /* fq_path */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <direct.h>
#  define rmdir _rmdir
#else
#  include <unistd.h>
#endif

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

    printf("[6] 둘째 청크에서 기록이 실패하면 첫 청크만 발행 (앞쪽 *published건, 그 뒤는 없음)\n");
    {
        enum { N = FQ_BATCH_CHUNK * 2 + 10, BAD = FQ_BATCH_CHUNK + 3 };
        static char pay[N][12];
        static fq_item it[N];
        for (int i = 0; i < N; i++) {
            snprintf(pay[i], sizeof(pay[i]), "%04d", i);
            it[i].data = pay[i];
            it[i].len = 4;
            it[i].stream_key = NULL;
        }
        it[BAD].data = NULL;
        it[BAD].len = 16;                                 /* 결함 주입: 이 항목의 기록만 실패 */
        size_t pub = 99;
        int rc = fq_publish_batch(q, it, N, &pub);
        CHECK(rc != FQ_OK && pub == FQ_BATCH_CHUNK, "오류, *published = 첫 청크");
        CHECK(count_dir(q, FQ_DIR_INCOMING) == FQ_BATCH_CHUNK, "  ... incoming/에 첫 청크만");
        CHECK(count_dir(q, FQ_DIR_TMP) == 0, "  ... tmp/에 잔재 없음");
        int ok = 1, i = 0;
        while (fq_consume(q, &m) == FQ_OK) {
            char want[12];
            snprintf(want, sizeof(want), "%04d", i);
            if (m->len != 4 || memcmp(m->data, want, 4) != 0) ok = 0;
            fq_ack(q, m);
            i++;
        }
        CHECK(ok && i == FQ_BATCH_CHUNK, "  ... 앞에서부터 순서대로");
    }

    printf("[7] 마지막 청크의 항목이 검사에 어긋나도 아무것도 쓰지 않음\n");
    {
        char longkey[128];
        memset(longkey, 'k', 127);
        longkey[127] = '\0';
        char nid[64];
        memset(nid, 'n', 63);
        nid[63] = '\0';
        fq_queue *ql = NULL;
        if (fq_open(root, nid, &ql) == FQ_OK) {
            enum { N = FQ_BATCH_CHUNK + 10 };
            static fq_item it[N];
            for (int i = 0; i < N; i++) { it[i].data = "v"; it[i].len = 1; it[i].stream_key = "k1"; }
            it[N - 1].stream_key = longkey;               /* 이 node_id로는 이름이 claim 한도를 넘음 */
            size_t pub = 99;
            CHECK(fq_publish_batch(ql, it, N, &pub) == FQ_EINVAL && pub == 0, "마지막 항목 이름이 너무 김: FQ_EINVAL, 0건");
            CHECK(count_dir(q, FQ_DIR_INCOMING) == 0 && count_dir(q, FQ_DIR_TMP) == 0, "  ... 첫 청크도 쓰지 않음");
            it[N - 1].stream_key = "bad key";
            CHECK(fq_publish_batch(ql, it, N, &pub) == FQ_EINVAL && pub == 0, "마지막 항목 stream_key가 잘못됨: FQ_EINVAL, 0건");
            CHECK(count_dir(q, FQ_DIR_INCOMING) == 0 && count_dir(q, FQ_DIR_TMP) == 0, "  ... 아무것도 쓰지 않음");
            fq_close(ql);
        } else {
            CHECK(0, "긴 node_id로 fq_open");
        }
    }

    printf("[8] rename이 실패하면 남은 tmp를 지운다\n");
    {
        char idir[1280], moved[1300];
        fq_path(idir, sizeof(idir), q->root, FQ_DIR_INCOMING, NULL);
        snprintf(moved, sizeof(moved), "%s.away", idir);
        CHECK(fq_fs_rename(idir, moved) == FQ_OK, "incoming/ 치움");
        fq_item it[5] = { { "r1", 2, NULL }, { "r2", 2, NULL }, { "r3", 2, NULL }, { "r4", 2, NULL }, { "r5", 2, NULL } };
        size_t pub = 99;
        int rc = fq_publish_batch(q, it, 5, &pub);
        CHECK(rc != FQ_OK && pub == 0, "rename 실패: 오류, 0건");
        CHECK(count_dir(q, FQ_DIR_TMP) == 0, "  ... tmp/에 잔재 없음");
        CHECK(fq_fs_rename(moved, idir) == FQ_OK, "incoming/ 되돌림");
        CHECK(count_dir(q, FQ_DIR_INCOMING) == 0, "  ... incoming/ 그대로");
    }

    printf("[9] 스레드를 만들 수 없어도 호출 스레드가 모두 기록\n");
    {
        enum { N = 50 };
        static char pay[N][12];
        static fq_item it[N];
        for (int i = 0; i < N; i++) {
            snprintf(pay[i], sizeof(pay[i]), "%02d", i);
            it[i].data = pay[i];
            it[i].len = 2;
            it[i].stream_key = NULL;
        }
        fq_test_par_nothreads = 1;
        size_t pub = 0;
        int rc = fq_publish_batch(q, it, N, &pub);
        fq_test_par_nothreads = 0;
        CHECK(rc == FQ_OK && pub == N, "50건");
        int ok = 1, i = 0;
        while (fq_consume(q, &m) == FQ_OK) {
            char want[12];
            snprintf(want, sizeof(want), "%02d", i);
            if (m->len != 2 || memcmp(m->data, want, 2) != 0) ok = 0;
            fq_ack(q, m);
            i++;
        }
        CHECK(ok && i == N, "  ... 순서대로 50건");
        CHECK(count_dir(q, FQ_DIR_TMP) == 0, "  ... tmp/에 잔재 없음");
    }

    printf("[10] fq_consume_batch / fq_ack_batch: 순서, max씩, 들고 있는 것은 하트비트가 회수 안 함\n");
    {
        enum { N = 30 };
        static char pay[N][12];
        static fq_item it[N];
        for (int i = 0; i < N; i++) {
            snprintf(pay[i], sizeof(pay[i]), "c%02d", i);
            it[i].data = pay[i];
            it[i].len = 3;
            it[i].stream_key = NULL;
        }
        size_t pub = 0;
        CHECK(fq_publish_batch(q, it, N, &pub) == FQ_OK && pub == N, "30건 발행");
        fq_msg *out[8];
        size_t got = 99, total = 0;
        int ok = 1, sizes_ok = 1, ack_ok = 1, rc;
        while ((rc = fq_consume_batch(q, out, 8, &got)) == FQ_OK) {
            if (got != (total + 8 <= N ? 8u : (size_t)(N - total))) sizes_ok = 0;
            for (size_t j = 0; j < got; j++) {
                char want[12];
                snprintf(want, sizeof(want), "c%02d", (int)(total + j));
                if (out[j]->len != 3 || memcmp(out[j]->data, want, 3) != 0) ok = 0;
            }
            if (total == 0) {
                /* 들고 있는 동안 하트비트(→ fq_recover_stale)가 돌아도 다시 나오지 않아야 한다 */
                q->lead_next_renew_ms = 0;
                fq_msg *more[8];
                size_t g2 = 0;
                CHECK(fq_consume_batch(q, more, 8, &g2) == FQ_OK && g2 == 8, "  하트비트 뒤 다음 8건");
                int dup = 0;
                for (size_t j = 0; j < g2; j++) {
                    char want[12];
                    snprintf(want, sizeof(want), "c%02d", (int)(8 + j));
                    if (more[j]->len != 3 || memcmp(more[j]->data, want, 3) != 0) dup = 1;
                }
                CHECK(!dup, "  ... 들고 있는 첫 8건은 회수·재전달되지 않음");
                CHECK(fq_ack_batch(q, more, g2) == FQ_OK, "  ... 그 8건 fq_ack_batch");
                total += g2;
            }
            if (fq_ack_batch(q, out, got) != FQ_OK) ack_ok = 0;
            total += got;
        }
        CHECK(ack_ok, "fq_ack_batch 모두 FQ_OK");
        CHECK(rc == FQ_EEMPTY && got == 0, "끝나면 FQ_EEMPTY, *got = 0");
        CHECK(ok && sizes_ok && total == N, "  ... 30건 순서대로, 8건씩(마지막은 나머지)");
        CHECK(count_dir(q, FQ_DIR_INFLIGHT) == 0, "  ... inflight/ 비어 있음 (모두 ack)");
        CHECK(fq_consume_batch(q, NULL, 8, &got) == FQ_EINVAL && fq_consume_batch(q, out, 0, &got) == FQ_EINVAL &&
              fq_consume_batch(q, out, 8, NULL) == FQ_EINVAL, "out NULL·max 0·got NULL: FQ_EINVAL");
        CHECK(fq_ack_batch(q, out, 0) == FQ_OK, "fq_ack_batch 0건: FQ_OK");
    }

    printf("[11] 한 번에 FQ_BATCH_CHUNK건까지\n");
    {
        enum { N = FQ_BATCH_CHUNK + 5 };
        static fq_item it[N];
        static fq_msg *out[N + 100];
        for (int i = 0; i < N; i++) { it[i].data = "w"; it[i].len = 1; it[i].stream_key = NULL; }
        size_t pub = 0, got = 0;
        CHECK(fq_publish_batch(q, it, N, &pub) == FQ_OK && pub == N, "CHUNK+5건 발행");
        CHECK(fq_consume_batch(q, out, N + 100, &got) == FQ_OK && got == FQ_BATCH_CHUNK, "max가 커도 CHUNK건");
        CHECK(fq_ack_batch(q, out, got) == FQ_OK, "  ... ack");
        CHECK(fq_consume_batch(q, out, N + 100, &got) == FQ_OK && got == 5, "  ... 나머지 5건");
        CHECK(fq_ack_batch(q, out, got) == FQ_OK, "  ... ack");
    }

    printf("[12] 읽을 수 없는 항목은 시도로 쳐서 건너뛰고(결국 dead/) 나머지를 준다\n");
    {
        char bad[1408], dead_bad[1408];
        const char *bn = "0000000000000001-0000000001-x-bad.msg";      /* 가장 오래된 이름: 묶음의 맨 앞 */
        fq_path(bad, sizeof(bad), q->root, FQ_DIR_INCOMING, bn);
        fq_fs_mkdirs(bad);
        fq_item it[4] = { { "g1", 2, NULL }, { "g2", 2, NULL }, { "g3", 2, NULL }, { "g4", 2, NULL } };
        size_t pub = 0;
        CHECK(fq_publish_batch(q, it, 4, &pub) == FQ_OK && pub == 4, "4건 발행 + incoming에 디렉터리");
        fq_msg *out[8];
        size_t got = 0;
        CHECK(fq_consume_batch(q, out, 8, &got) == FQ_OK && got == 4, "정상 4건만 받음");
        int ok = got == 4;
        for (size_t j = 0; ok && j < got; j++)
            if (out[j]->len != 2 || out[j]->data == NULL || ((char *)out[j]->data)[1] != (char)('1' + j)) ok = 0;
        CHECK(ok, "  ... 순서대로 g1~g4 (빈 메시지 없음)");
        fq_ack_batch(q, out, got);
        int rc = fq_consume_batch(q, out, 8, &got);
        CHECK(rc == FQ_EEMPTY && got == 0, "다음 호출: 디렉터리는 시도가 쌓여 dead/로 가고 FQ_EEMPTY");
        snprintf(dead_bad, sizeof(dead_bad), "%s/%s/%s.a%d", q->root, FQ_DIR_DEAD, bn, FQ_MAX_ATTEMPTS - 1);
        CHECK(fq_fs_exists(dead_bad) == 1, "  ... dead/에 격리");
        rmdir(dead_bad);
    }

    printf("[13] fq_ack_batch: 이미 회수된 것이 있으면 FQ_ENOENT, 나머지는 지움\n");
    {
        fq_item it[4] = { { "h1", 2, NULL }, { "h2", 2, NULL }, { "h3", 2, NULL }, { "h4", 2, NULL } };
        size_t pub = 0, got = 0;
        CHECK(fq_publish_batch(q, it, 4, &pub) == FQ_OK && pub == 4, "4건 발행");
        fq_msg *out[8];
        CHECK(fq_consume_batch(q, out, 8, &got) == FQ_OK && got == 4, "4건 받음");
        char p[1408];
        fq_path(p, sizeof(p), q->root, FQ_DIR_INFLIGHT, out[2]->name);
        CHECK(fq_fs_unlink(p) == FQ_OK, "셋째의 inflight를 치움(회수당한 것처럼)");
        CHECK(fq_ack_batch(q, out, got) == FQ_ENOENT, "fq_ack_batch: FQ_ENOENT");
        CHECK(count_dir(q, FQ_DIR_INFLIGHT) == 0, "  ... 나머지 셋은 지워짐");
        CHECK(fq_consume_batch(q, out, 8, &got) == FQ_EEMPTY, "  ... 다시 나오는 것 없음");
    }

    printf("[14] 스레드를 만들 수 없어도 묶음 소비·ack\n");
    {
        enum { N = 20 };
        static char pay[N][12];
        static fq_item it[N];
        for (int i = 0; i < N; i++) {
            snprintf(pay[i], sizeof(pay[i]), "t%02d", i);
            it[i].data = pay[i];
            it[i].len = 3;
            it[i].stream_key = NULL;
        }
        size_t pub = 0, got = 0;
        CHECK(fq_publish_batch(q, it, N, &pub) == FQ_OK && pub == N, "20건 발행");
        fq_test_par_nothreads = 1;
        fq_msg *out[N];
        int rc = fq_consume_batch(q, out, N, &got);
        int ok = rc == FQ_OK && got == N;
        for (size_t j = 0; ok && j < got; j++) {
            char want[12];
            snprintf(want, sizeof(want), "t%02d", (int)j);
            if (out[j]->len != 3 || memcmp(out[j]->data, want, 3) != 0) ok = 0;
        }
        CHECK(ok, "20건 순서대로 (읽기를 호출 스레드가)");
        CHECK(rc == FQ_OK && fq_ack_batch(q, out, got) == FQ_OK, "  ... fq_ack_batch");
        fq_test_par_nothreads = 0;
        CHECK(count_dir(q, FQ_DIR_INFLIGHT) == 0 && count_dir(q, FQ_DIR_INCOMING) == 0, "  ... 큐 비어 있음");
    }

    fq_close(q);
    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
