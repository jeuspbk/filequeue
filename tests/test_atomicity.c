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

#if defined(_WIN32) && !defined(__CYGWIN__)
/* [7]용: 잠깐 뒤에 핸들을 닫는다(남이 파일을 잠깐 쥐고 있는 상황) */
static thread_ret THREAD_CALL close_later(void *arg)
{
    Sleep(5);
    CloseHandle(*(HANDLE *)arg);
    return 0;
}

/* 이 볼륨이 핸들 기반 POSIX 의미 rename(FileRenameInfoEx)을 지원하는가. 라이브러리(fq_fs_replace)와 따로
 * 직접 시험한다 — 라이브러리 쪽이 고장 나도(변이 W01) 이 판정은 그대로여야 검사가 의미 있다.
 * NTFS(Win10 1809+)는 지원, SMB는 미지원(DESIGN.md §0). */
static int volume_has_posix_rename(const char *dir)
{
    char a[1100], b[1100];
    snprintf(a, sizeof(a), "%s/w_probe_a", dir);
    snprintf(b, sizeof(b), "%s/w_probe_b", dir);
    fq_fs_unlink(a); fq_fs_unlink(b);
    if (fq_fs_write_sync(a, "p", 1) != FQ_OK) return 0;
    WCHAR wb[1100], wfull[1100];
    int ok = 0;
    DWORD fl = 0;
    if (MultiByteToWideChar(CP_ACP, 0, b, -1, wb, 1100) > 0) fl = GetFullPathNameW(wb, 1100, wfull, NULL);
    HANDLE h = (fl > 0 && fl < 1100)
        ? CreateFileA(a, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
        : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        struct { DWORD Flags; HANDLE Root; DWORD Len; WCHAR Name[1100]; } ri;
        memset(&ri, 0, sizeof(ri));
        ri.Flags = 0x1 | 0x2;                          /* REPLACE_IF_EXISTS | POSIX_SEMANTICS */
        ri.Len = (DWORD)(fl * sizeof(WCHAR));
        memcpy(ri.Name, wfull, ri.Len);
        ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)22 /* FileRenameInfoEx */, &ri, sizeof(ri)) != 0;
        CloseHandle(h);
    }
    fq_fs_unlink(a); fq_fs_unlink(b);
    return ok;
}
#endif

static thread_ret THREAD_CALL replace_writer(void *arg)
{
    replace_ctx *c = (replace_ctx *)arg;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s/replaced.tmp", c->dir);
    while (!c->stop) {
        if (fq_fs_write_sync(tmp, "v", 1) == FQ_OK && fq_fs_replace(tmp, c->target) == FQ_OK)
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

    /* 6) 교체 중 읽기: leader.info는 fq_fs_replace로 갱신되고 다른 노드가 동시에 읽는다.
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
        long reads = 0, raw_enoent = 0, misses = 0, miss_enoent = 0;
        uint64_t end = fq_now_mono_ms() + 1500;
        while (fq_now_mono_ms() < end) {
            void *buf = NULL; size_t len = 0;
            reads++;
            if (fq_fs_read_file(c.target, &buf, &len) == FQ_ENOENT) raw_enoent++;
            else free(buf);
            buf = NULL;
            int rrc = fq_read_replaced(c.target, &buf, &len);
            if (rrc != FQ_OK) { misses++; if (rrc == FQ_ENOENT) miss_enoent++; }
            else free(buf);
        }
        c.stop = 1;
        thread_join(th);
        printf("  [INFO] 읽기 %ld회, 교체 %ld회, 재시도 없이 ENOENT %ld회%s\n", reads, c.replaces,
               raw_enoent, raw_enoent ? " (이 FS는 교체가 읽는 쪽에 원자적이지 않음)" : "");
        if (misses) printf("  [INFO] 재시도 뒤에도 실패 %ld회 (ENOENT %ld, 그 밖 %ld)\n", misses, miss_enoent,
                           misses - miss_enoent);
        CHECK(c.replaces > 0, "교체가 실제로 일어남");
        CHECK(misses == 0, "재시도 읽기(fq_read_replaced)는 항상 성공");
        fq_fs_unlink(c.target);
    }

#if defined(_WIN32) && !defined(__CYGWIN__)
    /* 7) Win32 전용: 다른 핸들이 연 파일과 겹치는 교체·rename. [6]은 타이밍에 따라 겹치지 않고도 통과할 수
     *    있어서, 겹침을 결정적으로 만든다(변이 W01·W02: tests/mutations_win32.txt). */
    printf("[7] Win32: 다른 핸들이 연 파일의 교체·rename\n");
    {
        char tgt[1100], tmp[1100], src[1100], dst[1100];
        snprintf(tgt, sizeof(tgt), "%s/w_target", dir);
        snprintf(tmp, sizeof(tmp), "%s/w_target.tmp", dir);
        fq_fs_unlink(tgt);
        CHECK(fq_fs_write_sync(tgt, "old", 3) == FQ_OK && fq_fs_write_sync(tmp, "new", 3) == FQ_OK, "대상·새 내용 작성");
        /* 대기 노드가 leader.info를 읽는 중: fq_fs_read_file과 같은 공유 모드로 열어 둔다 */
        HANDLE rd = CreateFileA(tgt, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(rd != INVALID_HANDLE_VALUE, "대상을 읽기 핸들로 열어 둠");
        int rrc = fq_fs_replace(tmp, tgt);
        if (rd != INVALID_HANDLE_VALUE) CloseHandle(rd);
        if (volume_has_posix_rename(dir)) {
            CHECK(rrc == FQ_OK, "열린 대상도 fq_fs_replace로 교체 (POSIX 의미 rename. MoveFileEx는 거부)");
            void *d = NULL; size_t n = 0;
            CHECK(fq_fs_read_file(tgt, &d, &n) == FQ_OK && n == 3 && memcmp(d, "new", 3) == 0, "  ... 새 내용");
            free(d);
        } else {
            printf("  [INFO] 이 볼륨은 POSIX 의미 rename 미지원(SMB 등): 열린 대상 교체 %s. 대기 노드가 leader.info를\n"
                   "         읽는 동안 하트비트가 일시 실패(FQ_ERR)할 수 있다 — 리더십은 안전(DESIGN.md §0 SMB 실측)\n",
                   rrc == FQ_OK ? "성공" : "실패");
        }
        fq_fs_unlink(tgt); fq_fs_unlink(tmp);

        /* 원본을 남이 공유 삭제 없이 잠깐(5ms) 쥐고 있음: rename은 공유 위반을 짧게 재시도해 넘겨야 한다 */
        snprintf(src, sizeof(src), "%s/w_src", dir);
        snprintf(dst, sizeof(dst), "%s/w_dst", dir);
        fq_fs_unlink(src); fq_fs_unlink(dst);
        CHECK(fq_fs_write_sync(src, "s", 1) == FQ_OK, "원본 작성");
        HANDLE hold = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        thread_t th;
        int started = hold != INVALID_HANDLE_VALUE && thread_start(&th, close_later, &hold) == 0;
        CHECK(started, "원본을 공유 삭제 없이 열고 5ms 뒤 닫는 스레드");
        CHECK(fq_fs_rename(src, dst) == FQ_OK, "잠깐 쥔 원본도 fq_fs_rename이 재시도로 옮김");
        if (started) thread_join(th);
        CHECK(fq_fs_exists(dst) == 1 && fq_fs_exists(src) == 0, "  ... 옮겨짐");
        fq_fs_unlink(dst);
    }
#endif

    printf("\n== 결과: %s (%d 실패) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
