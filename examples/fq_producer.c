/* fq_producer.c - 메시지 발행 CLI
 *
 * 사용: fq_producer <queue_root> <count> [prefix]
 * <prefix><i> 형태의 페이로드를 count개 발행한다 (i = 0..count-1).
 */
#include "fq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <queue_root> <count> [prefix]\n", argv[0]);
        return 2;
    }
    const char *root   = argv[1];
    long        count  = strtol(argv[2], NULL, 10);
    const char *prefix = argc > 3 ? argv[3] : "msg-";

    fq_queue *q = NULL;
    if (fq_open(root, NULL, &q) != FQ_OK) {   /* node_id 불필요(발행만) → NULL 폴백 */
        fprintf(stderr, "fq_open(%s) 실패\n", root);
        return 1;
    }

    long ok = 0;
    char payload[256];
    for (long i = 0; i < count; i++) {
        int n = snprintf(payload, sizeof(payload), "%s%ld", prefix, i);
        if (fq_publish(q, payload, (size_t)n, NULL) == FQ_OK) ok++;
    }

    fq_close(q);
    fprintf(stderr, "[producer] published=%ld/%ld\n", ok, count);
    return ok == count ? 0 : 1;
}
