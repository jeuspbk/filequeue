# 핵심 개념

## 핵심 불변식: 상태 = 디렉터리, 전이 = 원자적 rename

filequeue 전체를 관통하는 단 하나의 원칙이다.

> **메시지의 상태 = 어느 하위 디렉터리에 있는가.
> 모든 상태 전이 = 원자적 rename 또는 unlink.
> 파일 내용을 제자리(in-place) 수정하는 일은 절대 없다.**

이 덕분에 어느 시점에 크래시가 나도 파일은 항상 정확히 한 디렉터리에 **온전한 형태로** 존재한다.
부분적으로 쓰이거나 두 상태에 걸친 메시지는 생기지 않는다. 즉, 크래시 안전성이 코드의 부지런함이
아니라 **구조 자체**로 보장된다(crash-safe by construction).

이 불변식을 깨는 변경(예: 파일을 열어 append, 메타데이터를 별도 파일에 갱신)은 크래시 안전성을
무너뜨리므로 추가하면 안 된다.

## 온디스크 레이아웃

큐 루트(`<root>/`) 아래 디렉터리 위치가 곧 메시지의 상태다.

```
<root>/
  tmp/            발행 중 임시 파일 (스테이징)
  incoming/       발행 완료, 소비 대기
  inflight/       소비자가 점유하여 처리 중
  dead/           DLQ: 재시도 한도를 초과한 메시지
  control/
    leader.info   현재 리더 id, fencing token, 리스 만료시각
    election.lock 리더 선출 직렬화용 배타 생성 mutex
    heartbeat     리더 하트비트 (FS 시각 출처)
```

## 메시지 파일명 규칙

별도의 메타데이터 파일 없이, **상태 정보는 파일명에 인코딩**된다.

```
발행된 메시지:  <epoch_ms>-<seq>-<producer>-<id>[.<stream>].msg
점유(inflight): <메시지명>__t<token>
재시도:         <메시지명>.a<N>
```

| 요소 | 의미 |
|---|---|
| `epoch_ms`-`seq` | 발행 시각 + 시퀀스. 파일명 정렬만으로 best-effort FIFO 순서를 만든다. |
| `producer`-`id` | 노드 간 조율 없이 전역 유일성 보장. |
| `__t<token>` | inflight 점유 시 stamp되는 **fencing token**(리더 토큰). 복구의 판단 기준. |
| `.a<N>` | 재시도 횟수. `FQ_MAX_ATTEMPTS` 초과 시 `dead/`로 이동. |

이 파일명 파싱은 `src/fq_util.c`의 `fq_parse_attempt` / `fq_parse_inflight`에 집중돼 있다.
포맷을 바꾸려면 이 두 함수와 `fq_queue.c`/`fq_leader.c`의 이름 생성부를 함께 고쳐야 한다.

## 메시지 생애주기

```
                fq_publish                fq_claim                 fq_ack
  (생산자)  ───────────────▶  incoming/  ──────────▶  inflight/  ──────────▶  (삭제)
                  ▲                                        │
                  │            fq_nack / fq_recover_stale  │  (처리 실패 또는 소비자 크래시)
                  └────────────────────────────────────────┘
                         attempt+1 로 재큐잉 (한도 초과 시 dead/)
```

- **발행**: `tmp/`에 기록 + fsync → `incoming/`으로 원자적 rename → 디렉터리 fsync.
- **소비**: `incoming/` → `inflight/` rename으로 점유(CLAIM) → 처리 → unlink로 ACK.
- **실패/복구**: CLAIM~ACK 사이 크래시 시 파일이 `inflight/`에 남고, 다음 리더가
  `fq_recover_stale`로 `incoming/`에 되돌린다(재처리). 이것이 at-least-once의 근거다.

크래시 지점별 정확한 결과는 [Failover 운영 > 크래시 지점별 결과](#크래시-지점별-결과)의 표를 참고.
