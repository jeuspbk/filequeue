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

/* root 상한: 하위 경로("<root>/inflight/<이름>__t<token>.a<N>", 이름 최대 약 270자)가 경로 버퍼(1408)에
 * 잘리지 않고 들어가야 한다. snprintf가 조용히 잘라 엉뚱한 경로를 쓰는 일을 막는다. */
#define FQ_ROOT_MAX 900

int fq_open(const char *root, const char *node_id, fq_queue **out)
{
    if (!root || !root[0] || !out || strlen(root) > FQ_ROOT_MAX) return FQ_EINVAL;
    fq_queue *q = (fq_queue *)calloc(1, sizeof(*q));
    if (!q) return FQ_ERR;

    snprintf(q->root, sizeof(q->root), "%s", root);

    /* node_id: 인자 우선, 없으면 환경변수 FQ_NODE_ID, 그것도 없으면 node-<pid> */
    if (node_id && node_id[0]) {
        if (!fq_valid_ident(node_id, sizeof(q->node_id) - 1)) { free(q); return FQ_EINVAL; }
        snprintf(q->node_id, sizeof(q->node_id), "%s", node_id);
    } else {
        const char *env = getenv("FQ_NODE_ID");
        if (env && env[0]) {
            if (!fq_valid_ident(env, sizeof(q->node_id) - 1)) { free(q); return FQ_EINVAL; }
            snprintf(q->node_id, sizeof(q->node_id), "%s", env);
        } else {
            fq_default_node_id(q->node_id, sizeof(q->node_id));
        }
    }
    fq_gen_id(q->instance, sizeof(q->instance));

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
    if (q->lead_held) fq_release_leadership(q, &q->lead_lease);  /* 실패해도 리스 만료로 넘어간다 */
    claim_cache_clear(q);
    free(q);
}

/* ---- Producer ---- */

/* 추정 FS 시각(ms). DESIGN.md §4-4.
 * 로컬 벽시계에 "공유 FS 시각 - 로컬 벽시계" 오프셋을 더한다. 오프셋은 FQ_CLOCK_SYNC_MS마다
 * 한 번만 재서(파일 생성+stat+삭제 1회) 호출 비용을 늘리지 않는다. 로컬 시계가 크게 뒤로 가면
 * 즉시 다시 잰다. 재동기에 실패하면 직전 오프셋을 유지한다(처음부터 실패하면 0 = 순수 벽시계). */
uint64_t fq_fs_clock_est_ms(fq_queue *q)
{
    uint64_t wall = fq_now_wall_ms();

    if (q->clock_next_sync_ms == 0 || wall >= q->clock_next_sync_ms ||
        wall + FQ_CLOCK_SYNC_MS < q->clock_next_sync_ms /* 로컬 시계가 크게 뒤로 감 */) {
        char tmp_dir[1280];
        fq_path(tmp_dir, sizeof(tmp_dir), q->root, FQ_DIR_TMP, NULL);
        uint64_t fs_now = 0;
        if (fq_fs_now_ms(tmp_dir, q->node_id, &fs_now) == FQ_OK)
            q->clock_offset_ms = (int64_t)fs_now - (int64_t)wall;
        q->clock_next_sync_ms = wall + FQ_CLOCK_SYNC_MS;
    }

    int64_t t = (int64_t)wall + q->clock_offset_ms;
    return t > 0 ? (uint64_t)t : 0;
}

/* 메시지명에 쓸 시각(ms): 추정 FS 시각 + 단조 보정.
 * 모든 발행 노드가 같은 시계(FS)를 기준으로 이름을 붙이고, 마지막으로 발급한 값보다 작아지면
 * 그 값에 머물러 한 프로세스 안에서는 절대 역행하지 않는다. 같은 ms 안의 순서는 seq가 정한다.
 * (+1ms씩 밀면 초당 1000건 넘게 발행할 때 이름의 시각이 실제보다 계속 앞서 나가 다른 노드와의
 * FIFO가 어긋난다.) */
static uint64_t msg_clock_ms(fq_queue *q)
{
    uint64_t now = fq_fs_clock_est_ms(q);
    if (now < q->last_msg_ms) now = q->last_msg_ms;
    q->last_msg_ms = now;
    return now;
}

