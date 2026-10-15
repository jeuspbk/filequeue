/* fq_fs_win32.c - 네이티브 Win32 원자 연산 구현
 *
 * 주의: 이 파일은 MSVC 등 네이티브 Windows 빌드에서만 컴파일된다
 * (_WIN32 && !__CYGWIN__). 개발 머신의 cmake 빌드(Cygwin)는 fq_fs_posix.c를 쓰므로, 이 파일은
 * MinGW 크로스 컴파일러로 네이티브 빌드해 검증한다(CLAUDE.md). MSVC 빌드는 아직 검증되지 않았다.
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

    /* 드라이브("C:")·UNC("\\srv\share") 접두부는 만들 수 없다: CreateDirectory("\\srv")는 실패한다.
     * 건너뛰고 그 뒤부터 만든다. */
    size_t skip = fq_path_root_len(buf);
    for (char *p = buf + (skip < n ? skip + 1 : n); *p; p++) {
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

static int write_err_rc(DWORD e)
{
    return (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) ? FQ_ENOSPC : FQ_ERR;
}

/* 쓰기 실패 시 반쯤 쓰인 파일을 지운다(fq_fs.h). 오류 코드는 지우기 전에 보존한다. */
static int write_fail(const char *path, HANDLE h)
{
    int rc = write_err_rc(GetLastError());
    CloseHandle(h);
    DeleteFileA(path);
    return rc;
}

int fq_fs_write_sync(const char *path, const void *data, size_t len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return write_err_rc(GetLastError());

    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        DWORD chunk = (left > 0x40000000u) ? 0x40000000u : (DWORD)left;
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, NULL)) return write_fail(path, h);
        p += wrote;
        left -= wrote;
    }
    if (!FlushFileBuffers(h)) return write_fail(path, h);
    CloseHandle(h);
    return FQ_OK;
}

/* 다른 핸들이 열고 있는 대상을 교체하거나(leader.info를 대기 노드가 읽는 중), 경합 상대가 옮기는
 * 중인 src를 rename하면 ACCESS_DENIED/SHARING_VIOLATION이 잠깐 날 수 있다. 그대로 FQ_ERR로 돌려주면
 * 하트비트나 claim이 실패하므로 짧게 재시도한다. 경합에서 진 claim은 재시도 뒤 FILE_NOT_FOUND가 된다.
 * (MSVC 실기 검증 필요) */
#define FQ_RENAME_RETRIES 10

/* 전체 경로 길이(끝의 NUL 제외). 실패하면 0. */
static size_t full_path_len(const char *path)
{
    DWORD need = GetFullPathNameA(path, 0, NULL, NULL);   /* NUL 포함 필요 크기 */
    return need ? (size_t)need - 1 : 0;
}

int fq_fs_path_fits(const char *path, size_t extra)
{
    size_t n = full_path_len(path);
    return n && n + extra < MAX_PATH;
}

/* ANSI API는 전체 경로가 MAX_PATH를 넘으면 PATH_NOT_FOUND를 낸다(MinGW 실측). 그대로 FQ_ENOENT로 돌려주면
 * claim이 경합 패배로 보고 같은 메시지를 끝없이 다시 시도하므로 FQ_EINVAL로 구분한다(fq_fs.h). */
static int not_found_rc(const char *src, const char *dst, DWORD e)
{
    if (e == ERROR_FILENAME_EXCED_RANGE || !fq_fs_path_fits(src, 0) || !fq_fs_path_fits(dst, 0))
        return FQ_EINVAL;
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FQ_ENOENT : FQ_ERR;
}

int fq_fs_rename(const char *src, const char *dst)
{
    for (int i = 0;; i++) {
        if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return FQ_OK;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_FILENAME_EXCED_RANGE)
            return not_found_rc(src, dst, e);
        if ((e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) || i >= FQ_RENAME_RETRIES)
            return FQ_ERR;
        Sleep(1);
    }
}

/* FILE_RENAME_INFO의 Win10 RS1+ 배치(Flags). SDK마다 _WIN32_WINNT에 따라 선언이 달라 직접 둔다. */
typedef struct {
    DWORD  Flags;
    HANDLE RootDirectory;
    DWORD  FileNameLength;
    WCHAR  FileName[1];
} fq_rename_info_ex;
#define FQ_FILE_RENAME_INFO_EX      22   /* FILE_INFO_BY_HANDLE_CLASS FileRenameInfoEx */
#define FQ_RENAME_REPLACE_IF_EXISTS 0x1
#define FQ_RENAME_POSIX_SEMANTICS   0x2

