# Failover 운영

filequeue의 failover는 **리스(lease) 기반 리더 선출 + fencing token + stale 복구** 세 가지로
구성된다.

## 리더 선출과 리스(lease)

한 시점에 활성 소비자는 하나여야 한다. `control/leader.info`가
`<leader_id> <token> <lease_expiry>` 한 줄로 현재 리더를 기록한다.

1. 대기 노드는 `leader.info`를 폴링한다. **리스 만료가 관측되면** 인수를 시도한다.
2. 인수는 `control/election.lock`을 **원자적 배타 생성**(`fq_fs_create_new`)으로 직렬화한다.
   성공한 단 하나의 노드만 선출을 진행한다.
3. 승자는 `token`을 +1 증가시켜 `leader.info`를 tmp+rename으로 교체하고, 새 리스 만료시각을 쓴다.
4. `election.lock`을 삭제한다.

활성 리더는 `FQ_HEARTBEAT_MS` 주기로 `fq_renew_lease`를 호출해 만료시각을 연장한다. 리더가
죽으면 더 이상 갱신되지 않아 리스가 만료되고, 대기 노드가 위 절차로 인수한다.

> 선출 도중 크래시로 `election.lock`이 남으면, `FQ_ELECTION_STALE_MS`보다 오래된 lock은
> 다음 시도자가 강제 회수한다. 데드락이 생기지 않는다.

## Fencing token (좀비 리더 방어)

GC 정지나 I/O 지연으로 멈췄던 옛 리더가 **리스 만료 후 깨어나** 큐를 건드리는 위험이 있다.
이를 막기 위해 단조 증가하는 `token`을 사용한다.

- 모든 inflight 파일에는 점유한 리더의 token이 stamp된다(`__t<token>`).
- 복구는 **현재 token보다 낮은** inflight만 회수 대상으로 삼는다.
- 리더는 매 배치 전 `fq_renew_lease`로 자신이 여전히 리더인지 확인하고, 아니면(`FQ_ENOLEADER`)
  즉시 소비를 중단한다.

> **정직한 한계:** 파일/리스만으로는 옛 리더가 만료 직전의 틈에 단 한 번 잘못된 ACK을 하는 것을
> 100% 막을 수 없다. 다만 at-least-once + 멱등 소비자에서는 그 결과가 "유실"이 아니라 "중복"이라
> 허용 범위다. 완전 차단이 필요하면 스토리지 레벨 fencing(SCSI-3 PR 예약, Windows Failover
> Cluster의 디스크 소유권)을 사용한다.

## Stale inflight 복구

새 리더는 인수 직후 `fq_recover_stale`를 호출한다. 이 함수는 `inflight/`를 훑어 **옛 token**으로
점유된 파일(죽은 선임자가 처리 중이던 것)을 찾아 `incoming/`으로 되돌린다(attempt+1). 한도를
초과하면 `dead/`로 보낸다. 이것이 **실제 failover에서 진행 중이던 작업이 유실되지 않고 재처리되는**
메커니즘이다.

## 토폴로지 A/B와 스토리지 fencing

"공유 디스크 + Active-Passive"는 보통 둘 중 하나로 배치된다. 설계가 갈리므로 구분해야 한다.

| 토폴로지 | 설명 | 동시 접근 | 필요한 보호 |
|---|---|---|---|
| **A. 단일 마운트** (권장) | 공유 볼륨을 활성 노드만 마운트, 장애 시 fencing 후 대기 노드가 remount | 정상 시 1개 노드만 | 크래시 일관성만 (노드 간 락 불필요) |
| **B. 동시 마운트** | NFS/SMB/클러스터 FS를 여러 노드가 동시 RW 마운트 | 다중 노드 동시 | 노드 간 원자적 rename + 락 의미 |

> 일반 블록 스토리지(SAN LUN)는 클러스터 FS 없이 여러 노드가 동시 RW 마운트하면 파일시스템이
> 깨진다. 따라서 Active-Passive에서는 거의 항상 **토폴로지 A**다. filequeue는 둘 다에서 동작하도록
> atomic rename과 atomic create에만 의존한다. 토폴로지 A에서는 클러스터 매니저가 스토리지 레벨
> fencing을 대신해 주므로 좀비 리더 위험도 사라진다.

## 크래시 지점별 결과

소비 흐름의 어느 단계에서 크래시가 나도 유실은 없다. 중복만 발생할 수 있다.

| 크래시 시점 | 디스크 상태 | 복구 후 결과 | At-least-once |
|---|---|---|---|
| 발행 중 (tmp 단계) | `tmp/`에 잔존 | GC가 정리, 미발행 | OK (미투입) |
| CLAIM 직후 | `inflight/`에 잔존 | 재처리 | OK (중복 가능) |
| PROCESS 중 | `inflight/`에 잔존 | 재처리 | OK (중복 가능) |
| ACK 직전 | `inflight/`에 잔존 | 재처리 → 중복 | OK (멱등 필요) |
| ACK 후 | 파일 없음 | 완료 | OK |

→ **유실은 어느 경우에도 없다. 중복만 가능하다. 따라서 소비자 멱등성이 유일한 계약 조건이다.**
