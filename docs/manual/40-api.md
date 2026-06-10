# API 레퍼런스

API의 **정본은 `include/fq.h`의 선언과 주석**이다. 이 절은 표 형태의 요약과 사용 주의점을 제공한다.

## 반환 코드

대부분의 함수는 `int`를 반환하며, `FQ_OK`(0)이 성공, 음수가 오류다.

| 상수 | 값 | 의미 |
|---|---|---|
| `FQ_OK` | 0 | 성공 |
| `FQ_ERR` | -1 | 일반 오류 (I/O 등) |
| `FQ_EEXIST` | -2 | 대상이 이미 존재 (rename/create 경합) |
| `FQ_ENOENT` | -3 | 대상 없음 (claim 경합에서 패배 등) |
| `FQ_EEMPTY` | -4 | 큐가 비어 있음 |
| `FQ_ELOCKED` | -5 | 락/리더십을 다른 쪽이 보유 |
| `FQ_ENOLEADER` | -6 | 리더십을 잃음 (fencing) |

> `fq_claim` 도중 다른 소비자에게 메시지를 빼앗기면 내부적으로 `FQ_ENOENT`/`FQ_EEXIST`로
> 표현되지만, 이는 정상 흐름이라 라이브러리가 다음 후보로 넘어간다. 호출자에게는 결국 `FQ_OK`(획득)
> 또는 `FQ_EEMPTY`(없음)만 보인다.

## 튜닝 노브 (컴파일 타임 `#define`)

| 상수 | 기본값 | 의미 |
|---|---|---|
| `FQ_MAX_ATTEMPTS` | 5 | 재시도 한도. 초과 시 `dead/`로 이동 |
| `FQ_LEASE_MS` | 15000 | 리스 유효시간(ms) |
| `FQ_HEARTBEAT_MS` | 3000 | 리더 하트비트 주기(ms). 리스의 1/3~1/5 권장 |
| `FQ_ELECTION_STALE_MS` | 30000 | `election.lock` 강제 회수 임계(ms) |

## 데이터 구조

### `fq_lease` — 리더십 리스

```c
typedef struct {
    char     leader_id[64];
    uint64_t token;            /* 단조 증가하는 fencing token */
    uint64_t lease_expiry_ms;  /* FS 시각 기준 만료시각 */
} fq_lease;
```

### `fq_msg` — claim된 메시지

```c
typedef struct {
    char     name[768];   /* inflight 파일명 (root 기준 상대) */
    void    *data;        /* 페이로드 (소유) */
    size_t   len;
    uint32_t attempt;     /* 현재까지 재처리 시도 횟수 */
} fq_msg;
```

`data`는 라이브러리가 소유한다. `fq_ack` / `fq_nack` 호출 시 자동 해제되며, 직접 폐기할 때는
`fq_msg_free`를 쓴다.

## 생애주기 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_open` | `int fq_open(const char *root, const char *node_id, fq_queue **out)` | 큐를 열고 하위 디렉터리를 생성. `node_id`는 이 노드의 식별자(메시지 파일명·리더십 id 겸용). `NULL`이면 `FQ_NODE_ID` 환경변수 → `node-<pid>` 순으로 폴백. |
| `fq_close` | `void fq_close(fq_queue *q)` | 큐 핸들 해제. |

## 생산자 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_publish` | `int fq_publish(fq_queue *q, const void *data, size_t len, const char *stream_key)` | 메시지를 발행. `stream_key`는 `NULL` 허용(같은 key는 순서 보존 파티셔닝에 사용 가능). tmp→fsync→rename으로 원자적·내구적 발행. |

## 소비자 / 리더십 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_acquire_leadership` | `int fq_acquire_leadership(fq_queue *q, fq_lease *lease)` | 리더십 획득 시도(식별자는 `q->node_id` 사용). 유효한 다른 리더가 있으면 `FQ_ELOCKED`. 성공 시 `lease`에 새 token 발급. |
| `fq_renew_lease` | `int fq_renew_lease(fq_queue *q, fq_lease *lease)` | 하트비트. 여전히 내가 리더면 만료시각 연장. 아니면 `FQ_ENOLEADER`. |
| `fq_recover_stale` | `int fq_recover_stale(fq_queue *q, const fq_lease *lease)` | 옛 token의 inflight(죽은 선임자 것)를 `incoming/`으로 회수. 인수 직후 1회 호출 권장. |
| `fq_claim` | `int fq_claim(fq_queue *q, const fq_lease *lease, fq_msg **out)` | 가장 오래된 메시지를 점유. 비었으면 `FQ_EEMPTY`. |
| `fq_ack` | `int fq_ack(fq_queue *q, fq_msg *m)` | 처리 완료. inflight 삭제 + `m` 해제. |
| `fq_nack` | `int fq_nack(fq_queue *q, fq_msg *m)` | 즉시 재큐잉(attempt+1). 한도 초과 시 `dead/`. `m` 해제. |
| `fq_msg_free` | `void fq_msg_free(fq_msg *m)` | ack/nack 없이 메시지 폐기(버퍼 해제). |

> 리더십을 보유한 동안 긴 소비 루프를 돌릴 때는 `FQ_HEARTBEAT_MS` 주기로 `fq_renew_lease`를
> 호출해야 한다. 호출하지 않으면 리스가 만료되어 대기 노드가 리더십을 빼앗아갈 수 있다.

## 통합 소비 wrapper

위 소비자/리더십 함수의 일반적 사용 흐름(획득 → `recover_stale` → 하트비트 → `claim`)을 하나로
묶은 편의 함수다. 리더십 상태는 큐 핸들 내부에 보관된다.

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_consume` | `int fq_consume(fq_queue *q, fq_msg **out)` | 리더십 획득/유지·`recover_stale`·하트비트·`claim`을 자동 처리하고 다음 메시지를 반환. 식별자는 `q->node_id` 사용. |

반환값:

| 반환 | 의미 | 호출자 동작 |
|---|---|---|
| `FQ_OK` | `*out`에 메시지 | 처리 후 `fq_ack` / `fq_nack` |
| `FQ_EEMPTY` | 내가 활성 리더이나 큐가 빔 | 잠시 후 재호출 |
| `FQ_ELOCKED` | 다른 노드가 활성 리더(대기 상태) | 잠시 후 재호출 |
| `FQ_ERR` | 오류 | — |

## 유지보수 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_gc` | `int fq_gc(fq_queue *q, uint64_t tmp_max_age_ms)` | `tmp/`의 고아 임시파일(발행 중 크래시 잔재) 중 `tmp_max_age_ms`보다 오래된 것 정리. 반환: 삭제 개수(≥0), 오류 시 음수. |
