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
static fq_msg *new_msg(fq_queue *q, const char *inf_name, void *data, size_t len, uint32_t attempt);

/* root 상한: 하위 경로("<root>/inflight/<이름>__t<token>.a<N>", 이름 최대 약 270자)가 경로 버퍼(1408)에
 * 잘리지 않고 들어가야 한다. snprintf가 조용히 잘라 엉뚱한 경로를 쓰는 일을 막는다. */
#define FQ_ROOT_MAX 900

/* incoming 메시지명의 최대 길이. 이 이름에 ".a<N>"(재시도)와 "__t<token>"(claim, token 최대 20자리)이
 * 붙어도 파일명 한도 255자 안이어야 한다. 넘는 이름을 받아 두면 claim의 rename이 매번 실패해(FQ_ERR)
 * 그 메시지가 큐 맨 앞을 영원히 막는다. 그래서 발행 때 FQ_EINVAL로 거절한다. */
#define FQ_MSG_NAME_MAX (255 - 4 /* .a<NN> */ - 23 /* __t<20자리> */)

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

    /* 경로 전체 한도(네이티브 Win32 260자): 이 노드의 가장 짧은 메시지("<16>-<10>-<node_id>-<id 32>.msg")조차
     * claim 접미사를 붙여 inflight/에 둘 수 없는 루트면 큐로 쓸 수 없다. control/의 가장 긴 이름
     * (election.lock.stale-<node_id>-<id>)도 이보다 짧다. 열어 두면 FS 시각 측정 파일부터 만들지 못해
     * fq_consume이 끝없이 FQ_ERR(일시 오류로 보여 재시도)를 낸다(MinGW 실측: 루트 230자). */
    char probe[1280];
    fq_path(probe, sizeof(probe), q->root, FQ_DIR_INFLIGHT, NULL);
    if (!fq_fs_path_fits(probe, 1 + 65 + strlen(q->node_id) + (255 - FQ_MSG_NAME_MAX))) {
        free(q);
        return FQ_EINVAL;
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
    if (q->lead_held) fq_release_leadership(q, &q->lead_lease);  /* 실패해도 리스 만료로 넘어간다 */
    claim_cache_clear(q);
    /* 아직 들고 있는 메시지는 핸들보다 오래 살 수 있다: 나중의 fq_msg_free가 해제된 핸들을 건드리지 않게 */
    for (size_t i = 0; i < q->held_n; i++) q->held[i]->owner = NULL;
    free(q->held);
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
static void format_msg_name(const fq_queue *q, uint64_t now, uint32_t seq, const char *id, const char *stream_key,
                            char *msg_name, size_t n)
{
    if (stream_key && stream_key[0])
        snprintf(msg_name, n, "%016llu-%010u-%s-%s.%s.msg",
                 (unsigned long long)now, seq, q->node_id, id, stream_key);
    else
        snprintf(msg_name, n, "%016llu-%010u-%s-%s.msg",
                 (unsigned long long)now, seq, q->node_id, id);
}

static void new_msg_name(fq_queue *q, const char *id, const char *stream_key, char *msg_name, size_t n)
{
    uint64_t now = msg_clock_ms(q);
    format_msg_name(q, now, ++q->seq, id, stream_key, msg_name, n);
}

static int ordered_name(fq_queue *q, const char *id, const char *stream_key, const fq_order *o,
                        char *name, size_t n);

/* 이름·경로 한도: claim·재시도 접미사를 붙여도 들어가야 한다. 맞으면 msg_path에 incoming/ 경로 */
static int name_fits(const fq_queue *q, const char *msg_name, char *msg_path, size_t n)
{
    if (strlen(msg_name) > FQ_MSG_NAME_MAX) return FQ_EINVAL;
    fq_path(msg_path, n, q->root, FQ_DIR_INCOMING, msg_name);
    /* 경로 전체 한도(네이티브 Win32 260자)도 같은 이유로 접미사까지 붙여 본다("inflight"는 "incoming"과 길이가 같다) */
    if (!fq_fs_path_fits(msg_path, 255 - FQ_MSG_NAME_MAX)) return FQ_EINVAL;
    return FQ_OK;
}

/* incoming/ 안의 새 메시지 경로. 순서 지정(o)의 대상이 없으면 FQ_EEMPTY, 이름이 너무 길면 FQ_EINVAL. */
static int incoming_path(fq_queue *q, const char *id, const char *stream_key, const fq_order *o,
                         char *msg_path, size_t n)
{
    char msg_name[512];
    int rc = ordered_name(q, id, stream_key, o, msg_name, sizeof(msg_name));
    if (rc != FQ_OK) return rc;
    return name_fits(q, msg_name, msg_path, n);
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
    return fq_publish_ex(q, data, len, stream_key, NULL);
}

int fq_publish_ex(fq_queue *q, const void *data, size_t len, const char *stream_key, const fq_order *order)
{
    if (stream_key && stream_key[0] && !fq_valid_ident(stream_key, FQ_STREAM_KEY_MAX))
        return FQ_EINVAL;
    char id[64];
    fq_gen_id(id, sizeof(id));

    /* 1) 최종 메시지명. 순서 지정이면 대상을 먼저 찾는다: 못 찾으면 아무것도 쓰지 않고 끝 */
    char msg_path[1408];
    int nrc = incoming_path(q, id, stream_key, order, msg_path, sizeof(msg_path));
    if (nrc != FQ_OK) return nrc;

    /* 2) tmp에 기록 + fsync. tmp/는 모든 발행 노드가 공유하므로 이름에 node_id를 넣어
     *    다른 노드와 같은 임시 파일을 덮어쓰는 일(메시지 유실)을 구조적으로 막는다. */
    char tmp_name[192], tmp_path[1408];
    snprintf(tmp_name, sizeof(tmp_name), "%s-%s.tmp", q->node_id, id);
    fq_path(tmp_path, sizeof(tmp_path), q->root, FQ_DIR_TMP, tmp_name);
    int wrc = fq_fs_write_sync(tmp_path, data, len);   /* 실패 시 tmp는 이미 지워짐 */
    if (wrc != FQ_OK) return wrc;                       /* FQ_ENOSPC 또는 FQ_ERR */

    /* 3) 원자적 rename (대상 이름은 구조상 유일) */
    int rc = fq_fs_rename(tmp_path, msg_path);
    if (rc != FQ_OK) { fq_fs_unlink(tmp_path); return rc; }

    /* 4) 디렉터리 영속화 */
    fsync_incoming(q);
    return FQ_OK;
}

/* ---- 배치 발행 (fq.h) ---- */

typedef struct {
    char        msg[1408];   /* incoming/ 최종 경로 */
    char        tmp[1408];   /* tmp/ 임시 경로 */
    const void *data;
    size_t      len;
    int         rc;          /* 기록 결과 */
} batch_ent;

typedef struct { batch_ent *e; size_t n, step; } batch_job;

/* 스레드 k는 k, k+step, k+2*step ... 번째 항목을 기록한다(나눠 맡아 원자적 카운터가 필요 없다) */
static void batch_write(void *arg, size_t k)
{
    batch_job *j = (batch_job *)arg;
    for (size_t i = k; i < j->n; i += j->step)
        j->e[i].rc = fq_fs_write_sync(j->e[i].tmp, j->e[i].data, j->e[i].len);
}

/* 꼬리 발행 이름이 한도 안인가. 시각·seq·id는 고정폭이라 길이는 node_id와 stream_key로만 정해진다 */
static int batch_key_fits(const fq_queue *q, const char *stream_key)
{
    char id[64], name[512], path[1408];
    fq_gen_id(id, sizeof(id));
    format_msg_name(q, 0, 0, id, stream_key, name, sizeof(name));
    return name_fits(q, name, path, sizeof(path));
}

/* 청크 하나(m <= FQ_BATCH_CHUNK건): 이름 → 기록(병렬) → 항목 순서대로 rename → 디렉터리 fsync.
 * 기록이 하나라도 실패하면 이 청크의 tmp를 모두 지우고 0건. *renamed = 발행한 앞쪽 건수. */
static int batch_chunk(fq_queue *q, const fq_item *items, size_t m, batch_ent *e, size_t *renamed)
{
    *renamed = 0;
    /* 1) 이름: 기록 직전에 항목 순서대로(시각은 단조, 같은 ms면 seq가 는다). 이름의 시각과 실제로 보이는
     *    시각의 차이가 청크 하나의 기록 시간으로 묶인다 */
    for (size_t i = 0; i < m; i++) {
        char id[64], tmp_name[192];
        fq_gen_id(id, sizeof(id));
        int rc = incoming_path(q, id, items[i].stream_key, NULL, e[i].msg, sizeof(e[i].msg));
        if (rc != FQ_OK) return rc;                      /* 사전 검사를 통과했으니 오지 않는다 */
        snprintf(tmp_name, sizeof(tmp_name), "%s-%s.tmp", q->node_id, id);
        fq_path(e[i].tmp, sizeof(e[i].tmp), q->root, FQ_DIR_TMP, tmp_name);
        e[i].data = items[i].data;
        e[i].len  = items[i].len;
        e[i].rc   = FQ_ERR;          /* 아무도 기록하지 않은 항목이 성공으로 보이지 않게 */
    }

    /* 2) 기록 + fsync를 겹쳐서. 하나라도 실패하면 이 청크의 tmp를 모두 지우고 끝(큐는 그대로) */
    batch_job job = { e, m, m < FQ_BATCH_THREADS ? m : FQ_BATCH_THREADS };
    fq_run_parallel(job.step, batch_write, &job);
    int err = FQ_OK;
    for (size_t i = 0; i < m; i++)
        if (e[i].rc != FQ_OK) { err = e[i].rc; break; }   /* FQ_ENOSPC 또는 FQ_ERR */
    if (err != FQ_OK) {
        for (size_t i = 0; i < m; i++)
            if (e[i].rc == FQ_OK) fq_fs_unlink(e[i].tmp);  /* 실패한 것은 fq_fs_write_sync가 이미 지웠다 */
        return err;
    }

    /* 3) 항목 순서대로 발행. 도중 실패하면 앞의 것만 발행된 채 나머지 tmp를 지운다 */
    size_t k = 0;
    int rc = FQ_OK;
    for (; k < m; k++) {
        rc = fq_fs_rename(e[k].tmp, e[k].msg);
        if (rc != FQ_OK) break;
    }
    for (size_t i = k; i < m; i++) fq_fs_unlink(e[i].tmp);

    /* 4) 디렉터리 영속화 한 번. 다음 청크로 가기 전에: 오류로 끝나도 *published건은 영속돼 있어야 한다 */
    if (k) fsync_incoming(q);
    *renamed = k;
    return rc;
}

int fq_publish_batch(fq_queue *q, const fq_item *items, size_t n, size_t *published)
{
    if (published) *published = 0;
    if (n == 0) return FQ_OK;
    if (!items) return FQ_EINVAL;

    /* 모든 항목을 먼저 검사: 어긋나면 아무것도 쓰지 않는다(뒤 청크에서 발견하면 앞 청크가 이미 발행됐다).
     * 같은 stream_key가 이어지면 한 번만 본다 */
    const char *seen = NULL;
    int have_seen = 0;
    for (size_t i = 0; i < n; i++) {
        const char *key = (items[i].stream_key && items[i].stream_key[0]) ? items[i].stream_key : NULL;
        if (have_seen && (key == seen || (key && seen && strcmp(key, seen) == 0))) continue;
        if (key && !fq_valid_ident(key, FQ_STREAM_KEY_MAX)) return FQ_EINVAL;
        int vrc = batch_key_fits(q, key);
        if (vrc != FQ_OK) return vrc;
        seen = key;
        have_seen = 1;
    }

    /* FQ_BATCH_CHUNK건씩: 메모리와, tmp 파일이 rename을 기다리는 시간(fq_gc의 나이 기준)을 청크 하나로 묶는다 */
    size_t cap = n < FQ_BATCH_CHUNK ? n : FQ_BATCH_CHUNK;
    batch_ent *e = (batch_ent *)calloc(cap, sizeof(*e));
    if (!e) return FQ_ERR;
    size_t done = 0;
    int rc = FQ_OK;
    while (done < n && rc == FQ_OK) {
        size_t m = n - done < cap ? n - done : cap, k = 0;
        rc = batch_chunk(q, items + done, m, e, &k);
        done += k;
    }
    free(e);
    if (published) *published = done;
    return rc;
}

/* ---- 2단계 발행 / 꺼내기 (트랜잭션 큐용, fq.h 참고) ---- */

int fq_adopt(fq_queue *q, const char *path, const char *stream_key)
{
    return fq_adopt_ex(q, path, stream_key, NULL);
}

int fq_adopt_ex(fq_queue *q, const char *path, const char *stream_key, const fq_order *order)
{
    if (stream_key && stream_key[0] && !fq_valid_ident(stream_key, FQ_STREAM_KEY_MAX))
        return FQ_EINVAL;
    char id[64];
    fq_gen_id(id, sizeof(id));
    char msg_path[1408];
    int nrc = incoming_path(q, id, stream_key, order, msg_path, sizeof(msg_path));
    if (nrc != FQ_OK) return nrc;            /* 대상 없음 등: path는 그대로 */
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
    char dst[1408], req_name[640];
    int dead = attempt >= FQ_MAX_ATTEMPTS - 1;   /* attempt + 1 >= 한도. UINT32_MAX에서 +1이 0으로 넘치지 않게 */
    if (dead) {
        /* nack이 dead/로 보낼 때와 같이 ".a<attempt>"를 남긴다: fq_dead_list가 attempt를 0으로 보여 주지 않게 */
        if (attempt) snprintf(req_name, sizeof(req_name), "%s.a%u", name, attempt);
        else         snprintf(req_name, sizeof(req_name), "%s", name);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_DEAD, req_name);
    } else {
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

/* claim 접미사를 붙이면 이름·경로 한도를 넘는 incoming 메시지(발행 검사 전 버전이 남긴 것, 밖에서 넣은 것)를
 * 같은 이름으로 dead/에 격리한다. "dead"는 "incoming"보다 짧아 그대로 들어간다. 두면 claim이 매번 실패해
 * 큐 맨 앞을 막거나(FQ_ERR), Win32에서는 경합 패배로 보여 끝없이 다시 시도한다.
 * 반환: FQ_OK = 치웠음(또는 그 사이 사라짐), 그 밖 = 오류. */
static int quarantine_incoming(fq_queue *q, const char *name)
{
    char src[1408], dst[1408];
    fq_path(src, sizeof(src), q->root, FQ_DIR_INCOMING, name);
    fq_path(dst, sizeof(dst), q->root, FQ_DIR_DEAD, name);
    int rc = fq_fs_rename(src, dst);
    if (rc == FQ_ENOENT) return FQ_OK;
    if (rc != FQ_OK) return rc;
    char dead_dir[1280];
    fq_path(dead_dir, sizeof(dead_dir), q->root, FQ_DIR_DEAD, NULL);
    fq_fs_fsync_dir(dead_dir);
    return FQ_OK;
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
            if (rc == FQ_EINVAL) {                            /* 접미사를 붙일 수 없는 이름 */
                if (quarantine_incoming(q, name) != FQ_OK) return FQ_ERR;
                continue;
            }
            if (rc != FQ_OK) return FQ_ERR;

            /* 읽기 실패(EACCES·EIO, 일반 파일이 아닌 항목)는 이 메시지의 실패한 시도 한 번으로 친다.
             * 원래 이름으로 되돌리면 attempt가 늘지 않아 영원히 DLQ로 가지 않고, 이름이 가장 오래되어
             * 캐시를 채울 때마다 맨 앞에서 FQ_ERR를 낸다. attempt+1로 보내면 일시 오류는 재시도되고
             * 계속 실패하면 dead/로 격리된다. 큐는 다음 후보로 계속 흐른다. */
            void *data = NULL; size_t len = 0;
            int rrc = fq_fs_read_file(dst, &data, &len);
            if (rrc == FQ_ENOENT) continue;   /* claim 직후 더 새 리더가 회수함(내가 좀비): 경합 패배와 같다 */
            if (rrc != FQ_OK) {
                if (fq_requeue_inflight(q, inf_name, NULL) != FQ_OK) {
                    fq_fs_rename(dst, src);   /* 그것도 안 되면 원래 자리로(내 token의 고아 방지) */
                    return FQ_ERR;
                }
                continue;
            }

            /* 할당 실패는 메시지 탓이 아니므로 그대로 되돌린다(내 token의 고아 방지). */
            fq_msg *m = new_msg(q, inf_name, data, len, attempt);
            if (!m) { free(data); fq_fs_rename(dst, src); return FQ_ERR; }
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
        char base[512];
        uint32_t attempt = 0;
        fq_parse_attempt(name, base, sizeof(base), &attempt);
        char inf_name[640], dst[1408];
        snprintf(inf_name, sizeof(inf_name), "%s__t%llu", name, (unsigned long long)lease->token);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_INFLIGHT, inf_name);

        void *data = NULL; size_t len = 0;
        int rrc = fq_fs_read_file(src, &data, &len);
        if (rrc == FQ_ENOENT) continue;                             /* 그 사이 사라짐 */
        if (rrc != FQ_OK) {
            /* 읽을 수 없는 메시지(권한·I/O 오류, 일반 파일이 아닌 항목): fq_claim과 같이 claim해서 실패한
             * 시도 한 번으로 친다(attempt+1, 한도면 dead/). 건너뛰기만 하면 영원히 남아 매번 다시 읽힌다. */
            int mrc = fq_fs_rename(src, dst);
            if (mrc == FQ_OK && fq_requeue_inflight(q, inf_name, NULL) != FQ_OK)
                fq_fs_rename(dst, src);                             /* 그것도 안 되면 원래 자리로 */
            if (mrc == FQ_EINVAL) quarantine_incoming(q, name);     /* fq_claim과 같이 격리 */
            continue;
        }
        if (!want(data, len, ud)) { free(data); continue; }

        int rc = fq_fs_rename(src, dst);
        if (rc == FQ_ENOENT) { free(data); continue; }
        if (rc == FQ_EINVAL) {                                      /* fq_claim과 같이 격리 */
            free(data);
            if (quarantine_incoming(q, name) != FQ_OK) { result = FQ_ERR; break; }
            continue;
        }
        if (rc != FQ_OK) { free(data); result = FQ_ERR; break; }

        fq_msg *m = new_msg(q, inf_name, data, len, attempt);
        if (!m) { free(data); fq_fs_rename(dst, src); result = FQ_ERR; break; } /* 고아 inflight 방지 */
        *out = m;
        result = FQ_OK;
    }
    strvec_free(&sv);
    return result;
}

