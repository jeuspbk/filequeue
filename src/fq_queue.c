/* fq_queue.c - 발행 / claim / ack / nack
 *
 * 핵심 원칙(DESIGN.md §0): 메시지 상태 = 디렉터리 위치, 전이 = 원자적 rename/unlink.
 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void claim_cache_clear(fq_queue *q);

int fq_open(const char *root, const char *node_id, fq_queue **out)
{
    fq_queue *q = (fq_queue *)calloc(1, sizeof(*q));
    if (!q) return FQ_ERR;

    snprintf(q->root, sizeof(q->root), "%s", root);

    /* node_id: 인자 우선, 없으면 환경변수 FQ_NODE_ID, 그것도 없으면 node-<pid> */
    if (node_id && node_id[0]) {
        snprintf(q->node_id, sizeof(q->node_id), "%s", node_id);
    } else {
        const char *env = getenv("FQ_NODE_ID");
        if (env && env[0])
            snprintf(q->node_id, sizeof(q->node_id), "%s", env);
        else
            snprintf(q->node_id, sizeof(q->node_id), "node-%u", fq_pid());
    }

    /* 하위 디렉터리 생성 */
    const char *subs[] = { FQ_DIR_TMP, FQ_DIR_INCOMING, FQ_DIR_INFLIGHT,
                           FQ_DIR_DEAD, FQ_DIR_CONTROL };
    char path[1280];
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        fq_path(path, sizeof(path), q->root, subs[i], NULL);
        if (fq_fs_mkdirs(path) != FQ_OK) { free(q); return FQ_ERR; }
    }

    *out = q;
    return FQ_OK;
}

void fq_close(fq_queue *q)
{
    if (!q) return;
    claim_cache_clear(q);
    free(q);
}

/* ---- Producer ---- */

/* 최종 메시지명: <epoch_ms>-<seq>-<producer>-<id>[.<stream>].msg
 * 앞쪽 시간+seq 로 best-effort FIFO 정렬. */
static void incoming_path(fq_queue *q, const char *id, const char *stream_key, char *msg_path, size_t n)
{
    uint64_t now = fq_now_wall_ms();
    uint32_t seq = ++q->seq;
    char msg_name[512];
    if (stream_key && stream_key[0])
        snprintf(msg_name, sizeof(msg_name), "%016llu-%06u-%s-%s.%s.msg",
                 (unsigned long long)now, seq, q->node_id, id, stream_key);
    else
        snprintf(msg_name, sizeof(msg_name), "%016llu-%06u-%s-%s.msg",
                 (unsigned long long)now, seq, q->node_id, id);
    fq_path(msg_path, n, q->root, FQ_DIR_INCOMING, msg_name);
}

static void fsync_incoming(fq_queue *q)
{
    char inc_dir[1280];
    fq_path(inc_dir, sizeof(inc_dir), q->root, FQ_DIR_INCOMING, NULL);
    fq_fs_fsync_dir(inc_dir);
}

int fq_publish(fq_queue *q, const void *data, size_t len, const char *stream_key)
{
    char id[64];
    fq_gen_id(id, sizeof(id));

    /* 1) tmp에 기록 + fsync */
    char tmp_name[128], tmp_path[1408];
    snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", id);
    fq_path(tmp_path, sizeof(tmp_path), q->root, FQ_DIR_TMP, tmp_name);
    if (fq_fs_write_sync(tmp_path, data, len) != FQ_OK) return FQ_ERR;

    /* 2) 최종 메시지명 */
    char msg_path[1408];
    incoming_path(q, id, stream_key, msg_path, sizeof(msg_path));

    /* 3) 원자적 rename (대상은 유일하므로 EEXIST 없음) */
    int rc = fq_fs_rename_noreplace(tmp_path, msg_path);
    if (rc != FQ_OK) { fq_fs_unlink(tmp_path); return rc; }

    /* 4) 디렉터리 영속화 */
    fsync_incoming(q);
    return FQ_OK;
}

