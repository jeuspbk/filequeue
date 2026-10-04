# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 개요

공유 디스크 기반의 통신용 파일 큐(C 라이브러리). 하나의 파일시스템 디렉터리를 큐로 사용하여
프로세스/노드 간 메시지를 주고받고, 공유 스토리지를 통해 **Active-Passive failover**와
**At-least-once** 전달을 제공한다. 전체 설계 근거와 실패 시나리오 표는 `DESIGN.md`에 있다.
설계 결정을 바꾸기 전에 반드시 `DESIGN.md`를 먼저 읽을 것.

## 빌드 / 테스트

이 머신의 툴체인은 **Cygwin gcc + cmake + make** (`D:\cygwin64\bin`). Bash 툴에서 실행:

```bash
cd D:/ProjPkg/filequeue
cmake -S . -B build -G "Unix Makefiles"   # 최초 1회 (CMakeLists.txt 변경 시 재실행)
cmake --build build                        # 빌드
ctest --test-dir build --output-on-failure # C 단위 테스트 (atomicity, basic)
./build/test_atomicity.exe [작업디렉터리]  # 단일 테스트 직접 실행
./build/test_basic.exe [큐루트]

# 다중 프로세스 동시성 + failover 테스트 (bash로 별도 실행 — 아래 주의 참고)
bash tests/test_concurrency.sh ./build/fq_producer.exe ./build/fq_consumer.exe
# [C] 구간은 ./build/test_election.exe 를 자동으로 찾아 리더 선출 경합(split-brain)을 검사한다
```

**테스트가 2계층인 이유:** `ctest`에는 C 단위 테스트만 등록돼 있다. 동시성/failover 테스트는
실제 프로세스를 띄우는 bash 스크립트이며, **cygwin ctest와 git-bash의 경로 표기 차이
(`/cygdrive/d` vs `/d`)** 때문에 ctest 자동 실행이 불안정하여 수동 실행으로 분리했다.
스크립트 자체는 정상 동작한다. `examples/`의 `fq_producer`/`fq_consumer`는 이 테스트의
드라이버 겸 사용 예제다. `FQ_LEASE_MS_OVERRIDE` 환경변수로 리스를 단축해 failover를 빠르게 검증한다.

빌드 파일(CMakeLists.txt)은 플랫폼에 따라 FS 구현을 자동 선택한다:
- 네이티브 Windows(MSVC) → `src/fq_fs_win32.c`
- 그 외(POSIX, **Cygwin 포함**) → `src/fq_fs_posix.c`

→ 현재 머신에서는 항상 **POSIX 경로**로 빌드된다. `fq_fs_win32.c`는 MSVC 빌드 전용이며
이 머신에서는 컴파일조차 되지 않으므로, 수정 시 별도 검증이 필요하다.

## 아키텍처 (핵심)

3계층 구조이며, 각 계층은 아래 계층에만 의존한다:

```
fq_queue.c / fq_leader.c   (큐 로직: publish/claim/ack, 리더 선출/복구)
        │  fq_fs.h 인터페이스에만 의존
        ▼
fq_fs_posix.c / fq_fs_win32.c   (플랫폼 원자 연산 추상화)
```

**불변식 — 코드 전체를 관통하는 단 하나의 원칙:**
> 메시지의 상태 = 어느 하위 디렉터리에 있는가. 모든 상태 전이 = **원자적 rename 또는 unlink**.
> 파일 내용을 제자리(in-place) 수정하는 코드는 절대 추가하지 말 것.

이 덕분에 어느 시점에 크래시가 나도 파일은 항상 정확히 한 디렉터리에 온전히 존재한다.
새 기능을 넣을 때 이 불변식을 깨면(예: 파일을 열어 append) 크래시 안전성이 무너진다.

### 온디스크 레이아웃 (`<root>/` 하위)
`tmp/`(쓰기 스테이징) · `incoming/`(소비 대기) · `inflight/`(처리 중) · `dead/`(DLQ) ·
`control/`(leader.info, election.lock).

