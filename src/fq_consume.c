/* fq_consume.c - fq_acquire_leadership + fq_recover_stale + fq_renew_lease + fq_claim
 *                을 하나로 묶은 통합 소비 wrapper.
 *
 * 상태(리더십 보유 여부·리스·다음 하트비트 시각)는 fq_queue 핸들에 보관된다.
 * Active-Passive 소비 루프의 보일러플레이트를 호출자가 반복하지 않도록 한다.
 */
#include "fq.h"
#include "fq_internal.h"

#include <stdlib.h>

/* 하트비트 주기 = 리스의 1/3 (최소 50ms). 리스 만료 전에 갱신되도록. */
static uint64_t renew_interval_ms(void)
{
    uint64_t iv = fq_lease_ms() / 3;
    return iv < 50 ? 50 : iv;
}

/* 하트비트 1회: 리스 갱신 + 선임자/좀비가 남긴 stale inflight 회수.
 * 복구를 인수 때 한 번만 하면, 그 뒤에 옛 token으로 claim한 좀비의 inflight는 다음 failover까지
 * 방치된다. 하트비트마다 inflight를 한 번 훑어(대개 수 개) 회수한다. */
#define HEARTBEAT_LOCK_RETRIES 5   /* election.lock이 잠깐 잡혀 있을 때 재시도 횟수 (10ms 간격) */

/* 반환: FQ_OK 갱신됨, FQ_ELOCKED 리더십 상실, FQ_EEXIST election.lock이 계속 잡혀 있어 이번엔 못 함
 * (리더십은 아직 유지, 리스 자체 만료 감지가 안전망), FQ_ERR 오류. */
static int heartbeat_now(fq_queue *q, uint64_t now)
{
    int rc = fq_renew_lease(q, &q->lead_lease);
    for (int i = 0; rc == FQ_ELOCKED && i < HEARTBEAT_LOCK_RETRIES; i++) {
        fq_sleep_ms(10);
        rc = fq_renew_lease(q, &q->lead_lease);
    }
    if (rc == FQ_ELOCKED)   return FQ_EEXIST;
    if (rc == FQ_ENOLEADER) { q->lead_held = 0; return FQ_ELOCKED; } /* 리더십 상실 */
    if (rc != FQ_OK)        return FQ_ERR;
    q->lead_last_renew_ms = now;
    q->lead_next_renew_ms = now + renew_interval_ms();
    fq_recover_stale(q, &q->lead_lease);
    return FQ_OK;
}

/* 리더십 획득·갱신. FQ_OK면 claim해도 된다. */
static int lead(fq_queue *q)
{
    uint64_t now = fq_now_wall_ms();

    if (q->lead_held) {
        /* 리스 자체 만료 감지: 마지막 성공 갱신 뒤 리스 길이가 지났다면(GC 멈춤, 절전, 긴 처리)
         * FS상 리스는 만료됐고 다른 노드가 인수했을 수 있다. 다음 하트비트가 ENOLEADER를 받을
         * 때까지 옛 token으로 claim을 계속하는 대신, 즉시 내려놓고 election.lock을 거쳐 다시 얻는다.
         * 아무도 인수하지 않았다면 그대로 되찾는다(token만 +1). */
        if (now >= q->lead_last_renew_ms + fq_lease_ms())
            q->lead_held = 0;
        /* 벽시계가 뒤로 점프해 다음 갱신 시각이 멀어진 경우도 즉시 갱신 */
        else if (now + renew_interval_ms() < q->lead_next_renew_ms)
            q->lead_next_renew_ms = now;
    }

    if (!q->lead_held) {
        /* 대기 상태 → 리더십 획득 시도 */
        int rc = fq_acquire_leadership(q, &q->lead_lease);
        if (rc == FQ_ELOCKED) return FQ_ELOCKED;   /* 다른 노드가 활성 리더 */
        if (rc != FQ_OK)      return FQ_ERR;

        q->lead_held = 1;
        q->lead_last_renew_ms = now;
        q->lead_next_renew_ms = now + renew_interval_ms();
        fq_recover_stale(q, &q->lead_lease);        /* 인수 직후 1회 회수 */
    } else if (now >= q->lead_next_renew_ms) {
        int rc = heartbeat_now(q, now);
        if (rc == FQ_EEXIST) return FQ_OK;   /* 다음 호출에 다시 갱신 (next_renew 그대로) */
        return rc;
    }
    return FQ_OK;
}

int fq_heartbeat(fq_queue *q)
{
    if (!q->lead_held) return FQ_ELOCKED;
    int rc = heartbeat_now(q, fq_now_wall_ms());
    return rc == FQ_EEXIST ? FQ_ERR : rc;    /* 일시적: 리더십은 유지, 다시 호출하면 된다 */
}

int fq_consume(fq_queue *q, fq_msg **out)
{
    int rc = lead(q);
    if (rc != FQ_OK) return rc;
    return fq_claim(q, &q->lead_lease, out);
}

int fq_consume_if(fq_queue *q, fq_want_fn want, void *ud, fq_msg **out)
{
    int rc = lead(q);
    if (rc != FQ_OK) return rc;
    return fq_claim_if(q, &q->lead_lease, want, ud, out);
}
