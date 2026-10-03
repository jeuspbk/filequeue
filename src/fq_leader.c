/* fq_leader.c - 리더 선출 / 하트비트 / stale 복구 (Active-Passive)
 *
 * leader.info 포맷 (1행 텍스트):  "<leader_id> <token> <lease_expiry_ms>\n"
 * 시각은 모두 FS 시각(fq_fs_now_ms)을 단일 출처로 사용 → 노드 간 clock skew 회피.
 */
#include "fq.h"
#include "fq_fs.h"
#include "fq_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 유효 리스 시간(ms). 테스트에서 FQ_LEASE_MS_OVERRIDE로 단축 가능. */
static uint64_t fq_lease_ms(void)
{
    const char *e = getenv("FQ_LEASE_MS_OVERRIDE");
    if (e && e[0]) {
        unsigned long long v = strtoull(e, NULL, 10);
        if (v > 0) return (uint64_t)v;
    }
    return FQ_LEASE_MS;
}

/* leader.info 읽기. 없으면 token=0, expiry=0, id="" 반환. */
static int read_leader_info(fq_queue *q, char *id, size_t id_n,
                            uint64_t *token, uint64_t *expiry)
{
    char path[1280];
    fq_path(path, sizeof(path), q->root, FQ_DIR_CONTROL, "leader.info");

    void *buf = NULL; size_t len = 0;
    int rc = fq_fs_read_file(path, &buf, &len);
    if (rc == FQ_ENOENT) { id[0] = '\0'; *token = 0; *expiry = 0; return FQ_OK; }
    if (rc != FQ_OK) return rc;

    char tmp[256];
    size_t cp = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, buf, cp);
    tmp[cp] = '\0';
    free(buf);

    unsigned long long tk = 0, ex = 0;
    char rid[64] = {0};
    if (sscanf(tmp, "%63s %llu %llu", rid, &tk, &ex) < 3) {
        id[0] = '\0'; *token = 0; *expiry = 0; return FQ_OK;
    }
    snprintf(id, id_n, "%s", rid);
    *token = tk;
    *expiry = ex;
    return FQ_OK;
}

/* leader.info 를 tmp+rename 으로 원자적 교체 */
static int write_leader_info(fq_queue *q, const char *id,
                             uint64_t token, uint64_t expiry)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s %llu %llu\n",
                     id, (unsigned long long)token, (unsigned long long)expiry);

    char tmp_name[256], tmp_path[1408], dst[1280];
    char gid[64];
    fq_gen_id(gid, sizeof(gid));
    snprintf(tmp_name, sizeof(tmp_name), "leader-%s-%s.tmp", q->node_id, gid);
    fq_path(tmp_path, sizeof(tmp_path), q->root, FQ_DIR_CONTROL, tmp_name);
    fq_path(dst, sizeof(dst), q->root, FQ_DIR_CONTROL, "leader.info");

    if (fq_fs_write_sync(tmp_path, line, (size_t)n) != FQ_OK) return FQ_ERR;

    /* 원자적 교체. 삭제→생성 두 단계로 하면 그 사이에 읽는 노드가 "리더 없음(token 0)"을
     * 보고 살아 있는 리더를 밀어낸다(하트비트는 election.lock 없이 여기를 지나므로 치명적). */
    int rc = fq_fs_rename(tmp_path, dst);
    if (rc != FQ_OK) fq_fs_unlink(tmp_path);
    return rc;
}

