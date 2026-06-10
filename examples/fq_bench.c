/* fq_bench.c - 처리량 벤치마크
 *
 * 사용: fq_bench <queue_root> <count> <record_size> [mode]
 *   mode: both(기본) | send | recv
 *
 * SEND 단계: record_size 바이트 메시지를 count개 발행하고 송신 msg/s 측정.
 * RECV 단계: 리더십 획득 후 큐가 빌 때까지 claim/ack 하며 수신 msg/s 측정.
 */
#include "fq.h"
#include "fq_internal.h"   /* fq_now_wall_ms */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double rate(long n, double sec) { return sec > 0.0 ? (double)n / sec : 0.0; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <queue_root> <count> <record_size> [both|send|recv]\n", argv[0]);
        return 2;
    }
    const char *root = argv[1];
    long        count = strtol(argv[2], NULL, 10);
    size_t      rec   = (size_t)strtoul(argv[3], NULL, 10);
    const char *mode  = argc > 4 ? argv[4] : "both";
    int do_send = strcmp(mode, "recv") != 0;
    int do_recv = strcmp(mode, "send") != 0;

    char *buf = (char *)malloc(rec ? rec : 1);
    if (!buf) { fprintf(stderr, "oom\n"); return 1; }
    memset(buf, 'x', rec);

    fq_queue *q = NULL;
    if (fq_open(root, "bench", &q) != FQ_OK) { fprintf(stderr, "fq_open(%s) 실패\n", root); return 1; }

    printf("== fq_bench: count=%ld record=%zuB root=%s ==\n", count, rec, root);

    if (do_send) {
        uint64_t t0 = fq_now_wall_ms();
        long sent = 0;
        for (long i = 0; i < count; i++) {
            if (fq_publish(q, buf, rec, NULL) == FQ_OK) sent++;
            if ((i + 1) % 10000 == 0) fprintf(stderr, "  [send] %ld\n", i + 1);
        }
        double s = (fq_now_wall_ms() - t0) / 1000.0;
        printf("SEND: %ld msgs in %.3f s  ->  %.0f msg/s  (%.1f MB/s)\n",
               sent, s, rate(sent, s), rate(sent, s) * rec / (1024.0 * 1024.0));
    }

    if (do_recv) {
        fq_lease lease;
        if (fq_acquire_leadership(q, &lease) != FQ_OK) {
            fprintf(stderr, "리더십 획득 실패\n"); fq_close(q); free(buf); return 1;
        }
        fq_recover_stale(q, &lease);

        uint64_t t0 = fq_now_wall_ms();
        long recv = 0;
        for (;;) {
            fq_msg *m = NULL;
            int rc = fq_claim(q, &lease, &m);
            if (rc != FQ_OK) break;                 /* FQ_EEMPTY 포함 */
            volatile char c = ((char *)m->data)[0];  /* 페이로드 터치 */
            (void)c;
            fq_ack(q, m);
            recv++;
            if (recv % 10000 == 0) fprintf(stderr, "  [recv] %ld\n", recv);
        }
        double s = (fq_now_wall_ms() - t0) / 1000.0;
        printf("RECV: %ld msgs in %.3f s  ->  %.0f msg/s  (%.1f MB/s)\n",
               recv, s, rate(recv, s), rate(recv, s) * rec / (1024.0 * 1024.0));
    }

    free(buf);
    fq_close(q);
    return 0;
}