/* ---- 순서 지정 이름 (fq.h "순서 지정 발행") ----
 * 이름 = <정렬 키>-<producer>-<id>[.<stream>].msg. 자리는 이름 정렬로만 정한다.
 * 정렬 키 마디 = <16자리>-<10자리>(28자 중 27자 + 구분자).
 *   꼬리  : <ms>-<seq>                                   (new_msg_name)
 *   HEAD  : !<9..9 - ms>-<2^32-1 - seq>   '!'(0x21)는 숫자보다 앞이라 꼬리 모두보다 앞이고, 뒤집은
 *           시각·seq 때문에 나중에 넣은 것이 앞이다.
 *   BEFORE: <대상의 정렬 키>!<ms>-<seq>   대상과는 정렬 키가 끝나는 자리에서 '!' < '-'라 바로 앞이고,
 *           대상보다 앞선 메시지는 그 전 글자에서 이미 더 작다. 같은 대상 앞에 먼저 끼운 것은 시각이
 *           작아 그 앞이다. 끼울 때마다 28자씩만 는다(producer·id는 끝에 한 번).
 * node_id·stream에는 '!'가 없으므로(fq_valid_ident) '!'는 정렬 키 안에서만 쓰인다. */

static int digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* name의 정렬 키 길이(위 형식). 이 큐가 만든 이름이 아니면 0. */
static size_t sort_key_len(const char *name)
{
    size_t len = strlen(name), pos = name[0] == '!' ? 1 : 0;
    for (;;) {
        if (len < pos + 27 || !digits(name + pos, 16) || name[pos + 16] != '-' || !digits(name + pos + 17, 10))
            return 0;
        pos += 27;
        if (name[pos] == '-') return pos;
        if (name[pos] != '!') return 0;
        pos++;
    }
}