/* ---- 2단계 발행 / 꺼내기 (트랜잭션 큐용, fq.h 참고) ---- */

int fq_adopt(fq_queue *q, const char *path, const char *stream_key)
{
    char id[64];
    fq_gen_id(id, sizeof(id));
    char msg_path[1408];
    incoming_path(q, id, stream_key, msg_path, sizeof(msg_path));
    int rc = fq_fs_rename_noreplace(path, msg_path);   /* 원본이 없으면 FQ_ENOENT: 이미 옮겨짐 */
    if (rc != FQ_OK) return rc;
    fsync_incoming(q);
    return FQ_OK;
}

int fq_take(fq_queue *q, fq_msg *m, const char *path)
{
    char src[1408];
    fq_path(src, sizeof(src), q->root, FQ_DIR_INFLIGHT, m->name);
    int rc = fq_fs_rename_noreplace(src, path);
    if (rc != FQ_OK) return rc;                         /* m은 여전히 claim 상태 */
    /* 옮긴 곳의 디렉터리 영속화 */
    char dir[1408];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bs = strrchr(dir, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    if (slash) {
        *slash = '\0';
        fq_fs_fsync_dir(dir[0] ? dir : "/");
    }
    fq_msg_free(m);
    return FQ_OK;
}

/* ---- Consumer: claim ---- */

/* incoming 나열을 모아 가장 오래된 것부터 시도하기 위한 수집기 */
typedef struct { char **v; size_t n, cap; } strvec;

static int collect_cb(const char *name, void *ud)
{
    strvec *sv = (strvec *)ud;
    if (sv->n == sv->cap) {
        size_t nc = sv->cap ? sv->cap * 2 : 32;
        char **nv = (char **)realloc(sv->v, nc * sizeof(char *));
        if (!nv) return 1; /* 중단 */
        sv->v = nv;
        sv->cap = nc;
    }
    sv->v[sv->n++] = fq_strdup(name);
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void strvec_free(strvec *sv)
{
    for (size_t i = 0; i < sv->n; i++) free(sv->v[i]);
    free(sv->v);
}

static void claim_cache_clear(fq_queue *q)
{
    for (size_t i = 0; i < q->claim_n; i++) free(q->claim_cache[i]);
    free(q->claim_cache);
    q->claim_cache = NULL;
    q->claim_n = q->claim_idx = q->claim_cap = 0;
}

/* incoming/ 을 나열·정렬하여 캐시를 채운다. 비었으면 0, 채웠으면 1, 오류면 -1. */
static int claim_cache_fill(fq_queue *q)
{
    char inc_dir[1280];
    fq_path(inc_dir, sizeof(inc_dir), q->root, FQ_DIR_INCOMING, NULL);

    strvec sv = {0};
    if (fq_fs_list(inc_dir, collect_cb, &sv) != FQ_OK) { strvec_free(&sv); return -1; }
    if (sv.n == 0) { strvec_free(&sv); return 0; }

    qsort(sv.v, sv.n, sizeof(char *), cmp_str); /* 이름(시간) 오름차순 */

    claim_cache_clear(q);
    q->claim_cache = sv.v;
    q->claim_n     = sv.n;
    q->claim_cap   = sv.cap;
    q->claim_idx   = 0;
    return 1;
}

int fq_claim(fq_queue *q, const fq_lease *lease, fq_msg **out)
{
    for (;;) {
        /* 캐시 소진 시 재나열. 새로 채울 게 없으면 큐가 비었다는 뜻. */
        if (q->claim_idx >= q->claim_n) {
            int f = claim_cache_fill(q);
            if (f < 0) return FQ_ERR;
            if (f == 0) { claim_cache_clear(q); return FQ_EEMPTY; }
        }

        while (q->claim_idx < q->claim_n) {
            const char *name = q->claim_cache[q->claim_idx++];

            char base[512];
            uint32_t attempt = 0;
            fq_parse_attempt(name, base, sizeof(base), &attempt);

            /* CLAIM: incoming/<name> -> inflight/<name>__t<token> (원자적 rename) */
            char inf_name[640];
            snprintf(inf_name, sizeof(inf_name), "%s__t%llu",
                     name, (unsigned long long)lease->token);

            char src[1408], dst[1408];
            fq_path(src, sizeof(src), q->root, FQ_DIR_INCOMING, name);
            fq_path(dst, sizeof(dst), q->root, FQ_DIR_INFLIGHT, inf_name);

            int rc = fq_fs_rename_noreplace(src, dst);
            if (rc == FQ_ENOENT || rc == FQ_EEXIST) continue; /* 경합 패배/이미 처리됨 */
            if (rc != FQ_OK) return FQ_ERR;

            void *data = NULL; size_t len = 0;
            if (fq_fs_read_file(dst, &data, &len) != FQ_OK) return FQ_ERR;

            fq_msg *m = (fq_msg *)calloc(1, sizeof(*m));
            if (!m) { free(data); return FQ_ERR; }
            snprintf(m->name, sizeof(m->name), "%s", inf_name);
            m->data = data;
            m->len = len;
            m->attempt = attempt;

            *out = m;
            return FQ_OK;
        }
        /* 이 배치를 다 소진 → 루프 상단에서 재나열 시도 */
    }
}

/* ---- ack / nack ---- */

int fq_ack(fq_queue *q, fq_msg *m)
{
    char path[1408];
    fq_path(path, sizeof(path), q->root, FQ_DIR_INFLIGHT, m->name);
    int rc = fq_fs_unlink(path);
    fq_msg_free(m);
    return (rc == FQ_OK || rc == FQ_ENOENT) ? FQ_OK : rc;
}

int fq_nack(fq_queue *q, fq_msg *m)
{
    /* inflight -> incoming 으로 되돌림 (attempt+1), 한도 초과 시 dead/ */
    char logical[640];
    fq_parse_inflight(m->name, logical, sizeof(logical));

    char base[512];
    uint32_t attempt = 0;
    fq_parse_attempt(logical, base, sizeof(base), &attempt);

    char src[1408];
    fq_path(src, sizeof(src), q->root, FQ_DIR_INFLIGHT, m->name);

    int rc;
    if (attempt + 1 >= FQ_MAX_ATTEMPTS) {
        char dst[1408];
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_DEAD, logical);
        rc = fq_fs_rename_noreplace(src, dst);
    } else {
        char req_name[640], dst[1408];
        snprintf(req_name, sizeof(req_name), "%s.a%u", base, attempt + 1);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_INCOMING, req_name);
        rc = fq_fs_rename_noreplace(src, dst);
    }
    fq_msg_free(m);
    return rc;
}

