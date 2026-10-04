/* fq_leader.c - 리더 선출 / 하트비트 / stale 복구 (Active-Passive)
 *
 * leader.info 포맷 (1행 텍스트):  "<leader_id> <token> <lease_expiry_ms> <instance>\n"
 *   instance: 리스를 쓴 핸들의 nonce. 옛 3필드 포맷도 읽는다(instance = "").
 * 시각은 모두 FS 시각(fq_fs_now_ms)을 단일 출처로 사용 → 노드 간 clock skew 회피.
 *
 * leader.info를 쓰는 모든 경로(인수, 하트비트)는 election.lock 안에서 읽고-확인하고-쓴다.
 * 하트비트가 락 없이 쓰면, 읽은 뒤 멈춘 옛 리더가 그 사이 인수한 새 리더의 leader.info를 옛 token으로
 * 덮어써 token이 되돌아가고(같은 token 재발급) split-brain이 생긴다.
 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char     id[64];
    uint64_t token;
    uint64_t expiry;
    char     instance[40];
} leader_info;

/* leader.info 읽기. 없거나 형식이 깨졌으면 모두 0/""(리더 없음).
 * 깨진 파일 때문에 token이 0부터 다시 시작해도, 인수는 inflight에 남은 최대 token보다 큰 값을
 * 발급하므로(token_high_water) 살아 있는 inflight와 token이 겹치지 않는다. */
static int read_leader_info(fq_queue *q, leader_info *li)
{
    memset(li, 0, sizeof(*li));

    char path[1280];
    fq_path(path, sizeof(path), q->root, FQ_DIR_CONTROL, "leader.info");

    void *buf = NULL; size_t len = 0;
    int rc = fq_fs_read_file(path, &buf, &len);
    if (rc == FQ_ENOENT) return FQ_OK;
    if (rc != FQ_OK) return rc;

    char tmp[256];
    size_t cp = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, buf, cp);
    tmp[cp] = '\0';
    free(buf);

    unsigned long long tk = 0, ex = 0;
    char rid[64] = {0}, inst[40] = {0};
    if (sscanf(tmp, "%63s %llu %llu %39s", rid, &tk, &ex, inst) < 3) return FQ_OK;
    snprintf(li->id, sizeof(li->id), "%s", rid);
    snprintf(li->instance, sizeof(li->instance), "%s", inst);
    li->token = tk;
    li->expiry = ex;
    return FQ_OK;
}

/* 이 핸들이 쓴 리스인가. node_id만 비교하면 node_id가 같은 다른 프로세스의 유효한 리스를
 * 내 것으로 보고 빼앗는다(서로 계속 빼앗는 사실상의 Active-Active). */
static int is_mine(const fq_queue *q, const leader_info *li)
{
    return strcmp(li->id, q->node_id) == 0 && strcmp(li->instance, q->instance) == 0;
}

/* leader.info 를 tmp+rename 으로 원자적 교체 */
static int write_leader_info(fq_queue *q, uint64_t token, uint64_t expiry)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s %llu %llu %s\n", q->node_id,
                     (unsigned long long)token, (unsigned long long)expiry, q->instance);

    char tmp_name[256], tmp_path[1408], dst[1280];
    char gid[64];
    fq_gen_id(gid, sizeof(gid));
    snprintf(tmp_name, sizeof(tmp_name), "leader-%s-%s.tmp", q->node_id, gid);
    fq_path(tmp_path, sizeof(tmp_path), q->root, FQ_DIR_CONTROL, tmp_name);
    fq_path(dst, sizeof(dst), q->root, FQ_DIR_CONTROL, "leader.info");

    if (fq_fs_write_sync(tmp_path, line, (size_t)n) != FQ_OK) return FQ_ERR;

    /* 원자적 교체. 삭제→생성 두 단계로 하면 그 사이에 읽는 노드가 "리더 없음(token 0)"을
     * 보고 살아 있는 리더를 밀어낸다. */
    int rc = fq_fs_rename(tmp_path, dst);
    if (rc != FQ_OK) fq_fs_unlink(tmp_path);
    return rc;
}

/* election.lock 획득. now = FS 시각. FQ_OK = 획득, FQ_ELOCKED = 다른 쪽이 보유.
 * 선출 중 크래시로 남은 stale lock은 회수한다. unlink로 치우면 두 노드가 동시에 회수할 때
 * A가 지우고 새로 만든 락을 B가 다시 지워 둘 다 임계 구역에 들어간다. 대신 고유 이름으로
 * rename한다: rename은 한 쪽만 성공하므로 그 노드만 새 락을 만들고, 진 쪽은 양보한다. */