/* incoming을 앞에서부터 보며 want가 1인 첫 메시지의 이름. FQ_OK / FQ_EEMPTY / FQ_ERR. */
static int find_target(fq_queue *q, fq_want_fn want, void *ud, char *name, size_t n)
{
    char inc_dir[1280];
    fq_path(inc_dir, sizeof(inc_dir), q->root, FQ_DIR_INCOMING, NULL);
    strvec sv = {0};
    if (fq_fs_list(inc_dir, collect_cb, &sv) != FQ_OK) { strvec_free(&sv); return FQ_ERR; }
    if (sv.n) qsort(sv.v, sv.n, sizeof(char *), cmp_str);
    int result = FQ_EEMPTY;
    for (size_t i = 0; i < sv.n && result == FQ_EEMPTY; i++) {
        char src[1408];
        fq_path(src, sizeof(src), q->root, FQ_DIR_INCOMING, sv.v[i]);
        void *data = NULL; size_t len = 0;
        if (fq_fs_read_file(src, &data, &len) != FQ_OK) continue;   /* 사라짐·읽기 실패: 대상 아님 */
        if (want(data, len, ud)) {
            snprintf(name, n, "%s", sv.v[i]);
            result = FQ_OK;
        }
        free(data);
    }
    strvec_free(&sv);
    return result;
}

