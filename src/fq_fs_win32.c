/* fq_fs_win32.c - 네이티브 Win32 원자 연산 구현
 *
 * 주의: 이 파일은 MSVC 등 네이티브 Windows 빌드에서만 컴파일된다
 * (_WIN32 && !__CYGWIN__). 현재 개발 머신은 Cygwin gcc 이므로 fq_fs_posix.c가
 * 사용되며, 이 파일은 아직 실기 검증되지 않았다 (DESIGN.md §7-1 하니스로 검증 예정).
 */
#if defined(_WIN32) && !defined(__CYGWIN__)

#include "fq_fs.h"
#include "fq.h"
#include "fq_internal.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fq_lock { HANDLE h; };

int fq_fs_mkdirs(const char *path)
{
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return FQ_ERR;
    memcpy(buf, path, n + 1);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p; *p = '\0';
            if (!CreateDirectoryA(buf, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) return FQ_ERR;
            *p = c;
        }
    }
    if (!CreateDirectoryA(buf, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) return FQ_ERR;
    return FQ_OK;
}

int fq_fs_write_sync(const char *path, const void *data, size_t len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return FQ_ERR;

    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        DWORD chunk = (left > 0x40000000u) ? 0x40000000u : (DWORD)left;
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, NULL)) { CloseHandle(h); return FQ_ERR; }
        p += wrote;
        left -= wrote;
    }
    BOOL flushed = FlushFileBuffers(h);
    CloseHandle(h);
    return flushed ? FQ_OK : FQ_ERR;
}

int fq_fs_rename(const char *src, const char *dst)
{
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return FQ_OK;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return FQ_ENOENT;
    return FQ_ERR;
}

int fq_fs_rename_noreplace(const char *src, const char *dst)
{
    /* REPLACE_EXISTING 미지정 → 대상 존재 시 실패 */
    if (MoveFileExA(src, dst, MOVEFILE_WRITE_THROUGH)) return FQ_OK;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return FQ_EEXIST;
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return FQ_ENOENT;
    return FQ_ERR;
}

int fq_fs_create_new(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) return FQ_EEXIST;
        return FQ_ERR;
    }
    CloseHandle(h);
    return FQ_OK;
}

int fq_fs_unlink(const char *path)
{
    if (DeleteFileA(path)) return FQ_OK;
    DWORD e = GetLastError();
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FQ_ENOENT : FQ_ERR;
}

int fq_fs_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    if (a != INVALID_FILE_ATTRIBUTES) return 1;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
    return FQ_ERR;
}

int fq_fs_read_file(const char *path, void **out, size_t *out_len)
{
    /* FILE_SHARE_DELETE: 읽는 중에도 다른 쪽이 rename/unlink할 수 있어야 한다(POSIX와 같은 의미).
     * 없으면 fq_claim_if가 후보를 읽는 동안 리더의 claim rename이 공유 위반으로 실패한다. */
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FQ_ENOENT : FQ_ERR;
    }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return FQ_ERR; }
    size_t len = (size_t)sz.QuadPart;
    char *buf = (char *)malloc(len ? len : 1);
    if (!buf) { CloseHandle(h); return FQ_ERR; }

    size_t got = 0;
    while (got < len) {
        DWORD want = (DWORD)((len - got > 0x40000000u) ? 0x40000000u : (len - got));
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, want, &rd, NULL)) { free(buf); CloseHandle(h); return FQ_ERR; }
        if (rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    *out = buf;
    *out_len = got;
    return FQ_OK;
}

int fq_fs_fsync_dir(const char *path)
{
    (void)path; /* Win32: 디렉터리 fsync API 없음. write-through로 대체 */
    return FQ_OK;
}

int fq_fs_list(const char *dir, fq_dir_cb cb, void *ud)
{
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s/*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        /* "<dir>/*"는 빈 디렉터리에서도 . .. 를 돌려주므로 NOT_FOUND는 디렉터리 자체가 없다는 뜻 */
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FQ_ENOENT : FQ_ERR;
    }

    int rc = FQ_OK;
    for (;;) {
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
            int r = cb(fd.cFileName, ud);
            if (r != 0) { rc = r; break; }
        }
        if (!FindNextFileA(h, &fd)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) rc = FQ_ERR;
            break;
        }
    }

    FindClose(h);
    return rc;
}

int fq_fs_trylock(const char *path, fq_lock **out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FQ_ERR;

    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &ov)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) return FQ_ELOCKED;
        return FQ_ERR;
    }
    fq_lock *lk = (fq_lock *)malloc(sizeof(*lk));
    if (!lk) { CloseHandle(h); return FQ_ERR; }
    lk->h = h;
    *out = lk;
    return FQ_OK;
}

int fq_fs_unlock(fq_lock *lock)
{
    if (!lock) return FQ_OK;
    CloseHandle(lock->h); /* 닫으면 락 자동 해제 */
    free(lock);
    return FQ_OK;
}

static uint64_t filetime_to_ms(FILETIME ft)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000ULL;
}

int fq_fs_mtime_ms(const char *path, uint64_t *out_ms)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FQ_ENOENT : FQ_ERR;
    }
    *out_ms = filetime_to_ms(d.ftLastWriteTime);
    return FQ_OK;
}

int fq_fs_now_ms(const char *dir, uint64_t *out_ms)
{
    char tmp[1280], id[64];
    fq_gen_id(id, sizeof(id));
    snprintf(tmp, sizeof(tmp), "%s/.now-%s", dir, id);

    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return FQ_ERR;
    CloseHandle(h);

    int rc = fq_fs_mtime_ms(tmp, out_ms);
    DeleteFileA(tmp);
    return rc;
}

#endif /* _WIN32 && !__CYGWIN__ */
