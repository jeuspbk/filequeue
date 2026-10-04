/* test_atomicity.c - DESIGN.md §7-1: 목표 스토리지에서 핵심 원자성 검증
 *
 * 큐 설계는 아래 3가지 FS 원자성에 의존한다. 실제 공유 스토리지에서 이 하니스를
 * 돌려 전제가 성립하는지 먼저 확인해야 한다.
 *   1) atomic rename (교체형 fq_fs_rename / 대상 존재 시 실패하는 noreplace)
 *   2) atomic exclusive create (CREATE_NEW / O_EXCL)
 *   3) advisory lock (선택적)
 * 그리고 교체형 rename 중 동시 읽기([6]): 교체가 읽는 쪽에 원자적이지 않은 FS도 있어(ENOENT가 잠깐
 * 보임) 라이브러리는 재시도로 흡수한다. 재시도 후에도 못 읽으면 그 스토리지는 쓸 수 없다.
 *
 * 사용:  test_atomicity [작업디렉터리]
 * 종료코드 0 = 모든 검사 통과.
 */
#include "fq_fs.h"
#include "fq.h"
#include "fq_internal.h"   /* fq_read_replaced, fq_now_mono_ms */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

static int count_cb(const char *name, void *ud) { (void)name; (*(int *)ud)++; return 0; }

/* [6]용: 한 스레드는 대상 파일을 tmp+rename으로 계속 교체하고, 메인 스레드는 계속 읽는다 */
typedef struct {
    char          dir[1024], target[1100];
    volatile int  stop;
    long          replaces;
} replace_ctx;

#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <windows.h>
typedef HANDLE thread_t;
typedef DWORD thread_ret;
#  define THREAD_CALL WINAPI
static int thread_start(thread_t *t, thread_ret (THREAD_CALL *fn)(void *), void *arg)
{ *t = CreateThread(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#  include <pthread.h>
typedef pthread_t thread_t;
typedef void *thread_ret;
#  define THREAD_CALL
static int thread_start(thread_t *t, thread_ret (*fn)(void *), void *arg)
{ return pthread_create(t, NULL, fn, arg); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
#endif

static thread_ret THREAD_CALL replace_writer(void *arg)
{
    replace_ctx *c = (replace_ctx *)arg;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s/replaced.tmp", c->dir);
    while (!c->stop) {
        if (fq_fs_write_sync(tmp, "v", 1) == FQ_OK && fq_fs_rename(tmp, c->target) == FQ_OK)
            c->replaces++;
    }
    return 0;
}

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
        CHECK(fq_fs_now_ms(dir, "test", &t1) == FQ_OK && t1 > 0, "fs_now 읽기");
        CHECK(fq_fs_now_ms(dir, "test", &t2) == FQ_OK && t2 >= t1, "fs_now 단조 증가");
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

    /* 6) 교체 중 읽기: leader.info는 교체형 rename으로 갱신되고 다른 노드가 동시에 읽는다.
     *    일부 FS(Cygwin/NTFS 실측)는 교체 순간 읽는 쪽에 ENOENT를 보인다. 라이브러리는 이를
     *    fq_read_replaced의 짧은 재시도로 흡수하므로, 재시도 후에도 못 읽는 경우가 0이어야 한다. */
    printf("[6] 교체형 rename 중 동시 읽기\n");
    {
        replace_ctx c;
        memset(&c, 0, sizeof(c));
        snprintf(c.dir, sizeof(c.dir), "%s", dir);
        snprintf(c.target, sizeof(c.target), "%s/replaced", dir);
        fq_fs_write_sync(c.target, "v", 1);
        thread_t th;
        CHECK(thread_start(&th, replace_writer, &c) == 0, "교체 스레드 시작");
        long reads = 0, raw_enoent = 0, misses = 0;
        uint64_t end = fq_now_mono_ms() + 1500;
        while (fq_now_mono_ms() < end) {
            void *buf = NULL; size_t len = 0;
            reads++;
            if (fq_fs_read_file(c.target, &buf, &len) == FQ_ENOENT) raw_enoent++;
            else free(buf);
            buf = NULL;
            if (fq_read_replaced(c.target, &buf, &len) != FQ_OK) misses++;
            else free(buf);
        }
        c.stop = 1;
        thread_join(th);
        printf("  [INFO] 읽기 %ld회, 교체 %ld회, 재시도 없이 ENOENT %ld회%s\n", reads, c.replaces,
               raw_enoent, raw_enoent ? " (이 FS는 교체가 읽는 쪽에 원자적이지 않음)" : "");
        CHECK(c.replaces > 0, "교체가 실제로 일어남");
        CHECK(misses == 0, "재시도 읽기(fq_read_replaced)는 항상 성공");
        fq_fs_unlink(c.target);
    }

    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
