/* test_election.c - 리더 선출 경합 하네스 (2프로세스, tests/test_concurrency.sh [C]에서 구동)
 *
 * 사용: test_election <queue_root> A|B <run_ms>
 *   A: 리더십을 얻고 run_ms 동안 하트비트(fq_renew_lease)를 쉬지 않고 반복한다.
 *   B: 조금 뒤에 시작해 run_ms 동안 fq_acquire_leadership을 쉬지 않고 반복한다.
 * A의 리스는 만료되지 않으므로 B의 성공 횟수는 0이어야 한다. 0이 아니면 split-brain.
 * (leader.info를 삭제 후 생성하던 옛 구현에서는 3초에 수십 번 성공했다.)
 * 종료코드: 0 = 정상, 1 = 위반 관측.
 */
#include "fq.h"
#include "fq_internal.h"

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
    if (argc < 4) { fprintf(stderr, "usage: %s <queue_root> A|B <run_ms>\n", argv[0]); return 2; }
    const char *root = argv[1], *role = argv[2];
    uint64_t run_ms = strtoull(argv[3], NULL, 10);

    fq_queue *q = NULL;
    if (fq_open(root, role, &q) != FQ_OK) { fprintf(stderr, "fq_open 실패\n"); return 2; }
    fq_lease l; memset(&l, 0, sizeof l);
    long tries = 0, ok = 0, lost = 0, busy = 0;

    if (strcmp(role, "A") == 0) {
        if (fq_acquire_leadership(q, &l) != FQ_OK) { fprintf(stderr, "A: acquire 실패\n"); return 2; }
        uint64_t end = fq_now_wall_ms() + run_ms;
        /* 갱신은 election.lock 안에서 한다. B가 leader.info를 교체 순간에 읽어 "리더 없음"으로 보면
         * 락을 잡고 재검증한 뒤 양보하는데, 그동안 A의 갱신은 FQ_ELOCKED(나중에 재시도)를 받는다.
         * 이것은 상실이 아니다. 다만 갱신 못 하는 구간이 길어지면 리스가 위험하므로 그 길이를 잰다. */
        uint64_t last_ok = fq_now_wall_ms(), max_gap = 0;
        while (fq_now_wall_ms() < end) {
            tries++;
            int rc = fq_renew_lease(q, &l);
            uint64_t now = fq_now_wall_ms();
            if (rc == FQ_OK) { ok++; last_ok = now; }
            else if (rc == FQ_ELOCKED) busy++;
            else lost++;                                 /* ENOLEADER(상실) 또는 오류 */
            if (now - last_ok > max_gap) max_gap = now - last_ok;
        }
        printf("A: token=%llu renew=%ld ok=%ld busy=%ld lost=%ld max_gap=%llums\n",
               (unsigned long long)l.token, tries, ok, busy, lost, (unsigned long long)max_gap);
        fq_close(q);
        return (lost == 0 && max_gap < 1000) ? 0 : 1;
    }

    sleep_ms(200);                         /* A가 먼저 리더가 되도록 */
    uint64_t end = fq_now_wall_ms() + run_ms;
    while (fq_now_wall_ms() < end) {
        tries++;
        if (fq_acquire_leadership(q, &l) == FQ_OK) ok++;
    }
    printf("B: acquire=%ld stolen=%ld\n", tries, ok);
    fq_close(q);
    return ok == 0 ? 0 : 1;
}