/* 최종 메시지명: <epoch_ms>-<seq>-<producer>-<id>[.<stream>].msg
 * 앞쪽 시간+seq 로 best-effort FIFO 정렬. 시각은 msg_clock_ms (FS 시각 기준 보정값).
 * seq는 10자리 고정폭: 같은 ms 안의 순서를 문자열 정렬로 보존한다(uint32 전 범위). */
static void new_msg_name(fq_queue *q, const char *id, const char *stream_key, char *msg_name, size_t n)
{
    uint64_t now = msg_clock_ms(q);
    uint32_t seq = ++q->seq;
    if (stream_key && stream_key[0])
        snprintf(msg_name, n, "%016llu-%010u-%s-%s.%s.msg",
                 (unsigned long long)now, seq, q->node_id, id, stream_key);
    else
        snprintf(msg_name, n, "%016llu-%010u-%s-%s.msg",
                 (unsigned long long)now, seq, q->node_id, id);
}

static void incoming_path(fq_queue *q, const char *id, const char *stream_key, char *msg_path, size_t n)
{
    char msg_name[512];
    new_msg_name(q, id, stream_key, msg_name, sizeof(msg_name));
    fq_path(msg_path, n, q->root, FQ_DIR_INCOMING, msg_name);
}

static void fsync_incoming(fq_queue *q)
{
    char inc_dir[1280];
    fq_path(inc_dir, sizeof(inc_dir), q->root, FQ_DIR_INCOMING, NULL);
    fq_fs_fsync_dir(inc_dir);
}

#define FQ_STREAM_KEY_MAX 127

int fq_publish(fq_queue *q, const void *data, size_t len, const char *stream_key)
{
    if (stream_key && stream_key[0] && !fq_valid_ident(stream_key, FQ_STREAM_KEY_MAX))
        return FQ_EINVAL;
    char id[64];
    fq_gen_id(id, sizeof(id));

    /* 1) tmp에 기록 + fsync. tmp/는 모든 발행 노드가 공유하므로 이름에 node_id를 넣어
     *    다른 노드와 같은 임시 파일을 덮어쓰는 일(메시지 유실)을 구조적으로 막는다. */
    char tmp_name[192], tmp_path[1408];
    snprintf(tmp_name, sizeof(tmp_name), "%s-%s.tmp", q->node_id, id);
    fq_path(tmp_path, sizeof(tmp_path), q->root, FQ_DIR_TMP, tmp_name);
    if (fq_fs_write_sync(tmp_path, data, len) != FQ_OK) return FQ_ERR;

    /* 2) 최종 메시지명 */
    char msg_path[1408];
    incoming_path(q, id, stream_key, msg_path, sizeof(msg_path));

    /* 3) 원자적 rename (대상 이름은 구조상 유일) */
    int rc = fq_fs_rename(tmp_path, msg_path);
    if (rc != FQ_OK) { fq_fs_unlink(tmp_path); return rc; }

    /* 4) 디렉터리 영속화 */
    fsync_incoming(q);
    return FQ_OK;
}

/* ---- 2단계 발행 / 꺼내기 (트랜잭션 큐용, fq.h 참고) ---- */

int fq_adopt(fq_queue *q, const char *path, const char *stream_key)
{
    if (stream_key && stream_key[0] && !fq_valid_ident(stream_key, FQ_STREAM_KEY_MAX))
        return FQ_EINVAL;
    char id[64];
    fq_gen_id(id, sizeof(id));
    char msg_path[1408];
    incoming_path(q, id, stream_key, msg_path, sizeof(msg_path));
    int rc = fq_fs_rename(path, msg_path);   /* 원본이 없으면 FQ_ENOENT: 이미 옮겨짐 */
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

int fq_return(fq_queue *q, const char *path, uint32_t attempt)
{
    /* fq_nack과 같은 규칙: attempt+1로 incoming, 한도에 닿으면 dead/ */
    char id[64];
    fq_gen_id(id, sizeof(id));
    char name[512];
    new_msg_name(q, id, NULL, name, sizeof(name));
    char dst[1408];
    int dead = attempt + 1 >= FQ_MAX_ATTEMPTS;
    if (dead) {
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_DEAD, name);
    } else {
        char req_name[640];
        snprintf(req_name, sizeof(req_name), "%s.a%u", name, attempt + 1);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_INCOMING, req_name);
    }
    int rc = fq_fs_rename(path, dst);         /* 원본이 없으면 FQ_ENOENT: 이미 되돌림 */
    if (rc != FQ_OK) return rc;
    if (dead) {
        char dead_dir[1280];
        fq_path(dead_dir, sizeof(dead_dir), q->root, FQ_DIR_DEAD, NULL);
        fq_fs_fsync_dir(dead_dir);
    } else {
        fsync_incoming(q);
    }
    return FQ_OK;
}