static int election_lock(fq_queue *q, uint64_t now, char *lock_path, size_t n)
{
    fq_path(lock_path, n, q->root, FQ_DIR_CONTROL, "election.lock");

    int rc = fq_fs_create_new(lock_path);
    if (rc == FQ_EEXIST) {
        uint64_t lock_mtime = 0;
        if (fq_fs_mtime_ms(lock_path, &lock_mtime) == FQ_OK &&
            now > lock_mtime + FQ_ELECTION_STALE_MS) {
            char gid[64], stale[1408];
            fq_gen_id(gid, sizeof(gid));
            snprintf(stale, sizeof(stale), "%s.stale-%s-%s", lock_path, q->node_id, gid);
            if (fq_fs_rename(lock_path, stale) == FQ_OK) {
                fq_fs_unlink(stale);
                rc = fq_fs_create_new(lock_path);
            }
        }
        if (rc == FQ_EEXIST) return FQ_ELOCKED;
    }
    return rc == FQ_OK ? FQ_OK : FQ_ERR;
}

static int max_token_cb(const char *name, void *ud)
{
    uint64_t *mx = (uint64_t *)ud;
    char logical[640];
    uint64_t t = fq_parse_inflight(name, logical, sizeof(logical));
    if (t > *mx) *mx = t;
    return 0;
}

/* 발급된 적 있는 가장 큰 token 추정: leader.info의 token과 inflight/에 stamp된 token 중 최댓값.
 * leader.info가 어떤 이유로든(손상, 락 회수 뒤 늦게 도착한 쓰기) 되돌아가도 새 token이
 * 살아 있는 inflight의 token과 겹치지 않게 한다. 겹치면 그 inflight는 `token >= 내 token`이라
 * 회수되지 않고 다음 failover까지 멈춘다. */
static int token_high_water(fq_queue *q, uint64_t cur_token, uint64_t *out)
{
    char inf_dir[1280];
    fq_path(inf_dir, sizeof(inf_dir), q->root, FQ_DIR_INFLIGHT, NULL);
    uint64_t mx = cur_token;
    int rc = fq_fs_list(inf_dir, max_token_cb, &mx);
    if (rc != FQ_OK && rc != FQ_ENOENT) return FQ_ERR;
    *out = mx;
    return FQ_OK;
}

int fq_acquire_leadership(fq_queue *q, fq_lease *lease)
{
    char ctrl[1280];
    fq_path(ctrl, sizeof(ctrl), q->root, FQ_DIR_CONTROL, NULL);

    leader_info cur;
    if (read_leader_info(q, &cur) != FQ_OK) return FQ_ERR;
    int other = cur.id[0] && !is_mine(q, &cur);

    /* 폴링 부하 절감: 대기 노드는 짧은 주기로 여기를 반복한다. 매번 fq_fs_now_ms(파일 생성+stat+삭제)를
     * 하면 공유 스토리지에 초당 수십 번 메타데이터 쓰기가 생긴다. 추정 FS 시각으로도 리스가 유효하면
     * 파일을 만들지 않고 양보한다. 추정이 늦으면 인수가 그만큼(최대 재동기 주기) 늦어질 뿐이고,
     * 추정이 앞서면 아래 실제 FS 시각 판정으로 넘어가므로 안전성은 그대로다. */
    if (other && cur.expiry > fq_fs_clock_est_ms(q))
        return FQ_ELOCKED;

    uint64_t now = 0;
    if (fq_fs_now_ms(ctrl, q->node_id, &now) != FQ_OK) return FQ_ERR;

    /* 유효한 다른 리더가 살아있으면 양보 */
    if (other && cur.expiry > now)
        return FQ_ELOCKED;

    /* 인수 시도: election.lock 을 배타 생성으로 획득 (직렬화) */
    char lock_path[1280];
    int rc = election_lock(q, now, lock_path, sizeof(lock_path));
    if (rc != FQ_OK) return rc;

    /* 임계 구역. 락 밖에서 읽은 값은 낡았을 수 있다: 두 노드가 같은 만료 상태를 읽고 차례로
     * 락을 잡으면, 재확인 없이는 둘 다 같은 token을 쓰고 두 번째가 첫 번째의 유효 리스를
     * 덮어쓴다(split-brain + token 중복). 락 안에서 다시 읽어 재검증한다. */
    uint64_t high = 0;
    if (fq_fs_now_ms(ctrl, q->node_id, &now) != FQ_OK ||
        read_leader_info(q, &cur) != FQ_OK ||
        token_high_water(q, cur.token, &high) != FQ_OK) {
        fq_fs_unlink(lock_path);
        return FQ_ERR;
    }
    if (cur.expiry > now && cur.id[0] && !is_mine(q, &cur)) {
        fq_fs_unlink(lock_path);
        return FQ_ELOCKED;                       /* 그 사이 다른 노드가 인수함 */
    }

    uint64_t new_token = high + 1;
    uint64_t expiry = now + fq_lease_ms();
    int wrc = write_leader_info(q, new_token, expiry);

    fq_fs_unlink(lock_path); /* election.lock 해제 */

    if (wrc != FQ_OK) return FQ_ERR;

    snprintf(lease->leader_id, sizeof(lease->leader_id), "%s", q->node_id);
    lease->token = new_token;
    lease->lease_expiry_ms = expiry;
    return FQ_OK;
}

