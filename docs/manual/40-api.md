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
| `FQ_EINVAL` | -7 | 잘못된 인자 (`node_id` / `stream_key` 형식, [식별자 규칙](#식별자-규칙) 참고) |

> `fq_claim` 도중 다른 소비자에게 메시지를 빼앗기면 내부적으로 `FQ_ENOENT`/`FQ_EEXIST`로
> 표현되지만, 이는 정상 흐름이라 라이브러리가 다음 후보로 넘어간다. 호출자에게는 결국 `FQ_OK`(획득)
> 또는 `FQ_EEMPTY`(없음)만 보인다.

## 튜닝 노브 (컴파일 타임 `#define`)

| 상수 | 기본값 | 의미 |
|---|---|---|
| `FQ_MAX_ATTEMPTS` | 5 | 재시도 한도. 초과 시 `dead/`로 이동 |
| `FQ_LEASE_MS` | 15000 | 리스 유효시간(ms) |
| `FQ_HEARTBEAT_MS` | 3000 | 리더 하트비트 주기(ms). 리스의 1/3~1/5 권장 |
| `FQ_ELECTION_STALE_MS` | 30000 | `election.lock` 강제 회수 임계의 상한(ms). 실제 임계 = min(이 값, 리스/3) |

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
| `fq_open` | `int fq_open(const char *root, const char *node_id, fq_queue **out)` | 큐를 열고 하위 디렉터리를 생성. `node_id`는 이 노드의 식별자(메시지 파일명·리더십 id 겸용). `NULL`이면 `FQ_NODE_ID` 환경변수 → `<호스트명>-<pid>` 순으로 폴백. 형식이 어긋나거나 `root`가 NULL·900자 초과면 `FQ_EINVAL`. |
| `fq_close` | `void fq_close(fq_queue *q)` | 큐 핸들 해제. `fq_consume`으로 얻은 리더십을 쥐고 있으면 반납한다(저수준 API로 얻은 리스는 `fq_release_leadership`으로 직접). |

## 생산자 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_publish` | `int fq_publish(fq_queue *q, const void *data, size_t len, const char *stream_key)` | 메시지를 발행. `stream_key`는 `NULL` 허용(같은 key는 순서 보존 파티셔닝에 사용 가능). tmp→fsync→rename으로 원자적·내구적 발행. `stream_key` 형식이 어긋나면 `FQ_EINVAL`. |

## 소비자 / 리더십 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_acquire_leadership` | `int fq_acquire_leadership(fq_queue *q, fq_lease *lease)` | 리더십 획득 시도(식별자는 `q->node_id` 사용). 유효한 다른 리더가 있으면 `FQ_ELOCKED`. 성공 시 `lease`에 새 token 발급. |
| `fq_renew_lease` | `int fq_renew_lease(fq_queue *q, fq_lease *lease)` | 하트비트. `election.lock` 안에서 여전히 내가 리더인지 확인하고 만료시각 연장. 아니면 `FQ_ENOLEADER`. 선출이 진행 중이라 락이 잡혀 있으면 `FQ_ELOCKED`(리더십은 유지될 수 있음 → 잠시 후 재시도). |
| `fq_recover_stale` | `int fq_recover_stale(fq_queue *q, const fq_lease *lease)` | 옛 token의 inflight(죽은 선임자·좀비 것)를 `incoming/`으로 회수(attempt+1, 한도 초과 시 `dead/`). 인수 직후와 **매 하트비트마다** 호출. |
| `fq_claim` | `int fq_claim(fq_queue *q, const fq_lease *lease, fq_msg **out)` | 가장 오래된 메시지를 점유. 비었으면 `FQ_EEMPTY`. |
| `fq_ack` | `int fq_ack(fq_queue *q, fq_msg *m)` | 처리 완료. inflight 삭제 + `m` 해제. `FQ_ENOENT` = 파일이 이미 없음(리스를 놓친 사이 회수됨 → 이 메시지는 다시 전달된다. 멱등 소비자면 무해). |
| `fq_release_leadership` | `int fq_release_leadership(fq_queue *q, const fq_lease *lease)` | 정상 종료 시 리더십 반납. 아직 내 리스면 만료시각을 0으로 써서 대기 노드가 곧바로 인수한다. `FQ_ENOLEADER` 이미 내 리스 아님, `FQ_ELOCKED` 선출 중(재시도). |
| `fq_nack` | `int fq_nack(fq_queue *q, fq_msg *m)` | 즉시 재큐잉(attempt+1). 한도 초과 시 `dead/`. `m` 해제. |
| `fq_msg_free` | `void fq_msg_free(fq_msg *m)` | ack/nack 없이 메시지 폐기(버퍼 해제). |

> 리더십을 보유한 동안 긴 소비 루프를 돌릴 때는 `FQ_HEARTBEAT_MS` 주기로 `fq_renew_lease`를
> 호출하고, 성공하면 이어서 `fq_recover_stale`도 호출한다. 갱신하지 않으면 리스가 만료되어 대기
> 노드가 리더십을 빼앗아갈 수 있고, 회수를 인수 때만 하면 리스 만료를 모르는 좀비가 그 뒤에 옛
> token으로 claim한 파일이 다음 failover까지 `inflight/`에 멈춰 있다.

### 골라서 소비

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_claim_if` | `int fq_claim_if(fq_queue *q, const fq_lease *lease, fq_want_fn want, void *ud, fq_msg **out)` | `incoming/`을 오래된 순으로 보며 `want(data, len, ud)`가 1을 돌려주는 첫 메시지를 claim(0이면 건너뜀). 후보마다 파일을 읽으므로 `fq_claim`보다 비싸다. 원하는 메시지가 없으면 `FQ_EEMPTY`. |
| `fq_release` | `int fq_release(fq_queue *q, fq_msg *m)` | claim한 메시지를 원래 이름 그대로 `incoming/`에 되돌린다(attempt·순서 유지, 들여다보기 용). 성공하면 `m` 해제. |

```c
typedef int (*fq_want_fn)(const void *data, size_t len, void *ud);
```

## 통합 소비 wrapper

위 소비자/리더십 함수의 일반적 사용 흐름(획득 → `recover_stale` → 하트비트 → `claim`)을 하나로
묶은 편의 함수다. 리더십 상태는 큐 핸들 내부에 보관된다.

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_consume` | `int fq_consume(fq_queue *q, fq_msg **out)` | 리더십 획득/유지·`recover_stale`·하트비트·`claim`을 자동 처리하고 다음 메시지를 반환. 식별자는 `q->node_id` 사용. |
| `fq_consume_if` | `int fq_consume_if(fq_queue *q, fq_want_fn want, void *ud, fq_msg **out)` | `fq_consume`과 같되 `fq_claim_if`로 고른다. |
| `fq_heartbeat` | `int fq_heartbeat(fq_queue *q)` | `fq_consume` 사용자용 하트비트. 주기와 무관하게 즉시 리스를 연장하고 stale inflight를 회수한다. `FQ_OK` 연장됨, `FQ_ELOCKED` 리더가 아님, `FQ_ERR` 오류 또는 선출 락이 잠시 잡혀 있음(리더십 유지, 다시 호출). |

반환값:

| 반환 | 의미 | 호출자 동작 |
|---|---|---|
| `FQ_OK` | `*out`에 메시지 | 처리 후 `fq_ack` / `fq_nack` |
| `FQ_EEMPTY` | 내가 활성 리더이나 큐가 빔 | 잠시 후 재호출 |
| `FQ_ELOCKED` | 다른 노드가 활성 리더(대기 상태) | 잠시 후 재호출 |
| `FQ_ERR` | 오류 | — |

`fq_consume`은 **리스 자체 만료를 스스로 감지**한다. 마지막 성공 갱신 뒤 리스 길이(`FQ_LEASE_MS`)가
지났다면(GC 멈춤, 절전, 긴 처리) 옛 token으로 claim을 계속하지 않고 리더십을 내려놓은 뒤
`election.lock`을 거쳐 다시 얻는다(아무도 인수하지 않았으면 token만 +1). 메시지 하나의 처리가
리스보다 길어질 수 있다면 처리 도중 `fq_heartbeat(q)`를 주기적으로 호출하라.

## 2단계 발행 / 꺼내기 (트랜잭션 큐용)

트랜잭션 관리자가 큐 밖(**같은 파일시스템**)의 파일에 메시지를 보관했다가 결정에 따라 원자적 rename
한 번으로 큐에 넣거나 뺀다. 원본이 옮겨지면 사라지므로 크래시 후 같은 호출을 다시 해도 중복이
생기지 않는다(멱등). 단 POSIX의 `fq_take`는 link+unlink 두 단계라, 그 사이 크래시하면 같은 메시지가
`path`와 `inflight/`에 함께 남고 inflight 쪽은 나중에 복구되어 다시 전달된다(at-least-once 범위의 중복).

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_adopt` | `int fq_adopt(fq_queue *q, const char *path, const char *stream_key)` | 이미 영속된(fsync된) 파일 `path`를 새 메시지로 큐에 넣는다(발행 시각은 지금). `FQ_ENOENT` = `path`가 없음(이미 넣었음). |
| `fq_take` | `int fq_take(fq_queue *q, fq_msg *m, const char *path)` | claim한 `m`을 큐에서 꺼내 `path`로 옮긴다(ack 대신). 성공하면 `m` 해제 + `path` 디렉터리 fsync. 실패하면 `m`은 claim 상태 그대로(`fq_nack` 가능). |
| `fq_return` | `int fq_return(fq_queue *q, const char *path, uint32_t attempt)` | `fq_take`로 꺼낸 파일을 `fq_nack`처럼 되돌린다. `attempt`는 꺼낼 때의 `m->attempt`; attempt+1로 `incoming/`에 넣고 한도에 닿으면 `dead/`. 이름은 새로 붙어(시각 = 지금, stream_key 없음) 원래 순서·파티션은 유지되지 않는다. `FQ_ENOENT` = 이미 되돌렸음. |

## 유지보수 함수

| 함수 | 시그니처 | 설명 |
|---|---|---|
| `fq_gc` | `int fq_gc(fq_queue *q, uint64_t tmp_max_age_ms)` | mtime이 `tmp_max_age_ms`보다 오래된 크래시 잔재 정리: `tmp/`의 모든 파일(발행 중 크래시), `control/`의 `.now-*`(FS 시각 측정), `leader-*.tmp`(leader.info 교체 중), `election.lock.stale-*`. 반환: 삭제 개수(≥0), 오류 시 음수. |

> `tmp_max_age_ms`는 정상 발행·하트비트 한 번보다 충분히 길게(예: 수 분) 준다. 진행 중인 임시
> 파일을 지우면 그 발행이나 하트비트가 실패한다.