/* ---- Consumer: claim ---- */

/* incoming 나열을 모아 가장 오래된 것부터 시도하기 위한 수집기 */
typedef struct { char **v; size_t n, cap; } strvec;

/* 메모리 부족이면 FQ_ERR로 나열을 중단한다 → fq_fs_list가 그 값을 돌려준다.
 * (예전에는 strdup 실패 시 NULL을 넣어 qsort/strcmp에서 죽었다.) */
static int collect_cb(const char *name, void *ud)
{
    strvec *sv = (strvec *)ud;
    if (sv->n == sv->cap) {
        size_t nc = sv->cap ? sv->cap * 2 : 32;
        char **nv = (char **)realloc(sv->v, nc * sizeof(char *));
        if (!nv) return FQ_ERR;
        sv->v = nv;
        sv->cap = nc;
    }
    char *d = fq_strdup(name);
    if (!d) return FQ_ERR;
    sv->v[sv->n++] = d;
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

            /* CLAIM: incoming/<name> -> inflight/<name>__t<token> (원자적 rename).
             * 같은 src를 두 쪽이 rename하면 한 쪽만 성공 → 경합 패배는 ENOENT. */
            char inf_name[640];
            snprintf(inf_name, sizeof(inf_name), "%s__t%llu",
                     name, (unsigned long long)lease->token);

            char src[1408], dst[1408];
            fq_path(src, sizeof(src), q->root, FQ_DIR_INCOMING, name);
            fq_path(dst, sizeof(dst), q->root, FQ_DIR_INFLIGHT, inf_name);

            int rc = fq_fs_rename(src, dst);
            if (rc == FQ_ENOENT) continue;                    /* 경합 패배/이미 처리됨 */
            if (rc != FQ_OK) return FQ_ERR;

            /* rename은 됐는데 읽기/할당이 실패하면 내 token이 찍힌 파일이 inflight에 남는다.
             * 내 token 이상은 복구 대상이 아니라 내가 죽을 때까지 멈추므로 즉시 되돌린다. */
            void *data = NULL; size_t len = 0;
            if (fq_fs_read_file(dst, &data, &len) != FQ_OK) { fq_fs_rename(dst, src); return FQ_ERR; }

            fq_msg *m = (fq_msg *)calloc(1, sizeof(*m));
            if (!m) { free(data); fq_fs_rename(dst, src); return FQ_ERR; }
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

/* ---- 골라서 claim / 되돌려 놓기 ---- */

int fq_claim_if(fq_queue *q, const fq_lease *lease, fq_want_fn want, void *ud, fq_msg **out)
{
    /* 캐시를 쓰지 않고 매번 새로 나열한다: 건너뛴 메시지가 다음 호출에서 다시 후보가 되어야 한다. */
    char inc_dir[1280];
    fq_path(inc_dir, sizeof(inc_dir), q->root, FQ_DIR_INCOMING, NULL);
    strvec sv = {0};
    if (fq_fs_list(inc_dir, collect_cb, &sv) != FQ_OK) { strvec_free(&sv); return FQ_ERR; }
    if (sv.n) qsort(sv.v, sv.n, sizeof(char *), cmp_str);
    claim_cache_clear(q);                               /* fq_claim의 캐시도 무효 */

    int result = FQ_EEMPTY;
    for (size_t i = 0; i < sv.n && result == FQ_EEMPTY; i++) {
        const char *name = sv.v[i];
        char src[1408];
        fq_path(src, sizeof(src), q->root, FQ_DIR_INCOMING, name);
        void *data = NULL; size_t len = 0;
        if (fq_fs_read_file(src, &data, &len) != FQ_OK) continue;   /* 그 사이 사라짐 */
        if (!want(data, len, ud)) { free(data); continue; }

        char base[512];
        uint32_t attempt = 0;
        fq_parse_attempt(name, base, sizeof(base), &attempt);
        char inf_name[640], dst[1408];
        snprintf(inf_name, sizeof(inf_name), "%s__t%llu", name, (unsigned long long)lease->token);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_INFLIGHT, inf_name);
        int rc = fq_fs_rename(src, dst);
        if (rc == FQ_ENOENT) { free(data); continue; }
        if (rc != FQ_OK) { free(data); result = FQ_ERR; break; }

        fq_msg *m = (fq_msg *)calloc(1, sizeof(*m));
        if (!m) { free(data); fq_fs_rename(dst, src); result = FQ_ERR; break; } /* 고아 inflight 방지 */
        snprintf(m->name, sizeof(m->name), "%s", inf_name);
        m->data = data;
        m->len = len;
        m->attempt = attempt;
        *out = m;
        result = FQ_OK;
    }
    strvec_free(&sv);
    return result;
}

