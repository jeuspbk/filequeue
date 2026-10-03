# filequeue 설계 문서

공유 디스크 기반의 통신용 파일 큐. 하나의 파일시스템 디렉터리를 큐로 사용하여
프로세스/노드 간 메시지를 주고받고, 공유 스토리지를 통해 Active-Passive failover를 제공한다.

## 확정 전제

| 항목 | 결정 | 비고 |
|---|---|---|
| 구현 언어 | **C** | Windows 환경 → Win32 API 중심, POSIX 호환 계층 분리 |
| Failover | **Active-Passive** | 활성 소비자 1개, 리스(lease) 기반 인수 |
| 전달 보장 | **At-least-once** | 유실 없음, 중복 가능 → 소비자 멱등(idempotent) 처리 전제 |
| 공유 디스크 | 종류 미상 / 일반 블록 스토리지 | 고급 FS 기능에 의존하지 않는 보수적 설계 |

---

## 0. 토폴로지와 의존하는 원자성

"공유 블록 스토리지 + Active-Passive"는 보통 둘 중 하나로 배치된다. 설계가 갈리므로 명시한다.

| 토폴로지 | 설명 | 동시 접근 | 필요한 보호 |
|---|---|---|---|
| **A. 단일 마운트 (기본 가정)** | SAN LUN/클러스터 디스크를 활성 노드만 마운트, 장애 시 fencing 후 대기 노드가 remount | 정상 시 노드 1개만 FS 접근 | 크래시 일관성만 필요 (노드 간 락 불필요) |
| **B. 동시 마운트** | NFS/SMB/클러스터 FS를 여러 노드가 동시에 RW 마운트 | 다중 노드 동시 접근 | 노드 간 원자적 rename + 락 의미 모두 필요 |

> 일반 블록 스토리지(예: SAN LUN)는 클러스터 FS 없이 여러 노드가 동시 RW 마운트하면 FS가 깨진다.
> 따라서 Active-Passive에서는 거의 항상 **토폴로지 A**이다. 본 설계는 A를 기본으로 하되,
> B에서도 동작하도록 "오직 atomic rename + atomic create에만 의존"하는 보수적 설계를 채택한다.

### 의존하는 파일시스템 원자성 (목표 스토리지에서 반드시 검증)

1. **같은 볼륨 내 `rename`은 원자적** ← 가장 핵심. (NTFS / 대부분 POSIX FS 보장)
   큐 내부 전이는 교체형 `rename`(POSIX `rename(2)`, Win32 `MOVEFILE_REPLACE_EXISTING`)만 쓴다.
   대상 이름이 구조상 유일하므로 "덮어쓰지 않음"은 필요 없고, 같은 src를 두 쪽이 rename하면
   정확히 한 쪽만 성공한다는 성질만 필요하다. POSIX에서 fail-if-exists를 흉내 내는 link+unlink는
   두 단계라 이 성질을 깨므로 내부 전이에 쓰지 않는다
2. **`O_EXCL` / `CREATE_NEW` 배타 생성은 원자적** ← 리더 선출 mutex
3. (선택) OS 권고 락(`LockFileEx` / `fcntl`)의 노드 간 신뢰성 ← 있으면 최적화, 없어도 동작

### 핵심 설계 원칙

> **메시지의 상태 = 어느 디렉터리에 있는가.
> 모든 상태 전이 = 원자적 rename 또는 unlink.
> 파일 내용을 제자리 수정(in-place)하는 일은 절대 없음.**

어느 시점에 크래시가 나도 파일은 항상 정확히 한 디렉터리에 온전한 형태로 존재한다.
→ 구조적으로 크래시 안전(crash-safe by construction).

---

## 1. 온디스크 레이아웃

```
<queue_root>/
  tmp/            # 쓰기 스테이징 (atomic publish 전 임시 파일)
  incoming/       # 발행 완료, 소비 대기 (producer가 rename으로 투입)
  inflight/       # 활성 소비자가 claim하여 처리 중
  dead/           # DLQ: N회 재시도 실패한 poison 메시지
  control/
    election.lock # 리더 선출용 배타 생성 mutex (CREATE_NEW)
    leader.info   # 현재 리더 id, fencing token, 리스 만료시각
    heartbeat     # 리더가 주기적 갱신 (시각 단일 출처로 mtime 활용)
```

### 메시지 파일명

```
<epoch_ms>-<seq>-<producer_id>-<uuid>.msg
예) 0001717920000123-000042-nodeA-3f9c1e7b.msg
```