static int ordered_name(fq_queue *q, const char *id, const char *stream_key, const fq_order *o,
                        char *name, size_t n)
{
    if (!o || o->mode == FQ_ORDER_TAIL) {
        new_msg_name(q, id, stream_key, name, n);
        return FQ_OK;
    }
    if (o->mode == FQ_ORDER_HEAD) {
        uint64_t inv_ms = 9999999999999999ULL - msg_clock_ms(q);
        uint32_t inv_seq = 4294967295u - ++q->seq;
        if (stream_key && stream_key[0])
            snprintf(name, n, "!%016llu-%010u-%s-%s.%s.msg",
                     (unsigned long long)inv_ms, inv_seq, q->node_id, id, stream_key);
        else
            snprintf(name, n, "!%016llu-%010u-%s-%s.msg", (unsigned long long)inv_ms, inv_seq, q->node_id, id);
        claim_cache_clear(q);                            /* 이 핸들의 캐시가 새 맨 앞을 놓치지 않게 */
        return FQ_OK;
    }
    if (o->mode != FQ_ORDER_BEFORE || !o->before) return FQ_EINVAL;
    char target[512];
    int rc = find_target(q, o->before, o->ud, target, sizeof(target));
    if (rc != FQ_OK) return rc;
    size_t kl = sort_key_len(target);
    if (!kl) return FQ_EINVAL;                           /* 이 큐가 만든 이름이 아님 */
    char tail[512];
    new_msg_name(q, id, stream_key, tail, sizeof(tail)); /* <ms>-<seq>-<producer>-<id>... */
    size_t tl = strlen(tail), total = kl + 1 + tl;
    if (total > FQ_ORDER_NAME_MAX || total >= n) return FQ_EINVAL;
    memcpy(name, target, kl);
    name[kl] = '!';
    memcpy(name + kl + 1, tail, tl + 1);
    claim_cache_clear(q);
    return FQ_OK;
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

int fq_requeue_inflight(fq_queue *q, const char *inflight_name, int *to_dead)
{
    char logical[640];
    fq_parse_inflight(inflight_name, logical, sizeof(logical));
    char base[512];
    uint32_t attempt = 0;
    fq_parse_attempt(logical, base, sizeof(base), &attempt);

    char src[1408], dst[1408];
    fq_path(src, sizeof(src), q->root, FQ_DIR_INFLIGHT, inflight_name);
    int dead = attempt >= FQ_MAX_ATTEMPTS - 1;   /* 이름의 ".a<N>"가 아주 커도 +1이 0으로 넘치지 않게 */
    if (dead) {
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_DEAD, logical);
    } else {
        char req_name[640];
        snprintf(req_name, sizeof(req_name), "%s.a%u", base, attempt + 1);
        fq_path(dst, sizeof(dst), q->root, FQ_DIR_INCOMING, req_name);
    }
    if (to_dead) *to_dead = dead;
    return fq_fs_rename(src, dst);
}

