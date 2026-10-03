/* fq_fs.h - 플랫폼 원자 연산 추상화 계층
 *
 * 큐 로직(fq_queue/fq_leader)은 이 인터페이스에만 의존한다.
 * 구현은 fq_fs_posix.c (POSIX/Cygwin) 와 fq_fs_win32.c (네이티브 Win32) 두 가지.
 *
 * 설계가 의존하는 핵심 원자성:
 *   1) fq_fs_rename            : 같은 볼륨 내 원자적 rename (경합 시 단 하나만 성공)
 *   2) fq_fs_create_new        : 원자적 배타 생성 (리더 선출 mutex)
 *   3) fq_fs_trylock           : 권고 락 (선택적 최적화)
 */
#ifndef FQ_FS_H
#define FQ_FS_H

#include <stddef.h>
#include <stdint.h>

typedef struct fq_lock fq_lock;   /* 불투명 핸들, 힙 할당 */

/* 디렉터리 생성 (mkdir -p). 이미 있으면 OK. */
int  fq_fs_mkdirs(const char *path);

/* data/len을 path에 기록하고 fsync (내용 영속화). 기존 파일은 덮어씀. */
int  fq_fs_write_sync(const char *path, const void *data, size_t len);

/* 원자적 rename. 대상이 있으면 원자적으로 교체한다(POSIX rename / MOVEFILE_REPLACE_EXISTING).
 * src가 없으면 FQ_ENOENT. 같은 src를 두 쪽이 동시에 rename하면 정확히 한 쪽만 성공한다.
 * 메시지 상태 전이(대상 이름이 구조상 유일)와 leader.info 교체에 쓴다. */
int  fq_fs_rename(const char *src, const char *dst);

/* 원자적 rename. 대상이 이미 있으면 FQ_EEXIST (덮어쓰지 않음).
 * 대상이 호출자 소유의 임의 경로일 때(fq_take)만 쓴다. POSIX 구현은 link+unlink라
 * 두 단계이므로 큐 내부 상태 전이에는 fq_fs_rename을 쓸 것. */
int  fq_fs_rename_noreplace(const char *src, const char *dst);

/* 빈 파일을 원자적·배타적으로 생성. 이미 있으면 FQ_EEXIST. */
int  fq_fs_create_new(const char *path);

int  fq_fs_unlink(const char *path);     /* 없으면 FQ_ENOENT */
int  fq_fs_exists(const char *path);     /* 1=있음, 0=없음, <0=오류 */

/* 파일 전체를 malloc 버퍼로 읽음 (호출자가 free). */
int  fq_fs_read_file(const char *path, void **out, size_t *out_len);

/* 디렉터리 메타데이터 영속화. 미지원 플랫폼에서는 no-op. */
int  fq_fs_fsync_dir(const char *path);

/* 디렉터리 나열. '.' '..' 제외. 반환: 끝까지 돌면 FQ_OK, cb가 0이 아닌 값을 반환하면 즉시 중단하고
 * 그 값을 그대로 반환(오류 전달용으로 음수 FQ_* 코드를 쓸 것), 디렉터리가 없으면 FQ_ENOENT,
 * 나열 도중 OS 오류면 FQ_ERR. */
typedef int (*fq_dir_cb)(const char *name, void *ud);
int  fq_fs_list(const char *dir, fq_dir_cb cb, void *ud);

/* 권고 배타 락 시도. 보유 중이면 FQ_ELOCKED. 성공 시 *out에 핸들. */
int  fq_fs_trylock(const char *path, fq_lock **out);
int  fq_fs_unlock(fq_lock *lock);

/* 파일 mtime을 ms로. */
int  fq_fs_mtime_ms(const char *path, uint64_t *out_ms);

/* FS 자체 시각을 ms로 (dir에 임시 파일을 만들어 mtime을 읽음).
 * 노드 간 clock skew를 피하기 위한 단일 시각 출처. */
int  fq_fs_now_ms(const char *dir, uint64_t *out_ms);

#endif /* FQ_FS_H */
