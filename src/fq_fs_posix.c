/* fq_fs_posix.c - POSIX/Cygwin 원자 연산 구현 */
#if !defined(_WIN32) || defined(__CYGWIN__)

#include "fq_fs.h"
#include "fq.h"
#include "fq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

struct fq_lock { int fd; };

int fq_fs_mkdirs(const char *path)
{
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return FQ_ERR;
    memcpy(buf, path, n + 1);

    /* "C:/..."(Cygwin이 받는 Windows 경로)에서 mkdir("C:")을 부르면 현재 디렉터리에 "C:"라는
     * 디렉터리가 생긴다. 드라이브·UNC 접두부는 건너뛰고 그 뒤부터 만든다. */
    size_t skip = fq_path_root_len(buf);
    for (char *p = buf + (skip < n ? skip + 1 : n); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return FQ_ERR;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return FQ_ERR;
    return FQ_OK;
}

static int write_errno_rc(int e)
{
#ifdef EDQUOT
    if (e == EDQUOT) return FQ_ENOSPC;
#endif
    return e == ENOSPC ? FQ_ENOSPC : FQ_ERR;
}

/* 쓰기 실패 시 반쯤 쓰인 파일을 지운다(fq_fs.h). 디스크가 찰수록 실패한 임시 파일이 남은 공간을
 * 갉아먹지 않도록. errno는 unlink 전에 보존한다. */
static int write_fail(const char *path, int fd)
{
    int rc = write_errno_rc(errno);
    if (fd >= 0) close(fd);
    unlink(path);
    return rc;
}

int fq_fs_write_sync(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return write_errno_rc(errno);

    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return write_fail(path, fd);
        }
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd) != 0) return write_fail(path, fd);
    if (close(fd) != 0) return write_fail(path, -1);
    return FQ_OK;
}

int fq_fs_rename(const char *src, const char *dst)
{
    /* rename(2)은 원자적이며 대상을 교체한다. src를 두 쪽이 동시에 rename하면 한 쪽은 ENOENT. */
    if (rename(src, dst) != 0)
        return (errno == ENOENT) ? FQ_ENOENT : FQ_ERR;
    return FQ_OK;
}

int fq_fs_rename_noreplace(const char *src, const char *dst)
{
    /* POSIX rename(2)은 대상을 덮어쓴다. fail-if-exists 의미를 위해 link+unlink 사용.
     * 두 단계라 크래시/경합 시 src와 dst가 잠시 공존할 수 있다 → 큐 내부 전이에는 쓰지 않는다. */
    if (link(src, dst) != 0) {
        if (errno == EEXIST) return FQ_EEXIST;
        if (errno == ENOENT) return FQ_ENOENT;
        /* link 미지원 FS 폴백: 대상 존재 검사 후 rename (경합에 약함, 스캐폴드) */
        if (errno == EPERM || errno == ENOSYS) {
            if (access(dst, F_OK) == 0) return FQ_EEXIST;
            if (rename(src, dst) != 0)
                return (errno == ENOENT) ? FQ_ENOENT : FQ_ERR;
            return FQ_OK;
        }
        return FQ_ERR;
    }
    if (unlink(src) != 0) {
        /* src가 남으면 같은 메시지가 두 곳에 있다(inflight의 src는 나중에 recover로 재전달).
         * dst를 거둬 "옮기지 않음"으로 되돌린다. ENOENT면 그 사이 다른 쪽이 src를 옮긴 것이다. */
        int e = errno;
        unlink(dst);
        return e == ENOENT ? FQ_ENOENT : FQ_ERR;
    }
    return FQ_OK;
}

int fq_fs_create_new(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return FQ_EEXIST;
        return FQ_ERR;
    }
    close(fd);
    return FQ_OK;
}

int fq_fs_unlink(const char *path)
{
    if (unlink(path) != 0)
        return (errno == ENOENT) ? FQ_ENOENT : FQ_ERR;
    return FQ_OK;
}

int fq_fs_exists(const char *path)
{
    if (access(path, F_OK) == 0) return 1;
    if (errno == ENOENT) return 0;
    return FQ_ERR;
}

int fq_fs_read_file(const char *path, void **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return (errno == ENOENT) ? FQ_ENOENT : FQ_ERR;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return FQ_ERR; }
    size_t len = (size_t)st.st_size;
    char *buf = (char *)malloc(len ? len : 1);
    if (!buf) { close(fd); return FQ_ERR; }

    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return FQ_ERR; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    *out = buf;
    *out_len = got;
    return FQ_OK;
}

int fq_fs_fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return FQ_ERR;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? FQ_OK : FQ_OK; /* 일부 FS는 dir fsync 미지원 → 치명적 아님 */
}

int fq_fs_list(const char *dir, fq_dir_cb cb, void *ud)
{
    DIR *d = opendir(dir);
    if (!d) return (errno == ENOENT) ? FQ_ENOENT : FQ_ERR;

    struct dirent *e;
    int rc = FQ_OK;
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) { if (errno != 0) rc = FQ_ERR; break; }   /* NULL + errno = 나열 오류 */
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        int r = cb(e->d_name, ud);
        if (r != 0) { rc = r; break; }
    }
    closedir(d);
    return rc;
}

int fq_fs_trylock(const char *path, fq_lock **out)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return FQ_ERR;

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; /* 파일 전체 */

    if (fcntl(fd, F_SETLK, &fl) != 0) {
        close(fd);
        if (errno == EACCES || errno == EAGAIN) return FQ_ELOCKED;
        return FQ_ERR;
    }
    fq_lock *lk = (fq_lock *)malloc(sizeof(*lk));
    if (!lk) { close(fd); return FQ_ERR; }
    lk->fd = fd;
    *out = lk;
    return FQ_OK;
}

int fq_fs_unlock(fq_lock *lock)
{
    if (!lock) return FQ_OK;
    close(lock->fd); /* 닫으면 락 자동 해제 */
    free(lock);
    return FQ_OK;
}

int fq_fs_mtime_ms(const char *path, uint64_t *out_ms)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return (errno == ENOENT) ? FQ_ENOENT : FQ_ERR;
#if defined(__APPLE__)
    *out_ms = (uint64_t)st.st_mtimespec.tv_sec * 1000ULL +
              (uint64_t)st.st_mtimespec.tv_nsec / 1000000ULL;
#else
    *out_ms = (uint64_t)st.st_mtim.tv_sec * 1000ULL +
              (uint64_t)st.st_mtim.tv_nsec / 1000000ULL;
#endif
    return FQ_OK;
}

int fq_fs_now_ms(const char *dir, const char *tag, uint64_t *out_ms)
{
    /* dir 안에 임시 파일을 만들어 그 mtime을 읽음 → FS 단일 시각 출처 */
    char tmp[1408];
    char id[64];
    fq_gen_id(id, sizeof(id));
    snprintf(tmp, sizeof(tmp), "%s/.now-%s-%s", dir, tag, id);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return FQ_ERR;
    close(fd);

    int rc = fq_fs_mtime_ms(tmp, out_ms);
    unlink(tmp);
    return rc;
}

#endif /* !_WIN32 || __CYGWIN__ */