- 앞쪽 시간 + seq → 파일명 정렬만으로 best-effort FIFO. 시간은 로컬 벽시계가 아니라
  공유 FS 시각 기준 보정값(§4-4)
- producer_id + uuid → 노드 간 조율 없이 전역 유일성 보장

inflight 파일명에는 소유권/펜싱 정보를 추가로 stamp 한다.

```
<원래이름>.msg__<token>__<attempt>
```

- `token`   : 리더의 fencing token (좀비 리더 식별용)
- `attempt` : 재처리 시도 횟수 (DLQ 이동 판단용)

---

## 2. 발행 프로토콜 (Producer) — torn 메시지 방지

```
1. payload를 tmp/<uuid>.tmp 에 기록
2. 파일 fsync                         # FlushFileBuffers: 내용 영속화
3. tmp/... → incoming/<name>.msg      # 원자적 rename (대상 유니크)
4. (POSIX) 디렉터리 fsync             # rename 영속화
```

독자는 `incoming/`만 스캔하므로 rename 완료 전에는 파일이 보이지 않는다.
→ **부분 기록 파일을 절대 읽지 않음.**

---

## 3. 소비 프로토콜 (활성 Consumer) — At-least-once

```
1. SCAN   : incoming/ 나열 → 가장 오래된 파일 선택 (이름 정렬)
2. CLAIM  : rename incoming/<name>.msg
                   → inflight/<name>.msg__<token>__<attempt>
            - rename은 원자적이라, 경쟁해도 단 하나만 성공
            - 실패(파일 없음) → 다음 파일로
3. PROCESS: 메시지 처리 (소비자는 반드시 멱등)
4. ACK    : inflight 파일 unlink → 더 이상 재처리 안 됨
```

### 크래시 지점별 결과

| 크래시 시점 | 디스크 상태 | 결과 | At-least-once |
|---|---|---|---|
| 발행 중 (tmp 단계) | tmp/에 .tmp 잔존 | GC가 정리, 미발행 | OK (미투입) |
| CLAIM 직후 | inflight/에 잔존 | 복구 시 재처리 | OK (중복 가능) |
| PROCESS 중 | inflight/에 잔존 | 복구 시 재처리 | OK (중복 가능) |
| ACK 직전 | inflight/에 잔존 | 복구 시 재처리 → 중복 | OK (멱등 필요) |
| ACK 후 | 파일 없음 | 완료 | OK |

→ 유실은 어느 경우에도 없음. 중복만 발생 가능.
→ **유일한 계약 조건: 소비자 멱등성(idempotency).**

---

## 4. Failover — 리더 선출 + Stale 복구 + Fencing

### 4-1. 리스(lease) 기반 리더 선출

- `leader.info` = `{ leader_id, fencing_token, lease_expiry }`
- 리더는 주기적으로 `heartbeat`를 갱신하고 `lease_expiry`를 연장한다(하트비트).
- 대기 노드는 `leader.info`를 폴링한다. **리스 만료가 관측되면** 인수 시도:
  1. `CREATE_NEW`로 `control/election.lock` 생성 시도 → 성공한 1개 노드만 선출 진행 (atomic create = mutex)
  2. 승자는 **락 안에서 `leader.info`를 다시 읽어** 리스가 여전히 만료 상태인지 재확인한다
     (락 밖에서 읽은 값으로 진행하면 차례로 락을 잡은 두 노드가 같은 token을 쓴다)
  3. `fencing_token`을 +1 증가시켜 `leader.info`를 tmp + **교체형 원자 rename**으로 바꾸고 새 리스 설정.
     하트비트도 같은 경로를 쓴다. 삭제→생성 두 단계는 금지: 그 사이에 읽는 노드가 "리더 없음"을 보고
     살아 있는 리더를 밀어낸다
  4. `election.lock` 삭제
- 선출 도중 크래시 대비: `election.lock`이 임계시간보다 오래되면 강제 정리(stale lock 회수)

### 4-2. Stale inflight 복구 (= 실제 failover 동작)

새 리더는 인수 직후 `inflight/`를 스캔하여 **이전 token**으로 stamp된 파일
(죽은 선임자가 처리 중이던 것)을 `incoming/`으로 되돌린다(rename). `attempt` +1.
같은 스캔을 **하트비트마다** 반복한다. 인수 때 한 번만 하면, 리스 만료를 아직 모르는 좀비가
그 뒤에 옛 token으로 claim한 파일은 다음 failover까지 `inflight/`에 멈춰 있기 때문이다.
`inflight/`는 대개 수 개라 비용은 하트비트 자체와 비슷하다.

