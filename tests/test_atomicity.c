/* test_atomicity.c - DESIGN.md §7-1: 목표 스토리지에서 핵심 원자성 검증
 *
 * 큐 설계는 아래 3가지 FS 원자성에 의존한다. 실제 공유 스토리지에서 이 하니스를
 * 돌려 전제가 성립하는지 먼저 확인해야 한다.
 *   1) atomic rename (교체형 fq_fs_rename / 대상 존재 시 실패하는 noreplace)
 *   2) atomic exclusive create (CREATE_NEW / O_EXCL)
 *   3) advisory lock (선택적)
 *
 * 사용:  test_atomicity [작업디렉터리]
 * 종료코드 0 = 모든 검사 통과.
 */
#include "fq_fs.h"
#include "fq.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

static int count_cb(const char *name, void *ud) { (void)name; (*(int *)ud)++; return 0; }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "_atom_test";
    char p[1024], a[1024], b[1024];

    printf("== filequeue 원자성 검증 (%s) ==\n", dir);
    fq_fs_mkdirs(dir);

    /* 1) 원자적 rename: src 작성 → dst로 rename → dst 존재/내용 확인 */
    printf("[1] 원자적 rename\n");
    snprintf(a, sizeof(a), "%s/r_src", dir);
    snprintf(b, sizeof(b), "%s/r_dst", dir);
    fq_fs_unlink(a); fq_fs_unlink(b);
    CHECK(fq_fs_write_sync(a, "hello", 5) == FQ_OK, "src 작성");
    CHECK(fq_fs_rename_noreplace(a, b) == FQ_OK, "rename src->dst 성공");
    CHECK(fq_fs_exists(b) == 1, "dst 존재");
    CHECK(fq_fs_exists(a) == 0, "src 사라짐");
    {
        void *buf = NULL; size_t len = 0;
        CHECK(fq_fs_read_file(b, &buf, &len) == FQ_OK && len == 5 &&
              memcmp(buf, "hello", 5) == 0, "내용 보존");
        free(buf);
    }
    /* 대상 존재 시 rename 은 EEXIST (덮어쓰지 않음) */
    CHECK(fq_fs_write_sync(a, "x", 1) == FQ_OK, "src 재작성");
    CHECK(fq_fs_rename_noreplace(a, b) == FQ_EEXIST, "기존 대상으로 rename 거부(EEXIST)");
    /* fq_fs_rename: 대상을 원자적으로 교체, src는 사라짐, 없는 src는 ENOENT */
    CHECK(fq_fs_rename(a, b) == FQ_OK, "fq_fs_rename: 기존 대상 교체 성공");
    CHECK(fq_fs_exists(a) == 0, "fq_fs_rename: src 사라짐");
    {
        void *buf = NULL; size_t len = 0;
        CHECK(fq_fs_read_file(b, &buf, &len) == FQ_OK && len == 1 && ((char *)buf)[0] == 'x',
              "fq_fs_rename: 교체된 내용");
        free(buf);
    }
    CHECK(fq_fs_rename(a, b) == FQ_ENOENT, "fq_fs_rename: 없는 src는 ENOENT (경합 패배 표현)");
    fq_fs_unlink(a); fq_fs_unlink(b);

    /* 2) 원자적 배타 생성 */
    printf("[2] 원자적 배타 생성 (CREATE_NEW / O_EXCL)\n");
    snprintf(p, sizeof(p), "%s/excl", dir);
    fq_fs_unlink(p);
    CHECK(fq_fs_create_new(p) == FQ_OK, "최초 생성 성공");
    CHECK(fq_fs_create_new(p) == FQ_EEXIST, "재생성 거부(EEXIST)");
    fq_fs_unlink(p);

    /* 3) 권고 락 (선택) */
    printf("[3] 권고 락 (advisory lock)\n");
    snprintf(p, sizeof(p), "%s/lk", dir);
    {
        fq_lock *l1 = NULL;
        CHECK(fq_fs_trylock(p, &l1) == FQ_OK && l1 != NULL, "락 획득");
        CHECK(fq_fs_unlock(l1) == FQ_OK, "락 해제");
        printf("  [INFO] 노드 간 락 경합은 두 프로세스/노드로 별도 검증 필요.\n");
    }

    /* 4) FS 시각 단일 출처 */
    printf("[4] FS 시각 (clock skew 회피용)\n");
    {
        uint64_t t1 = 0, t2 = 0;
        CHECK(fq_fs_now_ms(dir, &t1) == FQ_OK && t1 > 0, "fs_now 읽기");
        CHECK(fq_fs_now_ms(dir, &t2) == FQ_OK && t2 >= t1, "fs_now 단조 증가");
    }

    /* 5) 디렉터리 나열 */
    printf("[5] 디렉터리 나열\n");
    {
        char f[1024];
        snprintf(f, sizeof(f), "%s/list_x", dir);
        fq_fs_write_sync(f, "1", 1);
        int n = 0;
        CHECK(fq_fs_list(dir, count_cb, &n) == FQ_OK && n >= 1, "항목 1개 이상");
        fq_fs_unlink(f);
    }

    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
