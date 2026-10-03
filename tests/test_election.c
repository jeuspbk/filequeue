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
    long tries = 0, ok = 0, lost = 0;

    if (strcmp(role, "A") == 0) {
        if (fq_acquire_leadership(q, &l) != FQ_OK) { fprintf(stderr, "A: acquire 실패\n"); return 2; }
        uint64_t end = fq_now_wall_ms() + run_ms;
        while (fq_now_wall_ms() < end) {
            tries++;
            int rc = fq_renew_lease(q, &l);
            if (rc == FQ_OK) ok++; else lost++;
        }
        printf("A: token=%llu renew=%ld ok=%ld lost=%ld\n",
               (unsigned long long)l.token, tries, ok, lost);
        fq_close(q);
        return lost == 0 ? 0 : 1;
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
