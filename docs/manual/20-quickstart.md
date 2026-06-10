# 빠른 시작

모든 함수는 `include/fq.h` 하나만 인클루드하면 된다. 반환값은 `FQ_OK`(0)이 성공, 음수가 오류다.

## 생산자 (Producer)

큐를 열고 메시지를 발행한다. 발행은 어느 노드/프로세스에서나 동시에 가능하다.

```c
#include "fq.h"
#include <string.h>

int main(void) {
    fq_queue *q = NULL;
    if (fq_open("/shared/myqueue", NULL, &q) != FQ_OK)   /* node_id NULL → 자동 설정 */
        return 1;                       /* 큐 디렉터리 생성/열기 실패 */

    const char *msg = "hello world";
    fq_publish(q, msg, strlen(msg), NULL);   /* stream_key 없이 발행 */

    fq_close(q);
    return 0;
}
```

`fq_publish`는 임시 파일에 기록·fsync한 뒤 `incoming/`으로 원자적 rename한다. 따라서 소비자가
**반쯤 쓰인 메시지를 읽는 일은 없다**.

## 소비자 (Consumer)

filequeue는 Active-Passive다. 소비하려면 먼저 **리더십을 획득**해야 하고, 그 노드만 활성 소비자가
된다. 다른 노드는 대기 상태로 두었다가 리더가 죽으면 인수한다.

```c
#include "fq.h"
#include <stdio.h>

int main(void) {
    fq_queue *q = NULL;
    if (fq_open("/shared/myqueue", "nodeA", &q) != FQ_OK)  /* 이 노드의 식별자 = "nodeA" */
        return 1;

    fq_lease lease;
    if (fq_acquire_leadership(q, &lease) != FQ_OK) {   /* node_id는 q에서 가져옴 */
        /* 다른 노드가 활성 리더 → 대기 노드. 잠시 후 재시도하는 루프로 둔다. */
        fq_close(q);
        return 0;
    }

    fq_recover_stale(q, &lease);        /* 죽은 선임자가 남긴 inflight 회수 */

    fq_msg *m = NULL;
    while (fq_claim(q, &lease, &m) == FQ_OK) {
        /* === 메시지 처리 (반드시 멱등하게) === */
        fwrite(m->data, 1, m->len, stdout);
        fputc('\n', stdout);

        fq_ack(q, m);                   /* 처리 완료 → inflight 삭제 (m도 해제됨) */

        /* 긴 루프라면 주기적으로 리스를 갱신해 리더십을 유지한다 */
        /* fq_renew_lease(q, &lease); */
    }
    /* fq_claim 이 FQ_EEMPTY 를 반환하면 큐가 비었다는 뜻 */

    fq_close(q);
    return 0;
}
```

핵심 흐름은 **claim → 처리 → ack** 다.

- `fq_claim` : `incoming/`의 가장 오래된 메시지를 `inflight/`로 원자적 이동(점유). 경쟁이 있어도
  단 하나만 성공한다.
- `fq_ack` : 처리가 끝나면 `inflight/` 파일을 삭제. 이때 `fq_msg`도 함께 해제된다.
- 처리 중 크래시가 나면 메시지는 `inflight/`에 남고, 다음 리더가 `fq_recover_stale`로 되살린다.

## 더 간단하게: `fq_consume`

위의 리더십 획득 → `recover_stale` → 하트비트 → `claim` 보일러플레이트를 직접 다루기 싫다면,
통합 wrapper `fq_consume`를 쓴다. 리더십/리스 관리는 큐 핸들 내부 상태로 자동 처리되고, 호출자는
다음 메시지만 받는다.

```c
fq_queue *q = NULL;
fq_open("/shared/myqueue", "nodeA", &q);   /* node_id를 여기서 한 번만 지정 */

fq_msg *m = NULL;
for (;;) {
    int rc = fq_consume(q, &m);             /* 획득·복구·하트비트·claim 자동 */
    if (rc == FQ_OK)        { /* 처리(멱등) */ fq_ack(q, m); }
    else if (rc == FQ_ERR)  break;
    else                    sleep_ms(20);   /* FQ_EEMPTY(내가 리더,빔) 또는 FQ_ELOCKED(대기) */
}
```

## 빌드해서 실행하기

완전한 동작 예제는 저장소의 `examples/`에 있다. 이들은 동시성 테스트의 드라이버이기도 하다.

```bash
# 큐에 100건 발행
./build/fq_producer /shared/myqueue 100 "msg-"

# 활성 소비자로 5초 동안 소비 (소비 내용은 out.log 에 기록)
./build/fq_consumer /shared/myqueue nodeA out.log 5000
```

`fq_consumer.c`는 리더십 획득 → `recover_stale` → 하트비트 갱신 → claim/ack 루프를 모두 포함한
실전형 소비 루프를 보여준다. 실제 데몬을 만들 때 출발점으로 삼으면 된다.
