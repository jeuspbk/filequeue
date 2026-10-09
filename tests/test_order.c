/* test_order.c - 순서 지정 발행: fq_publish_ex / fq_adopt_ex (HEAD, BEFORE) */
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

static int want_str(const void *data, size_t len, void *ud)
{
    const char *s = ud;
    return len == strlen(s) && memcmp(data, s, len) == 0;
}

static int pub(fq_queue *q, const char *s, int mode, const char *before)
{
    fq_order o = { mode, want_str, (void *)before };
    return fq_publish_ex(q, s, strlen(s), NULL, &o);
}

/* 큐를 비우며 꺼낸 순서를 "a b c" 꼴로 out에 */
static void drain(fq_queue *q, char *out, size_t n)
{
    out[0] = '\0';
    fq_msg *m = NULL;
    while (fq_consume(q, &m) == FQ_OK) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%.*s", l ? " " : "", (int)m->len, (const char *)m->data);
        fq_ack(q, m);
    }
}

static int any_cb(const void *data, size_t len, void *ud)
{
    (void)data; (void)len; (void)ud;
    return 1;
}

/* fq_claim_if로 비우기 (매번 나열) */
static void drain_if(fq_queue *q, char *out, size_t n)
{
    out[0] = '\0';
    fq_msg *m = NULL;
    while (fq_consume_if(q, any_cb, NULL, &m) == FQ_OK) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%.*s", l ? " " : "", (int)m->len, (const char *)m->data);
        fq_ack(q, m);
    }
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "_order_test";
    printf("== filequeue 순서 지정 발행 (%s) ==\n", root);
    char qroot[512], stage[512], f1[600];
    snprintf(qroot, sizeof(qroot), "%s/q", root);
    snprintf(stage, sizeof(stage), "%s/stage", root);
    snprintf(f1, sizeof(f1), "%s/e1", stage);

    fq_queue *q = NULL;
    CHECK(fq_open(qroot, "nodeO", &q) == FQ_OK && q, "fq_open");
    if (!q) return 1;
    char got[512];
    drain(q, got, sizeof(got));                           /* 이전 실행의 잔여 */

    printf("[1] HEAD: 꼬리보다 앞, 맨 앞끼리는 나중 것이 앞(LIFO)\n");
    CHECK(pub(q, "t1", FQ_ORDER_TAIL, NULL) == FQ_OK, "t1 꼬리");
    CHECK(pub(q, "h1", FQ_ORDER_HEAD, NULL) == FQ_OK, "h1 맨 앞");
    CHECK(fq_publish(q, "t2", 2, NULL) == FQ_OK, "t2 꼬리 (fq_publish)");
    CHECK(pub(q, "h2", FQ_ORDER_HEAD, NULL) == FQ_OK, "h2 맨 앞");
    drain(q, got, sizeof(got));
    CHECK(strcmp(got, "h2 h1 t1 t2") == 0, got);

    printf("[2] BEFORE: 대상 바로 앞, 같은 대상 앞에는 넣은 순서대로\n");
    pub(q, "a", FQ_ORDER_TAIL, NULL);
    pub(q, "b", FQ_ORDER_TAIL, NULL);
    pub(q, "c", FQ_ORDER_TAIL, NULL);
    CHECK(pub(q, "x1", FQ_ORDER_BEFORE, "b") == FQ_OK, "x1 before b");
    CHECK(pub(q, "x2", FQ_ORDER_BEFORE, "b") == FQ_OK, "x2 before b");
    CHECK(pub(q, "y", FQ_ORDER_BEFORE, "x1") == FQ_OK, "y before x1 (끼운 것 앞에 다시)");
    CHECK(pub(q, "z", FQ_ORDER_BEFORE, "a") == FQ_OK, "z before a (맨 앞 메시지 앞)");
    CHECK(pub(q, "nope", FQ_ORDER_BEFORE, "missing") == FQ_EEMPTY, "대상 없음: FQ_EEMPTY");
    drain_if(q, got, sizeof(got));
    CHECK(strcmp(got, "z a y x1 x2 b c") == 0, got);

    printf("[3] BEFORE: HEAD 메시지 앞, attempt 접미사가 붙은 대상\n");
    pub(q, "t", FQ_ORDER_TAIL, NULL);
    pub(q, "h", FQ_ORDER_HEAD, NULL);
    CHECK(pub(q, "bh", FQ_ORDER_BEFORE, "h") == FQ_OK, "bh before h");
    fq_msg *m = NULL;
    CHECK(fq_consume(q, &m) == FQ_OK && m && m->len == 2 && memcmp(m->data, "bh", 2) == 0, "bh가 먼저");
    if (m) fq_ack(q, m);
    CHECK(fq_consume(q, &m) == FQ_OK && m && m->len == 1 && memcmp(m->data, "h", 1) == 0, "h");
    CHECK(m && fq_nack(q, m) == FQ_OK, "h nack: .a1 이 붙어 같은 자리로");
    CHECK(pub(q, "bh2", FQ_ORDER_BEFORE, "h") == FQ_OK, "bh2 before h.a1");
    drain(q, got, sizeof(got));
    CHECK(strcmp(got, "bh2 h t") == 0, got);

    printf("[4] stream_key가 있는 대상과 메시지\n");
    CHECK(fq_publish(q, "s1", 2, "strm") == FQ_OK, "s1 (stream)");
    {
        fq_order o = { FQ_ORDER_BEFORE, want_str, (void *)"s1" };
        CHECK(fq_publish_ex(q, "s0", 2, "strm", &o) == FQ_OK, "s0 before s1 (stream)");
        fq_order h = { FQ_ORDER_HEAD, NULL, NULL };
        CHECK(fq_publish_ex(q, "sh", 2, "strm", &h) == FQ_OK, "sh head (stream)");
        fq_order bad = { FQ_ORDER_HEAD, NULL, NULL };
        CHECK(fq_publish_ex(q, "x", 1, "bad key", &bad) == FQ_EINVAL, "잘못된 stream_key: FQ_EINVAL");
        fq_order nomode = { 7, NULL, NULL };
        CHECK(fq_publish_ex(q, "x", 1, NULL, &nomode) == FQ_EINVAL, "모르는 mode: FQ_EINVAL");
        fq_order nofn = { FQ_ORDER_BEFORE, NULL, NULL };
        CHECK(fq_publish_ex(q, "x", 1, NULL, &nofn) == FQ_EINVAL, "BEFORE에 고르기 함수 없음: FQ_EINVAL");
    }
    drain(q, got, sizeof(got));
    CHECK(strcmp(got, "sh s0 s1") == 0, got);

    printf("[5] fq_adopt_ex\n");
    CHECK(fq_fs_mkdirs(stage) == FQ_OK, "보관 디렉터리");
    pub(q, "p", FQ_ORDER_TAIL, NULL);
    pub(q, "r", FQ_ORDER_TAIL, NULL);
    CHECK(fq_fs_write_sync(f1, "ad", 2) == FQ_OK, "보관 파일");
    {
        fq_order o = { FQ_ORDER_BEFORE, want_str, (void *)"missing" };
        CHECK(fq_adopt_ex(q, f1, NULL, &o) == FQ_EEMPTY, "대상 없음: FQ_EEMPTY");
        CHECK(fq_fs_exists(f1) == 1, "  ... 보관 파일은 그대로");
        o.ud = (void *)"r";
        CHECK(fq_adopt_ex(q, f1, NULL, &o) == FQ_OK, "adopt before r");
        CHECK(fq_fs_exists(f1) == 0, "  ... 옮겨짐");
        CHECK(fq_adopt_ex(q, f1, NULL, &o) == FQ_ENOENT, "다시: FQ_ENOENT (이미 옮김)");
        CHECK(fq_fs_write_sync(f1, "ah", 2) == FQ_OK, "보관 파일 2");
        fq_order h = { FQ_ORDER_HEAD, NULL, NULL };
        CHECK(fq_adopt_ex(q, f1, NULL, &h) == FQ_OK, "adopt head");
    }
    drain(q, got, sizeof(got));
    CHECK(strcmp(got, "ah p ad r") == 0, got);

    printf("[6] 이름 한도: 같은 자리에 겹겹이 끼우면 결국 FQ_EINVAL\n");
    pub(q, "k0", FQ_ORDER_TAIL, NULL);
    char prev[16] = "k0", cur[16];
    int depth = 0, rc = FQ_OK;
    while (depth < 50) {
        snprintf(cur, sizeof(cur), "k%d", depth + 1);
        rc = pub(q, cur, FQ_ORDER_BEFORE, prev);
        if (rc != FQ_OK) break;
        snprintf(prev, sizeof(prev), "%s", cur);
        depth++;
    }
    printf("       (깊이 %d에서 멈춤)\n", depth);
    CHECK(rc == FQ_EINVAL && depth >= 1, "한도를 넘으면 FQ_EINVAL");
    drain(q, got, sizeof(got));
    {
        /* 꺼낸 순서는 k<depth> ... k1 k0 */
        char want[512] = "";
        for (int i = depth; i >= 0; --i) {
            size_t l = strlen(want);
            snprintf(want + l, sizeof(want) - l, "%sk%d", l ? " " : "", i);
        }
        CHECK(strcmp(got, want) == 0, got);
    }

    printf("[7] 나열 캐시가 차 있을 때 같은 핸들이 맨 앞에 넣으면 바로 다음에 나온다\n");
    {
        pub(q, "c1", FQ_ORDER_TAIL, NULL);
        pub(q, "c2", FQ_ORDER_TAIL, NULL);
        pub(q, "c3", FQ_ORDER_TAIL, NULL);
        fq_msg *m = NULL;
        CHECK(fq_consume(q, &m) == FQ_OK && m && m->len == 2 && memcmp(m->data, "c1", 2) == 0,
              "c1 (fq_claim의 캐시에 c2 c3가 남음)");
        if (m) fq_ack(q, m);
        CHECK(pub(q, "hx", FQ_ORDER_HEAD, NULL) == FQ_OK, "hx 맨 앞");
        drain(q, got, sizeof(got));
        CHECK(strcmp(got, "hx c2 c3") == 0, got);
    }

    fq_close(q);
    printf(fails ? "== FAIL (%d) ==\n" : "== OK ==\n", fails);
    return fails ? 1 : 0;
}
