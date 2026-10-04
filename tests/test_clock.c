/* test_clock.c - 메시지명 시각 보정(FS 시각 오프셋 + 단조) 테스트. DESIGN.md §4-4 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

/* incoming 나열: 직전 나열에 없던 이름(=방금 발행한 것)의 앞쪽 시각을 찾는다 */
static char     seen[64][512];
static int      seen_n;
static uint64_t new_ms;
static char     new_name[512];

static int mark_cb(const char *name, void *ud)
{
    (void)ud;
    if (seen_n < 64) snprintf(seen[seen_n++], 512, "%s", name);
    return 0;
}
static int find_new_cb(const char *name, void *ud)
{
    (void)ud;
    for (int i = 0; i < seen_n; i++)
        if (strcmp(seen[i], name) == 0) return 0;
    new_ms = strtoull(name, NULL, 10);
    snprintf(new_name, sizeof(new_name), "%s", name);
    return 0;
}

/* 이전 실행 잔여물 제거: incoming의 파일을 모두 지운다 (seen 배열 상한 64 안에 머물도록) */
static int unlink_cb(const char *name, void *ud)
{
    char p[1280];
    fq_path(p, sizeof(p), (const char *)ud, FQ_DIR_INCOMING, name);
    fq_fs_unlink(p);
    return 0;
}

/* 발행 후 incoming에 새로 생긴 이름의 시각을 돌려준다 */
static uint64_t publish_and_read_ms(fq_queue *q, const char *inc)
{
    seen_n = 0; new_ms = 0;
    fq_fs_list(inc, mark_cb, NULL);
    if (fq_publish(q, "x", 1, NULL) != FQ_OK) return 0;
    fq_fs_list(inc, find_new_cb, NULL);
    return new_ms;
}

static uint64_t absdiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "_clock_test";
    printf("== 메시지명 시각 보정 (%s) ==\n", root);

    fq_queue *q = NULL;
    CHECK(fq_open(root, "nodeA", &q) == FQ_OK && q, "fq_open");
    if (!q) return 1;

    char inc[1280], tmp[1280];
    fq_path(inc, sizeof(inc), root, FQ_DIR_INCOMING, NULL);
    fq_path(tmp, sizeof(tmp), root, FQ_DIR_TMP, NULL);
    fq_fs_list(inc, unlink_cb, (void *)root);

    /* [1] 첫 발행에서 FS 시각으로 동기화: 이름의 시각 ≈ FS 시각 */
    printf("[1] FS 시각 동기화\n");
    uint64_t t1 = publish_and_read_ms(q, inc);
    uint64_t fs_now = 0;
    CHECK(fq_fs_now_ms(tmp, "test", &fs_now) == FQ_OK, "fq_fs_now_ms");
    CHECK(t1 != 0 && absdiff(t1, fs_now) < 5000, "이름 시각이 FS 시각과 5초 이내");
    CHECK(q->clock_next_sync_ms != 0, "재동기 시각 설정됨");

    /* [2] 오프셋 적용: 로컬 벽시계가 FS보다 1시간 뒤처진 노드를 흉내 → 이름은 FS 시각 기준 */
    printf("[2] 오프셋 적용\n");
    const int64_t skew = 3600 * 1000;
    q->clock_offset_ms = skew;                        /* FS - wall = +1h */
    q->clock_next_sync_ms = fq_now_wall_ms() + FQ_CLOCK_SYNC_MS; /* 재동기 억제 */
    uint64_t t2 = publish_and_read_ms(q, inc);
    CHECK(absdiff(t2, fq_now_wall_ms() + (uint64_t)skew) < 5000, "이름 시각 = 벽시계 + 오프셋");
    char n2[512]; snprintf(n2, sizeof(n2), "%s", new_name);

    /* [3] 단조 보정: 로컬 시계가 뒤로 가도(오프셋을 확 줄여 흉내) 이름 시각은 역행하지 않는다.
     *     직전 값에 머물고(+1ms씩 앞서 나가지 않음), 같은 ms 안의 순서는 seq가 이름 정렬로 보존한다. */
    printf("[3] 단조 보정\n");
    q->clock_offset_ms = -skew;                       /* 2시간 뒤로 점프한 셈 */
    uint64_t t3 = publish_and_read_ms(q, inc);
    char n3[512]; snprintf(n3, sizeof(n3), "%s", new_name);
    CHECK(t3 == t2, "역행 대신 직전 값에 머묾");
    uint64_t t4 = publish_and_read_ms(q, inc);
    CHECK(t4 == t2, "같은 ms가 쌓여도 시각이 앞서 나가지 않음");
    CHECK(strcmp(n2, n3) < 0 && strcmp(n3, new_name) < 0, "같은 ms 안의 이름 정렬 = 발행 순서(seq)");

    /* [4] 재동기: 동기 시각이 지나면 FS 시각으로 다시 맞춘다 (가짜 오프셋이 사라져야 함) */
    printf("[4] 재동기\n");
    q->clock_next_sync_ms = 1;                        /* 즉시 재동기 */
    q->last_msg_ms = 0;                               /* 단조 보정이 가리지 않도록 */
    uint64_t t5 = publish_and_read_ms(q, inc);
    CHECK(fq_fs_now_ms(tmp, "test", &fs_now) == FQ_OK && absdiff(t5, fs_now) < 5000,
          "재동기 후 이름 시각 ≈ FS 시각");
    CHECK((q->clock_offset_ms < 0 ? -q->clock_offset_ms : q->clock_offset_ms) < 5000,
          "오프셋이 실제 값(≈0)으로 복원");

    fq_close(q);
    printf("== %s (%d fail) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