int fq_acquire_leadership(fq_queue *q, fq_lease *lease)
{
    const char *node_id = q->node_id;

    char ctrl[1280];
    fq_path(ctrl, sizeof(ctrl), q->root, FQ_DIR_CONTROL, NULL);

    uint64_t now = 0;
    if (fq_fs_now_ms(ctrl, &now) != FQ_OK) return FQ_ERR;

    char cur_id[64]; uint64_t cur_token = 0, cur_expiry = 0;
    if (read_leader_info(q, cur_id, sizeof(cur_id), &cur_token, &cur_expiry) != FQ_OK)
        return FQ_ERR;

    /* 유효한 다른 리더가 살아있으면 양보 */
    if (cur_expiry > now && cur_id[0] && strcmp(cur_id, node_id) != 0)
        return FQ_ELOCKED;

    /* 인수 시도: election.lock 을 배타 생성으로 획득 (직렬화) */
    char lock_path[1280];
    fq_path(lock_path, sizeof(lock_path), q->root, FQ_DIR_CONTROL, "election.lock");

    int rc = fq_fs_create_new(lock_path);
    if (rc == FQ_EEXIST) {
        /* 선출 중 크래시로 남은 stale lock 회수. unlink로 치우면 두 노드가 동시에 회수할 때
         * A가 지우고 새로 만든 락을 B가 다시 지워 둘 다 임계 구역에 들어간다. 대신 고유 이름으로
         * rename한다: rename은 한 쪽만 성공하므로 그 노드만 새 락을 만들고, 진 쪽은 양보한다. */
        uint64_t lock_mtime = 0;
        if (fq_fs_mtime_ms(lock_path, &lock_mtime) == FQ_OK &&
            now > lock_mtime + FQ_ELECTION_STALE_MS) {
            char gid[64], stale[1408];
            fq_gen_id(gid, sizeof(gid));
            snprintf(stale, sizeof(stale), "%s.stale-%s", lock_path, gid);
            if (fq_fs_rename(lock_path, stale) == FQ_OK) {
                fq_fs_unlink(stale);
                rc = fq_fs_create_new(lock_path);
            }
        }
    }
    if (rc != FQ_OK) return FQ_ELOCKED;

    /* 임계 구역. 락 밖에서 읽은 값은 낡았을 수 있다: 두 노드가 같은 만료 상태를 읽고 차례로
     * 락을 잡으면, 재확인 없이는 둘 다 같은 token을 쓰고 두 번째가 첫 번째의 유효 리스를
     * 덮어쓴다(split-brain + token 중복). 락 안에서 다시 읽어 재검증한다. */
    if (fq_fs_now_ms(ctrl, &now) != FQ_OK ||
        read_leader_info(q, cur_id, sizeof(cur_id), &cur_token, &cur_expiry) != FQ_OK) {
        fq_fs_unlink(lock_path);
        return FQ_ERR;
    }
    if (cur_expiry > now && cur_id[0] && strcmp(cur_id, node_id) != 0) {
        fq_fs_unlink(lock_path);
        return FQ_ELOCKED;                       /* 그 사이 다른 노드가 인수함 */
    }

    uint64_t new_token = cur_token + 1;
    uint64_t expiry = now + fq_lease_ms();
    int wrc = write_leader_info(q, node_id, new_token, expiry);

    fq_fs_unlink(lock_path); /* election.lock 해제 */

    if (wrc != FQ_OK) return FQ_ERR;

    snprintf(lease->leader_id, sizeof(lease->leader_id), "%s", node_id);
    lease->token = new_token;
    lease->lease_expiry_ms = expiry;
    return FQ_OK;
}

int fq_renew_lease(fq_queue *q, fq_lease *lease)
{
    char ctrl[1280];
    fq_path(ctrl, sizeof(ctrl), q->root, FQ_DIR_CONTROL, NULL);

    uint64_t now = 0;
    if (fq_fs_now_ms(ctrl, &now) != FQ_OK) return FQ_ERR;

    char cur_id[64]; uint64_t cur_token = 0, cur_expiry = 0;
    if (read_leader_info(q, cur_id, sizeof(cur_id), &cur_token, &cur_expiry) != FQ_OK)
        return FQ_ERR;

    /* 여전히 내가 리더인지(fencing) 확인 */
    if (cur_token != lease->token || strcmp(cur_id, lease->leader_id) != 0)
        return FQ_ENOLEADER;

    uint64_t expiry = now + fq_lease_ms();
    if (write_leader_info(q, lease->leader_id, lease->token, expiry) != FQ_OK)
        return FQ_ERR;

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
