/* fq_consume.c - fq_acquire_leadership + fq_recover_stale + fq_renew_lease + fq_claim
 *                을 하나로 묶은 통합 소비 wrapper.
 *
 * 상태(리더십 보유 여부·리스·다음 하트비트 시각)는 fq_queue 핸들에 보관된다.
 * Active-Passive 소비 루프의 보일러플레이트를 호출자가 반복하지 않도록 한다.
 */
#include "fq.h"
#include "fq_internal.h"

#include <stdlib.h>

/* 유효 리스 시간(ms). fq_leader.c 와 동일하게 FQ_LEASE_MS_OVERRIDE 를 존중한다. */
static uint64_t eff_lease_ms(void)
{
    const char *e = getenv("FQ_LEASE_MS_OVERRIDE");
    if (e && e[0]) {
        unsigned long long v = strtoull(e, NULL, 10);
        if (v > 0) return (uint64_t)v;
    }
    return FQ_LEASE_MS;
}

/* 하트비트 주기 = 리스의 1/3 (최소 50ms). 리스 만료 전에 갱신되도록. */
static uint64_t renew_interval_ms(void)
{
    uint64_t iv = eff_lease_ms() / 3;
    return iv < 50 ? 50 : iv;
}

int fq_consume(fq_queue *q, fq_msg **out)
{
    uint64_t now = fq_now_wall_ms();

    if (!q->lead_held) {
        /* 대기 상태 → 리더십 획득 시도 */
        int rc = fq_acquire_leadership(q, &q->lead_lease);
        if (rc == FQ_ELOCKED) return FQ_ELOCKED;   /* 다른 노드가 활성 리더 */
        if (rc != FQ_OK)      return FQ_ERR;

        q->lead_held = 1;
        q->lead_next_renew_ms = now + renew_interval_ms();
        fq_recover_stale(q, &q->lead_lease);        /* 인수 직후 1회 회수 */
    } else if (now >= q->lead_next_renew_ms) {
        /* 활성 리더 → 하트비트 갱신 */
        int rc = fq_renew_lease(q, &q->lead_lease);
        if (rc == FQ_ENOLEADER) { q->lead_held = 0; return FQ_ELOCKED; } /* 리더십 상실 */
        if (rc != FQ_OK)        return FQ_ERR;
        q->lead_next_renew_ms = now + renew_interval_ms();
    }

    return fq_claim(q, &q->lead_lease, out);
}