void fq_msg_free(fq_msg *m)
{
    if (!m) return;
    free(m->data);
    free(m);
}

/* ---- GC ---- */

typedef struct { fq_queue *q; uint64_t cutoff; int removed; } gc_ctx;

static int gc_cb(const char *name, void *ud)
{
    gc_ctx *c = (gc_ctx *)ud;
    char path[1408];
    fq_path(path, sizeof(path), c->q->root, FQ_DIR_TMP, name);

    uint64_t mt = 0;
    if (fq_fs_mtime_ms(path, &mt) == FQ_OK && mt < c->cutoff) {
        if (fq_fs_unlink(path) == FQ_OK) c->removed++;
    }
    return 0;
}

int fq_gc(fq_queue *q, uint64_t tmp_max_age_ms)
{
    char tmp_dir[1280];
    fq_path(tmp_dir, sizeof(tmp_dir), q->root, FQ_DIR_TMP, NULL);

    uint64_t now = 0;
    if (fq_fs_now_ms(tmp_dir, &now) != FQ_OK) return FQ_ERR;

    gc_ctx c = { q, now > tmp_max_age_ms ? now - tmp_max_age_ms : 0, 0 };
    if (fq_fs_list(tmp_dir, gc_cb, &c) != FQ_OK) return FQ_ERR;
    return c.removed;
}