int fq_nack(fq_queue *q, fq_msg *m)
{
    /* inflight -> incoming 으로 되돌림 (attempt+1), 한도 초과 시 dead/.
     * 실패해도 m은 해제된다. 남은 inflight는 이제 아무도 들고 있지 않으므로 다음 하트비트(recover_stale)가
     * 고아로 회수한다. */
    int rc = fq_requeue_inflight(q, m->name, NULL);
    fq_msg_free(m);
    return rc;
}

/* ---- 들고 있는 메시지 추적 ---- */

static fq_msg *new_msg(fq_queue *q, const char *inf_name, void *data, size_t len, uint32_t attempt)
{
    if (q->held_n == q->held_cap) {
        size_t nc = q->held_cap ? q->held_cap * 2 : 8;
        fq_msg **nv = (fq_msg **)realloc(q->held, nc * sizeof(*nv));
        if (!nv) return NULL;
        q->held = nv;
        q->held_cap = nc;
    }
    fq_msg *m = (fq_msg *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    snprintf(m->name, sizeof(m->name), "%s", inf_name);
    m->data = data;
    m->len = len;
    m->attempt = attempt;
    m->owner = q;
    q->held[q->held_n++] = m;
    return m;
}

int fq_is_held(const fq_queue *q, const char *inflight_name)
{
    for (size_t i = 0; i < q->held_n; i++)
        if (strcmp(q->held[i]->name, inflight_name) == 0) return 1;
    return 0;
}

void fq_msg_free(fq_msg *m)
{
    if (!m) return;
    fq_queue *q = m->owner;
    if (q) {
        for (size_t i = 0; i < q->held_n; i++)
            if (q->held[i] == m) { q->held[i] = q->held[--q->held_n]; break; }
    }
    free(m->data);
    free(m);
}

/* ---- DLQ (fq.h) ---- */

/* 운영자가 넘긴 dead/ 항목 이름: 경로 구분자·"."·".."이면 dead/ 밖을 가리킨다. */
static int valid_entry_name(const char *name)
{
    if (!name || !name[0] || strlen(name) > 255) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    return strpbrk(name, "/\\") == NULL;
}

int fq_dead_list(fq_queue *q, fq_dead_fn cb, void *ud)
{
    char dead_dir[1280];
    fq_path(dead_dir, sizeof(dead_dir), q->root, FQ_DIR_DEAD, NULL);
    strvec sv = {0};
    if (fq_fs_list(dead_dir, collect_cb, &sv) != FQ_OK) { strvec_free(&sv); return FQ_ERR; }
    if (sv.n) qsort(sv.v, sv.n, sizeof(char *), cmp_str);
    int rc = FQ_OK;
    for (size_t i = 0; i < sv.n && rc == FQ_OK; i++) {
        char path[1408], base[512];
        uint32_t attempt = 0;
        fq_parse_attempt(sv.v[i], base, sizeof(base), &attempt);
        fq_path(path, sizeof(path), q->root, FQ_DIR_DEAD, sv.v[i]);
        void *data = NULL; size_t len = 0;
        int rrc = fq_fs_read_file(path, &data, &len);
        if (rrc == FQ_ENOENT) continue;                 /* 그 사이 되살렸거나 버림 */
        if (rrc != FQ_OK) { data = NULL; len = 0; }     /* 읽을 수 없어도 보여 준다(버릴 수 있게) */
        rc = cb(sv.v[i], data, len, attempt, ud);
        free(data);
    }
    strvec_free(&sv);
    return rc;
}

int fq_redrive(fq_queue *q, const char *name)
{
    if (!valid_entry_name(name)) return FQ_EINVAL;
    char base[512];
    uint32_t attempt = 0;
    fq_parse_attempt(name, base, sizeof(base), &attempt);   /* attempt 0으로: ".a<N>"를 뗀다 */
    char src[1408], dst[1408];
    fq_path(src, sizeof(src), q->root, FQ_DIR_DEAD, name);
    fq_path(dst, sizeof(dst), q->root, FQ_DIR_INCOMING, base);
    /* claim 접미사를 붙일 수 없는 이름(fq_claim이 격리한 것)은 되돌리면 다시 격리될 뿐이다 */
    if (strlen(base) > FQ_MSG_NAME_MAX || !fq_fs_path_fits(dst, 255 - FQ_MSG_NAME_MAX)) return FQ_EINVAL;
    int rc = fq_fs_rename(src, dst);
    if (rc != FQ_OK) return rc;                          /* FQ_ENOENT: 이미 되살렸거나 버림 */
    fsync_incoming(q);
    claim_cache_clear(q);                                /* 오래된 시각이라 캐시보다 앞일 수 있다 */
    return FQ_OK;
}

int fq_dead_drop(fq_queue *q, const char *name)
{
    if (!valid_entry_name(name)) return FQ_EINVAL;
    char path[1408];
    fq_path(path, sizeof(path), q->root, FQ_DIR_DEAD, name);
    return fq_fs_unlink(path);
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

    /* FS 시각은 파일을 하나 만들어 잰다. 디스크가 가득 차 그 생성이 실패하면(정리가 가장 필요할 때)
     * 추정 FS 시각(마지막으로 잰 오프셋)으로 대신한다. 나이 판정이 조금 부정확할 뿐 안전하다. */
    uint64_t now = 0;
    if (fq_fs_now_ms(tmp_dir, q->node_id, &now) != FQ_OK) now = fq_fs_clock_est_ms(q);

    gc_ctx c = { q, FQ_DIR_TMP, now > tmp_max_age_ms ? now - tmp_max_age_ms : 0, 0 };
    if (fq_fs_list(tmp_dir, gc_cb, &c) != FQ_OK) return FQ_ERR;

    char ctrl_dir[1280];
    fq_path(ctrl_dir, sizeof(ctrl_dir), q->root, FQ_DIR_CONTROL, NULL);
    c.sub = FQ_DIR_CONTROL;
    if (fq_fs_list(ctrl_dir, gc_cb, &c) != FQ_OK) return FQ_ERR;
    return c.removed;
}