int fq_release(fq_queue *q, fq_msg *m)
{
    /* inflight -> incoming 원래 이름 그대로(attempt 변화 없음, 순서 유지) */
    char logical[640];
    fq_parse_inflight(m->name, logical, sizeof(logical));
    char src[1408], dst[1408];
    fq_path(src, sizeof(src), q->root, FQ_DIR_INFLIGHT, m->name);
    fq_path(dst, sizeof(dst), q->root, FQ_DIR_INCOMING, logical);
    int rc = fq_fs_rename(src, dst);
    if (rc != FQ_OK) return rc;                         /* m은 claim 상태 그대로 */
    fsync_incoming(q);
    claim_cache_clear(q);                               /* 되돌린 메시지가 다시 맨 앞 후보 */
    fq_msg_free(m);
    return FQ_OK;
}

/* ---- ack / nack ---- */

int fq_ack(fq_queue *q, fq_msg *m)
{
    char path[1408];
    fq_path(path, sizeof(path), q->root, FQ_DIR_INFLIGHT, m->name);
    int rc = fq_fs_unlink(path);
    fq_msg_free(m);
    return rc;   /* FQ_ENOENT: 이미 회수됨 → 다시 전달될 수 있음(fq.h) */
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
        rc = fq_fs_rename(src, dst);
    } else {
        char req_name[640], dst[1408];
        snprintf(req_name, sizeof(req_name), "%s.a%u", base, attempt + 1);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_INCOMING, req_name);
        rc = fq_fs_rename(src, dst);
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

typedef struct { fq_queue *q; const char *sub; uint64_t cutoff; int removed; } gc_ctx;

static int has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int gc_cb(const char *name, void *ud)
{
    gc_ctx *c = (gc_ctx *)ud;
    /* control/에는 살아 있는 leader.info·election.lock이 있으므로 크래시 잔재 이름만 대상:
     * FS 시각 측정 파일(.now-*), leader.info 교체용 임시 파일(leader-*.tmp), 회수한 stale 락. */
    if (strcmp(c->sub, FQ_DIR_CONTROL) == 0 &&
        !has_prefix(name, ".now-") &&
        !(has_prefix(name, "leader-") && strstr(name, ".tmp")) &&
        !has_prefix(name, "election.lock.stale-"))
        return 0;

    char path[1408];
    fq_path(path, sizeof(path), c->q->root, c->sub, name);

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
    if (fq_fs_now_ms(tmp_dir, q->node_id, &now) != FQ_OK) return FQ_ERR;

    gc_ctx c = { q, FQ_DIR_TMP, now > tmp_max_age_ms ? now - tmp_max_age_ms : 0, 0 };
    if (fq_fs_list(tmp_dir, gc_cb, &c) != FQ_OK) return FQ_ERR;

    char ctrl_dir[1280];
    fq_path(ctrl_dir, sizeof(ctrl_dir), q->root, FQ_DIR_CONTROL, NULL);
    c.sub = FQ_DIR_CONTROL;
    if (fq_fs_list(ctrl_dir, gc_cb, &c) != FQ_OK) return FQ_ERR;
    return c.removed;
}