### 메시지 흐름
- **발행**: `tmp/`에 기록+fsync → `incoming/`으로 원자적 rename → 디렉터리 fsync. (torn 파일 방지)
- **소비**: `incoming/`→`inflight/` rename으로 CLAIM(경합 시 단 하나만 성공) → 처리 → unlink로 ACK.
  큐 내부 전이는 모두 `fq_fs_rename`(교체형, POSIX `rename`)이다. 대상 이름이 구조상 유일하기 때문.
  `fq_fs_rename_noreplace`는 POSIX에서 link+unlink 두 단계라 경합 시 둘 다 성공할 수 있으므로
  `fq_take`(호출자 경로로 내보내기)에만 쓴다.
- **실패**: CLAIM~ACK 사이 크래시 시 파일이 `inflight/`에 남고, 새 리더가 `fq_recover_stale`로
  `incoming/`에 되돌림 → 재처리(중복 가능, 유실 없음). **소비자는 반드시 멱등**이어야 한다.

### 파일명에 인코딩된 상태 (단, 별도 메타파일 없음)
- `node_id`·`stream_key`는 `fq_valid_ident`로 검증한다(`[A-Za-z0-9._-]`, `__` 금지, 어기면 `FQ_EINVAL`).
  이 규칙이 `__t<token>`·`.a<N>` 파싱과 leader.info의 공백 구분 포맷을 지탱하므로 완화하지 말 것.
- 메시지명: `<epoch_ms>-<seq>-<producer>-<id>[.<stream>].msg` — 앞쪽 시간으로 best-effort FIFO 정렬.
- inflight: `<메시지명>__t<token>` — `token`은 fencing용 리더 토큰.
- 재시도: `incoming`으로 되돌릴 때 `.a<N>` 접미사로 attempt 카운트. `FQ_MAX_ATTEMPTS` 초과 시 `dead/`.
- 이 이름 규칙 파싱은 `fq_util.c`의 `fq_parse_attempt` / `fq_parse_inflight`에 집중되어 있다.
  파일명 포맷을 바꾸면 이 두 함수와 `fq_queue.c`/`fq_leader.c`의 생성부를 함께 고쳐야 한다.

### Failover (Active-Passive) — `fq_leader.c`
- **리스 기반 선출**: `control/leader.info`(`<id> <token> <expiry> <instance>`)를 폴링, 만료 관측 시
  `control/election.lock`을 원자적 배타 생성(`fq_fs_create_new`)으로 직렬화한 뒤, **락 안에서
  leader.info를 다시 읽어 재검증**하고 token = max(leader.info, inflight의 최대 token) + 1로 인수.
  "내 리스" 판정은 node_id **와** instance(핸들 nonce)로 한다 — node_id만 보면 id가 같은 두 프로세스가
  서로 빼앗는다. **하트비트(`fq_renew_lease`)도 같은 락 안에서** 읽고-확인하고-쓴다(락 밖에서 쓰면
  멈췄던 옛 리더가 새 리더를 옛 token으로 덮어써 token이 되돌아간다). 락이 잡혀 있으면 `FQ_ELOCKED`.
  그래서 stale 락 회수 임계는 `fq_election_stale_ms()`(= min(30초, 리스/3))로 읽는다 — 리스보다 길면
  락을 쥔 채 죽은 노드가 리더까지 멈춘다. 정상 종료는 `fq_close`/`fq_release_leadership`으로 반납.
  leader.info 교체는 항상 tmp에 쓰고 `fq_fs_rename`으로 원자 교체한다. 삭제 후 생성으로 바꾸면 그
  사이에 읽는 노드가 "리더 없음"을 보고 살아 있는 리더를 밀어낸다 (`tests/test_election.c`가 이 경합을 검사).
- **leader.info 읽기는 `fq_read_replaced`**: 교체형 rename이 읽는 쪽에 원자적이지 않은 FS(Cygwin/NTFS
  실측)가 있어 ENOENT를 짧게 재시도한다. 그냥 `fq_fs_read_file`로 읽으면 "리더 없음"으로 오인한다.