### 4-3. Fencing token (좀비 리더 방어)

- GC 정지 · IO 지연 등으로 멈췄던 옛 리더가 리스 만료 후 깨어나 큐를 건드리는 위험.
- 모든 리더 동작은 자신의 token을 inflight 파일명에 stamp. 복구는 현재(최댓값) token만 신뢰.
- 리더는 매 배치 전 `leader.info`의 token이 여전히 자신인지 확인 → 아니면 즉시 양보.
- 리더는 **자기 리스 만료도 스스로 감지**한다: 마지막 성공 갱신 뒤 리스 길이가 지났으면(GC 멈춤, 절전,
  긴 메시지 처리) FS를 보지 않고도 리더십을 내려놓고 election.lock을 거쳐 다시 얻는다. 다음 하트비트가
  거절당할 때까지 옛 token으로 claim을 계속하는 창을 없애기 위함이다. 처리가 리스보다 길어질 수 있는
  소비자는 `fq_heartbeat`로 중간에 연장한다.
- `election.lock` stale 회수는 unlink가 아니라 고유 이름으로 rename → 동시에 회수하는 두 노드 중
  rename에 성공한 쪽만 새 락을 만든다.

> **정직한 한계:** 파일/리스만으로는 옛 리더가 만료 직전 틈에 단 한 번의 잘못된 ACK을 하는 것을
> 100% 막을 수 없다. 다만 At-least-once + 멱등 소비자에서는 그 결과가 "유실"이 아니라 "중복"이라
> 허용 범위다. 완전 차단이 필요하면 스토리지 레벨 fencing(SCSI-3 PR 예약, 또는 Windows Failover
> Cluster의 디스크 소유권)을 권한다. 토폴로지 A에서는 클러스터 매니저가 이 fencing을 대신 한다.

### 4-4. 시계 문제 (노드 간 clock skew)

리스 만료를 각 노드의 wall-clock으로 비교하면 시계 차이로 오작동.
→ **공유 FS의 타임스탬프를 단일 시각 출처로 사용**: `heartbeat` 파일 mtime과, 임시 파일을 만들어
읽은 "현재 FS 시각"을 비교. 모든 노드가 같은 시계(FS)를 보므로 skew 무관.
리스 길이는 하트비트 주기의 3~5배 + 여유로 설정.

**메시지명의 시각도 같은 문제를 가진다.** 파일명 정렬이 곧 소비 순서이므로 발행 노드들의 벽시계가
어긋나면 노드 간 FIFO가 깨지고, NTP가 시계를 되돌리면 한 노드 안에서도 뒤집힌다. 발행마다 FS 시각을
읽으면 발행 비용이 두 배가 되므로, 대신 **오프셋 캐시**를 쓴다: 프로세스마다 `FS 시각 - 로컬 벽시계`를
`FQ_CLOCK_SYNC_MS`(30초)마다 한 번 재서, 이름에는 `로컬 벽시계 + 오프셋`을 쓴다. 여기에 **단조 보정**
(직전 발급값 이하이면 +1)을 더해 재동기 사이에 로컬 시계가 뒤로 점프해도 한 프로세스 안에서는 역행하지
않는다. 남는 한계: 재동기 사이에 로컬 시계가 앞으로 점프한 구간, FS mtime 해상도(FAT·일부 SMB는 1~2초),
그리고 프로세스 재시작 시 `seq`가 0부터 다시 시작하는 것 — 모두 best-effort FIFO 범위 안이다.

---

## 5. 부가 메커니즘

- **Poison 메시지 / DLQ:** `attempt`가 N 초과 시 `dead/`로 이동(무한 재처리 루프 차단).
- **GC:** `tmp/`의 임계시간 초과 고아 파일 정리(크래시한 발행 중 파일).
- **폴링 vs 알림:** 베이스라인은 디렉터리 폴링(공유/네트워크 FS에서 가장 호환). 저지연이 필요하면
  Windows `ReadDirectoryChangesW`(로컬), Linux `inotify`를 최적화로 추가.
  단, 네트워크 공유에선 알림이 안 올 수 있으므로 폴링은 항상 유지.
- **순서:** 단일 활성 소비자가 파일명 정렬 순으로 oldest-first 처리하면 best-effort FIFO.
  엄격 순서가 필요하면 stream_key로 파티셔닝하여 파티션 내 순서만 보장
  (실패 복구 시 중복으로 인한 미세 재정렬은 발생 가능).