int fq_fs_replace(const char *src, const char *dst)
{
    /* 핸들 기반 POSIX 의미 rename: dst를 FILE_SHARE_DELETE로 연 독자가 있어도 교체된다(fq_fs.h).
     * FileName은 전체 경로여야 한다(상대 이름은 src 디렉터리 기준으로 해석된다). */
    WCHAR wdst[1408], wfull[1408];
    if (MultiByteToWideChar(CP_ACP, 0, dst, -1, wdst, 1408) > 0) {
        DWORD fl = GetFullPathNameW(wdst, 1408, wfull, NULL);
        HANDLE h = (fl > 0 && fl < 1408)
            ? CreateFileA(src, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
            : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            size_t nbytes = (size_t)fl * sizeof(WCHAR);
            size_t sz = sizeof(fq_rename_info_ex) + nbytes;
            fq_rename_info_ex *ri = (fq_rename_info_ex *)calloc(1, sz);
            BOOL ok = FALSE;
            if (ri) {
                ri->Flags = FQ_RENAME_REPLACE_IF_EXISTS | FQ_RENAME_POSIX_SEMANTICS;
                ri->FileNameLength = (DWORD)nbytes;
                memcpy(ri->FileName, wfull, nbytes);
                ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)FQ_FILE_RENAME_INFO_EX,
                                                ri, (DWORD)sz);
                free(ri);
            }
            CloseHandle(h);
            if (ok) return FQ_OK;
        }
    }
    /* 구버전 OS·미지원 FS(SMB 등)·그 밖의 실패: 기존 경로(교체 실패 시 짧게 재시도) */
    return fq_fs_rename(src, dst);
}

int fq_fs_rename_noreplace(const char *src, const char *dst)
{
    /* REPLACE_EXISTING 미지정 → 대상 존재 시 실패 */
    if (MoveFileExA(src, dst, MOVEFILE_WRITE_THROUGH)) return FQ_OK;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return FQ_EEXIST;
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_FILENAME_EXCED_RANGE)
        return not_found_rc(src, dst, e);
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

int fq_fs_create_new_data(const char *path, const void *data, size_t len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) ? FQ_EEXIST : FQ_ERR;
    }
    const char *p = (const char *)data;
    while (len > 0) {
        DWORD chunk = (len > 0x40000000u) ? 0x40000000u : (DWORD)len;
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, NULL) || wrote == 0) {
            CloseHandle(h); DeleteFileA(path); return FQ_ERR;
        }
        p += wrote;
        len -= wrote;
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
    char pat[1408];
    snprintf(pat, sizeof(pat), "%s/*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        /* 전체 일치 패턴은 빈 디렉터리에서도 . .. 를 돌려주므로 NOT_FOUND는 디렉터리 자체가 없다는 뜻 */
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

/* 변경 알림: FindFirstChangeNotification. 핸들은 다음 FindNextChangeNotification까지 신호 상태로 남으므로
 * wait 사이에 생긴 변화도 놓치지 않는다. */
struct fq_watch { HANDLE h; };

int fq_fs_watch_open(const char *dir, fq_watch **out)
{
    HANDLE h = FindFirstChangeNotificationA(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
    if (h == INVALID_HANDLE_VALUE) return FQ_ERR;
    fq_watch *w = (fq_watch *)malloc(sizeof(*w));
    if (!w) { FindCloseChangeNotification(h); return FQ_ERR; }
    w->h = h;
    *out = w;
    return FQ_OK;
}

int fq_fs_watch_wait(fq_watch *w, uint32_t timeout_ms)
{
    DWORD r = WaitForSingleObject(w->h, timeout_ms);
    if (r == WAIT_TIMEOUT) return FQ_EEMPTY;
    if (r != WAIT_OBJECT_0) return FQ_ERR;
    if (!FindNextChangeNotification(w->h)) return FQ_ERR;   /* 다시 무장 */
    return FQ_OK;
}

void fq_fs_watch_close(fq_watch *w)
{
    if (!w) return;
    FindCloseChangeNotification(w->h);
    free(w);
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

int fq_fs_now_ms(const char *dir, const char *tag, uint64_t *out_ms)
{
    char tmp[1408], id[64];
    fq_gen_id(id, sizeof(id));
    snprintf(tmp, sizeof(tmp), "%s/.now-%s-%s", dir, tag, id);

    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return FQ_ERR;
    CloseHandle(h);

    int rc = fq_fs_mtime_ms(tmp, out_ms);
    DeleteFileA(tmp);
    return rc;
}

#endif /* _WIN32 && !__CYGWIN__ */