int fq_renew_lease(fq_queue *q, fq_lease *lease)
{
    char ctrl[1280];
    fq_path(ctrl, sizeof(ctrl), q->root, FQ_DIR_CONTROL, NULL);

    uint64_t now = 0;
    if (fq_fs_now_ms(ctrl, q->node_id, &now) != FQ_OK) return FQ_ERR;

    /* 인수와 같은 락 안에서 읽고-확인하고-쓴다(파일 머리 주석). 락은 대기 노드가 리스 만료를
     * 관측했을 때만 경합하므로, 살아 있는 리더에게는 거의 항상 비어 있다. */
    char lock_path[1280];
    int rc = election_lock(q, now, lock_path, sizeof(lock_path));
    if (rc != FQ_OK) return rc;   /* FQ_ELOCKED: 선출 진행 중 → 잠시 후 재시도 */

    leader_info cur;
    if (read_leader_info(q, &cur) != FQ_OK) { fq_fs_unlink(lock_path); return FQ_ERR; }

    /* 여전히 내가 리더인지(fencing) 확인 */
    if (cur.token != lease->token || !is_mine(q, &cur) ||
        strcmp(cur.id, lease->leader_id) != 0) {
        fq_fs_unlink(lock_path);
        return FQ_ENOLEADER;
    }

    uint64_t expiry = now + fq_lease_ms();
    int wrc = write_leader_info(q, lease->token, expiry);
    fq_fs_unlink(lock_path);
    if (wrc != FQ_OK) return FQ_ERR;

    lease->lease_expiry_ms = expiry;
    return FQ_OK;
}

/* stale inflight 회수용 컨텍스트 */
typedef struct {
    fq_queue       *q;
    const fq_lease *lease;
    int             recovered;
    int             dead;
    int             err;
} recover_ctx;

static int recover_cb(const char *name, void *ud)
{
    recover_ctx *c = (recover_ctx *)ud;

    char logical[640];
    uint64_t token = fq_parse_inflight(name, logical, sizeof(logical));

    /* 현재 리더 token 이상은 살아있는(내) inflight → 건너뜀 */
    if (token >= c->lease->token) return 0;

    /* 죽은 선임자의 것 → 회수 */
    char base[512];
    uint32_t attempt = 0;
    fq_parse_attempt(logical, base, sizeof(base), &attempt);

    char src[1408];
    fq_path(src, sizeof(src), c->q->root, FQ_DIR_INFLIGHT, name);

    int rc;
    if (attempt + 1 >= FQ_MAX_ATTEMPTS) {
        char dst[1408];
        fq_path(dst, sizeof(dst), c->q->root, FQ_DIR_DEAD, logical);
        rc = fq_fs_rename(src, dst);
        if (rc == FQ_OK) c->dead++;
    } else {
        char req[640], dst[1408];
        snprintf(req, sizeof(req), "%s.a%u", base, attempt + 1);
        fq_path(dst, sizeof(dst), c->q->root, FQ_DIR_INCOMING, req);
        rc = fq_fs_rename(src, dst);
        if (rc == FQ_OK) c->recovered++;
    }
    if (rc != FQ_OK && rc != FQ_ENOENT) c->err = rc;
    return 0;
}

int fq_recover_stale(fq_queue *q, const fq_lease *lease)
{
    char inf_dir[1280];
    fq_path(inf_dir, sizeof(inf_dir), q->root, FQ_DIR_INFLIGHT, NULL);

    recover_ctx c = { q, lease, 0, 0, FQ_OK };
    if (fq_fs_list(inf_dir, recover_cb, &c) != FQ_OK) return FQ_ERR;
    return c.err;
}