- **2단계 발행/꺼내기 (`fq_adopt` / `fq_take`):** 트랜잭션 관리자(예: mica의 /Q 자원 관리자)가 메시지를 큐 밖
  (같은 파일시스템)의 파일에 보관했다가 결정에 따라 옮긴다. `fq_adopt` = 영속된 파일을 `incoming/`으로 rename
  (새 이름, 발행 시각 = 지금), `fq_take` = claim한 메시지를 `inflight/`에서 호출자의 파일로 rename(ack 대신).
  원칙 §0 그대로 상태 전이는 rename 한 번이고, 원본이 사라지므로 크래시 후 같은 호출을 반복해도 중복이 없다
  (`fq_adopt`는 원본이 없으면 `FQ_ENOENT`). 꺼낸 메시지는 `inflight/`에 없으므로 stale 복구 대상이 아니다.
  `fq_return`은 꺼낸 파일을 `fq_nack`과 같은 규칙으로 되돌린다(꺼낼 때의 attempt+1, 한도면 `dead/`) — 트랜잭션이
  롤백될 때마다 시도로 세어 poison 메시지가 무한히 돌지 않게 한다.
- **골라서 소비 (`fq_claim_if` / `fq_consume_if`) / 되돌려 놓기 (`fq_release`):** 오래된 순으로 incoming 파일을
  읽어 보며 술어가 받는 첫 메시지만 claim한다(상관 ID·지정 시각·만료 같은 응용 조건). 매번 새로 나열하고 후보마다
  읽으므로 `fq_claim`보다 비싸다. `fq_release`는 claim을 원래 이름 그대로 incoming에 돌려놓는다(attempt·순서 유지,
  들여다보기 용). 둘 다 claim 캐시를 비운다.

---

## 6. C API 스케치

```c
typedef struct fq_queue fq_queue;
typedef struct fq_msg   fq_msg;
typedef struct { char leader_id[64]; uint64_t token; } fq_lease;

/* 공통 */
int  fq_open(const char *root, fq_queue **out);
void fq_close(fq_queue *q);

/* Producer */
int  fq_publish(fq_queue *q, const void *data, size_t len,
                const char *stream_key /*nullable*/);

/* Consumer (리더십 보유 시에만 claim/ack) */
int  fq_acquire_leadership(fq_queue *q, fq_lease *lease);  /* 선출 시도 */
int  fq_renew_lease(fq_queue *q, fq_lease *lease);         /* 하트비트 */
int  fq_recover_stale(fq_queue *q, const fq_lease *lease); /* 선임자 inflight 회수 */
int  fq_claim(fq_queue *q, const fq_lease *lease, fq_msg **out); /* NULL=비었음 */
int  fq_ack(fq_queue *q, fq_msg *m);    /* inflight unlink */
int  fq_nack(fq_queue *q, fq_msg *m);   /* 즉시 requeue */
```

### Win32 ↔ POSIX 원자 연산 매핑 (플랫폼 추상화 계층 `fq_fs`로 격리)

| 연산 | Win32 | POSIX |
|---|---|---|
| 배타 생성 (mutex) | `CreateFile(CREATE_NEW)` | `open(O_CREAT\|O_EXCL)` |
| 원자 rename | `MoveFileExW(.., MOVEFILE_WRITE_THROUGH)` (대상 유니크) | `rename(2)` |
| 파일 fsync | `FlushFileBuffers` / `FILE_FLAG_WRITE_THROUGH` | `fsync` |
| 디렉터리 영속화 | NTFS 저널 (별도 dir fsync API 없음, write-through로 대체) | `fsync(dir_fd)` |
| 권고 락 (선택) | `LockFileEx(EXCLUSIVE\|FAIL_IMMEDIATELY)` | `fcntl(F_SETLK)` / `flock` |

---

## 7. 구현 순서 (스캐폴딩 계획)

1. **원자성 검증 하니스** — 목표 스토리지에서 rename / CREATE_NEW / 락의 원자성을 실제 확인하는 작은 C 테스트.
2. **`fq_fs` 추상화 계층** — Win32/POSIX 원자 연산 래퍼.
3. **발행·소비 골격** — `fq_publish` / `fq_claim` / `fq_ack`.
4. **리더 선출** — `fq_acquire_leadership` / `fq_renew_lease`.
5. **Stale 복구** — `fq_recover_stale`.
6. **크래시 주입 테스트** — 각 단계에서 강제 종료 후 재기동하여 §3 표를 검증.