- **fencing token**: 단조 증가. 모든 inflight에 stamp되며, 복구는 현재 token보다 낮은 것만 회수 →
  멈췄다 깨어난 옛 리더(좀비)가 큐를 건드려도 무해. 복구(`fq_recover_stale`)는 인수 직후뿐 아니라
  **하트비트마다** 돈다(좀비가 나중에 claim한 inflight도 다음 failover를 기다리지 않음).
- **리스 자체 만료 감지**: `fq_consume`의 리더는 마지막 성공 갱신 뒤 리스 길이가 지나면(단조 시계
  `fq_now_mono_ms`로 잰다. GC 멈춤, 절전, 긴 처리) 스스로 내려놓고 election.lock을 거쳐 다시 얻는다. 메시지 하나 처리가 리스보다 길 수 있으면
  처리 중 `fq_heartbeat(q)`를 호출할 것.
- **election.lock stale 회수**는 unlink가 아니라 고유 이름으로 rename한 뒤 새로 만든다. unlink면
  동시에 회수하는 두 노드가 서로의 새 락을 지워 둘 다 임계 구역에 들어간다.
- **시각**: 노드 간 clock skew를 피하려 `fq_fs_now_ms`(공유 FS에 임시 파일을 만들어 mtime을 읽음)를
  단일 시각 출처로 사용한다. 리스 비교에 로컬 wall-clock을 쓰지 말 것.
  메시지 파일명의 시각도 `fq_queue.c`의 `msg_clock_ms`가 FS 시각 오프셋(30초마다 재측정)을 더해
  만들고 단조 보정한다. 파일명에 `fq_now_wall_ms`를 직접 쓰지 말 것.

## 작업 시 주의점
- 새 FS 연산이 필요하면 먼저 `fq_fs.h`에 인터페이스를 추가하고 **posix/win32 양쪽**을 구현할 것.
  큐 로직에서 OS API를 직접 호출하면 추상화가 깨진다.
- `fq_fs_list` 콜백이 0이 아닌 값을 돌려주면 나열이 멈추고 그 값이 그대로 반환된다. 오류를 알릴 때는
  음수 `FQ_*` 코드를 돌려줄 것(0이 아닌 양수로 "그냥 멈춤"을 표현하면 호출자는 오류로 본다).
- 잦은 경로(대기 노드 폴링, 메시지 이름)에는 `fq_fs_clock_est_ms`(추정 FS 시각, 30초마다 재측정)를,
  정확해야 하는 판정(리스 인수, 갱신)에는 `fq_fs_now_ms`를 쓴다.
- 리스 길이는 `fq_lease_ms()` 하나로 읽는다(`FQ_LEASE_MS_OVERRIDE` 반영). 복제하지 말 것.
- 일시적 오류에 리더십을 버리지 말 것: `fq_consume`은 하트비트 실패(`FQ_ERR`·락 보유)에도 리스 자체 만료
  전까지 claim을 계속한다. `fq_fs_write_sync`는 실패 시 대상 파일을 지우므로 임시 파일 경로에만 쓸 것.
- 반환 규약: `FQ_OK`(0) 성공, 음수 오류 코드(`fq.h`). claim 경합 패배는 `FQ_ENOENT`로
  표현되며 정상 흐름이다 — 다음 후보로 넘어가야 한다.
- 공유 디렉터리(`tmp/`, `control/`)에 만드는 임시 파일 이름에는 반드시 `node_id`를 넣을 것(`fq_fs_now_ms`도
  tag 인자로 받는다). `fq_gen_id`는 pid+salt+카운터+ms 조합이라 노드 간 유일성이 확률적이다(카운터는
  원자적이라 프로세스 안에서는 스레드 간에도 유일). 메시지 이름은 이미 producer를 포함한다.
- claim 뒤 읽기나 할당이 실패하면 반드시 `incoming/`으로 되돌린다. 내 token이 찍힌 inflight는 복구 대상이
  아니라서 되돌리지 않으면 내가 죽을 때까지 멈춘다.
- 대상 스토리지가 바뀌면 `test_atomicity`를 그 스토리지에서 먼저 돌려 rename/create/lock 원자성을
  확인할 것. 이 전제가 깨지면 큐 정확성이 보장되지 않는다 (`DESIGN.md` §0).
