/* fq_consumer.c - Active-Passive 소비자 데몬 CLI
 *
 * 사용: fq_consumer <queue_root> <node_id> <logfile> <run_ms>
 *   - 리더십을 획득하면(활성) recover_stale 후 claim/ack 루프로 큐를 소비.
 *     하트비트(리스 갱신)마다 recover_stale을 다시 돈다.
 *   - 소비한 각 메시지의 페이로드를 logfile에 한 줄씩 append (at-least-once 검증용).
 *   - run_ms 동안 동작 후 종료. 리더가 아니면(대기) 짧게 쉬며 재시도.
 *
 * 테스트에서 FQ_LEASE_MS_OVERRIDE 환경변수로 리스를 단축할 수 있다.
 */
#include "fq.h"
#include "fq_internal.h"   /* fq_now_wall_ms, fq_lease_ms */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
#  include <unistd.h>
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <queue_root> <node_id> <logfile> <run_ms>\n", argv[0]);
        return 2;
    }
    const char *root    = argv[1];
    const char *node_id = argv[2];
    const char *logpath = argv[3];
    uint64_t    run_ms  = strtoull(argv[4], NULL, 10);

    fq_queue *q = NULL;
    if (fq_open(root, node_id, &q) != FQ_OK) { fprintf(stderr, "fq_open 실패\n"); return 1; }

    FILE *log = fopen(logpath, "a");
    if (!log) { fprintf(stderr, "logfile 열기 실패\n"); fq_close(q); return 1; }

    /* 하트비트 주기 = 리스/3 (최소 50ms). 리스 만료 전에 갱신되도록. */
    uint64_t renew_iv = fq_lease_ms() / 3;
    if (renew_iv < 50) renew_iv = 50;

    uint64_t deadline   = fq_now_wall_ms() + run_ms;
    fq_lease lease;
    int      have_lead  = 0;
    uint64_t next_renew = 0;
    long     processed  = 0;
    int      became_leader = 0;

    for (;;) {
        uint64_t now = fq_now_wall_ms();
        if (now >= deadline) break;

        /* 대기 노드 → 리더십 획득 시도 */
        if (!have_lead) {
            int rc = fq_acquire_leadership(q, &lease);
            if (rc != FQ_OK) { sleep_ms(30); continue; }  /* 아직 다른 리더 활성 */
            have_lead = 1; became_leader = 1;
            next_renew = now + renew_iv;
            fq_recover_stale(q, &lease);   /* 선임자가 남긴 inflight 회수 */
        }

        /* 활성 리더 → 하트비트 갱신 */
        if (now >= next_renew) {
            int rrc = fq_renew_lease(q, &lease);
            if (rrc == FQ_ELOCKED) { sleep_ms(10); continue; }  /* 선출 락이 잠깐 잡힘 → 곧 재시도 */
            if (rrc != FQ_OK) { have_lead = 0; continue; }       /* 리더십 상실(또는 오류) */
            next_renew = now + renew_iv;
            fq_recover_stale(q, &lease);   /* 좀비가 나중에 claim한 옛 token inflight도 회수 */
        }

        /* claim → 처리 → ack */
        fq_msg *m = NULL;
        int crc = fq_claim(q, &lease, &m);
        if (crc != FQ_OK) { sleep_ms(15); continue; }   /* EEMPTY 포함 */
        fwrite(m->data, 1, m->len, log);
        fputc('\n', log);
        fflush(log);
        fq_ack(q, m);
        processed++;
    }

    fclose(log);
    fq_close(q);
    fprintf(stderr, "[%s] leader=%s processed=%ld\n",
            node_id, became_leader ? "yes" : "no", processed);
    return 0;
}
