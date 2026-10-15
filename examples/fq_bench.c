/* fq_bench.c - 처리량 벤치마크
 *
 * 사용: fq_bench <queue_root> <count> <record_size> [mode] [batch]
 *   mode: both(기본) | send | recv
 *   batch: 송신을 fq_publish_batch로, 수신을 fq_consume_batch + fq_ack_batch로 이만큼씩 묶는다
 *          (기본 1 = fq_publish, fq_consume + fq_ack)
 *
 * SEND 단계: record_size 바이트 메시지를 count개 발행하고 송신 msg/s 측정.
 * RECV 단계: 큐가 빌 때까지 fq_consume/ack 하며 수신 msg/s 측정. 실제 소비 루프와 같이 리더십 획득·
 *            하트비트(리스/3마다)·stale 회수를 fq_consume이 처리하므로 그 비용도 수치에 들어간다.
 *            (저수준 claim 루프는 하트비트 없이 리스보다 오래 돌면 리스가 만료된 채 claim하게 된다.)
 */
#include "fq.h"
#include "fq_internal.h"   /* fq_now_mono_ms, fq_sleep_ms */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double rate(long n, double sec) { return sec > 0.0 ? (double)n / sec : 0.0; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <queue_root> <count> <record_size> [both|send|recv] [batch]\n", argv[0]);
        return 2;
    }
    const char *root = argv[1];
    long        count = strtol(argv[2], NULL, 10);
    size_t      rec   = (size_t)strtoul(argv[3], NULL, 10);
    const char *mode  = argc > 4 ? argv[4] : "both";
    int do_send = strcmp(mode, "recv") != 0;
    int do_recv = strcmp(mode, "send") != 0;
    long batch = argc > 5 ? strtol(argv[5], NULL, 10) : 1;
    if (batch < 1) batch = 1;

    char *buf = (char *)malloc(rec ? rec : 1);
    if (!buf) { fprintf(stderr, "oom\n"); return 1; }
    memset(buf, 'x', rec);

    fq_queue *q = NULL;
    if (fq_open(root, "bench", &q) != FQ_OK) { fprintf(stderr, "fq_open(%s) 실패\n", root); return 1; }

    printf("== fq_bench: count=%ld record=%zuB batch=%ld root=%s ==\n", count, rec, batch, root);
    int failed = 0;

    if (do_send) {
        uint64_t t0 = fq_now_mono_ms();
        long sent = 0, err = 0;
        int last = FQ_OK;
        if (batch == 1) {
            for (long i = 0; i < count; i++) {
                int rc = fq_publish(q, buf, rec, NULL);
                if (rc == FQ_OK) sent++; else { err++; last = rc; }
                if ((i + 1) % 10000 == 0) fprintf(stderr, "  [send] %ld\n", i + 1);
            }
        } else {
            fq_item *it = (fq_item *)malloc((size_t)batch * sizeof(*it));
            if (!it) { fprintf(stderr, "oom\n"); return 1; }
            for (long k = 0; k < batch; k++) { it[k].data = buf; it[k].len = rec; it[k].stream_key = NULL; }
            for (long i = 0; i < count; i += batch) {
                size_t n = (size_t)(count - i < batch ? count - i : batch), pub = 0;
                int rc = fq_publish_batch(q, it, n, &pub);
                sent += (long)pub;
                if (rc != FQ_OK) { err += (long)(n - pub); last = rc; }
            }
            free(it);
        }
        double s = (fq_now_mono_ms() - t0) / 1000.0;
        printf("SEND: %ld msgs in %.3f s  ->  %.0f msg/s  (%.2f MB/s)\n",
               sent, s, rate(sent, s), rate(sent, s) * rec / (1024.0 * 1024.0));
        if (err) { printf("SEND 실패: %ld건 (마지막 rc=%d)\n", err, last); failed = 1; }
    }

    if (do_recv && batch > 1) {
        /* 묶음 수신: fq_consume_batch + fq_ack_batch. 리더십 획득까지 기다리는 것은 아래와 같다 */
        fq_msg **ms = (fq_msg **)malloc((size_t)batch * sizeof(*ms));
        if (!ms) { fprintf(stderr, "oom\n"); return 1; }
        size_t got = 0;
        int rc;
        while ((rc = fq_consume_batch(q, ms, (size_t)batch, &got)) == FQ_ELOCKED || rc == FQ_ERR) fq_sleep_ms(50);

        uint64_t t0 = fq_now_mono_ms();
        long recv = 0, err = 0;
        for (;;) {
            if (rc == FQ_OK) {
                for (size_t k = 0; k < got; k++) {
                    volatile char c = ms[k]->len ? ((char *)ms[k]->data)[0] : 0;  /* 페이로드 터치 */
                    (void)c;
                }
                fq_ack_batch(q, ms, got);
                recv += (long)got;
            } else if (rc == FQ_EEMPTY) {
                break;
            } else {
                if (++err > 100) break;
                fq_sleep_ms(10);
            }
            rc = fq_consume_batch(q, ms, (size_t)batch, &got);
        }
        double s = (fq_now_mono_ms() - t0) / 1000.0;
        printf("RECV: %ld msgs in %.3f s  ->  %.0f msg/s  (%.2f MB/s)\n",
               recv, s, rate(recv, s), rate(recv, s) * rec / (1024.0 * 1024.0));
        if (err) { printf("RECV 오류·대기: %ld회\n", err); failed = 1; }
        free(ms);
    } else if (do_recv) {
        /* 리더십 획득까지(다른 리더의 리스가 남아 있으면 만료까지) 기다린 뒤 잰다 */
        fq_msg *m = NULL;
        int rc;
        while ((rc = fq_consume(q, &m)) == FQ_ELOCKED || rc == FQ_ERR) fq_sleep_ms(50);

        uint64_t t0 = fq_now_mono_ms();
        long recv = 0, err = 0;
        for (;;) {
            if (rc == FQ_OK) {
                volatile char c = m->len ? ((char *)m->data)[0] : 0;  /* 페이로드 터치 */
                (void)c;
                fq_ack(q, m);
                if (++recv % 10000 == 0) fprintf(stderr, "  [recv] %ld\n", recv);
            } else if (rc == FQ_EEMPTY) {
                break;
            } else {                                /* FQ_ERR(일시적일 수 있음)·리더십 상실 */
                if (++err > 100) break;
                fq_sleep_ms(10);
            }
            rc = fq_consume(q, &m);
        }
        double s = (fq_now_mono_ms() - t0) / 1000.0;
        printf("RECV: %ld msgs in %.3f s  ->  %.0f msg/s  (%.2f MB/s)\n",
               recv, s, rate(recv, s), rate(recv, s) * rec / (1024.0 * 1024.0));
        if (err) { printf("RECV 오류·대기: %ld회\n", err); failed = 1; }
    }

    free(buf);
    fq_close(q);                                    /* fq_consume으로 얻은 리더십 반납 */
    return failed;
}
