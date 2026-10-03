# 부록

## 명령어 요약

```bash
# --- 빌드 ---
cmake -S . -B build -G "Unix Makefiles"
cmake --build build

# --- 테스트 ---
ctest --test-dir build --output-on-failure          # C 단위 테스트
./build/test_atomicity [작업디렉터리]                # 원자성 검증
./build/test_basic [큐루트]                          # 기본 라운드트립/failover/식별자 검증
bash tests/test_concurrency.sh \
     ./build/fq_producer ./build/fq_consumer         # 다중 프로세스 동시성/failover

# --- CLI 예제 ---
./build/fq_producer <큐루트> <개수> [접두사]
./build/fq_consumer <큐루트> <node_id> <로그파일> <실행시간ms>
./build/fq_bench <큐루트> <개수> <레코드크기> [both|send|recv]

# --- 매뉴얼 빌드 ---
bash docs/build_manual.sh        # 또는
pwsh docs/build_manual.ps1
```

## 용어집

| 용어 | 설명 |
|---|---|
| **At-least-once** | 메시지가 최소 1번 처리됨을 보장하는 전달 모델. 유실 없음, 중복 가능. |
| **Active-Passive** | 한 시점에 활성 노드 1개만 동작하고, 장애 시 대기 노드가 인수하는 failover 방식. |
| **Lease (리스)** | 리더십의 시간제 임대. 만료 전 하트비트로 갱신하지 못하면 다른 노드가 인수한다. |
| **Fencing token** | 단조 증가하는 정수. inflight에 stamp되어, 만료된 옛 리더(좀비)의 작업을 식별·무시한다. |
| **inflight** | 소비자가 claim하여 처리 중인 상태(디렉터리). ack되면 사라지고, 미완 시 복구 대상. |
| **DLQ (dead-letter queue)** | 재시도 한도를 초과한 메시지를 격리하는 `dead/` 디렉터리. |
| **Stale 복구** | 리더가 옛 token의 inflight(죽은 선임자·좀비 것)를 다시 큐로 되돌리는 동작(`fq_recover_stale`). 인수 직후와 매 하트비트마다. |
| **리스 자체 만료 감지** | 리더가 마지막 성공 갱신 뒤 리스 길이가 지났음을 스스로 알아채고 리더십을 내려놓았다가 다시 얻는 동작. |
| **멱등 (idempotent)** | 같은 작업을 여러 번 해도 결과가 한 번 한 것과 같은 성질. 소비자의 계약 조건. |
| **토폴로지 A/B** | 공유 디스크를 활성 노드만 마운트(A)하는지, 여러 노드가 동시 마운트(B)하는지의 구분. |

## 더 읽을거리

- `DESIGN.md` — 설계 결정의 근거와 전체 실패 시나리오 분석.
- `CLAUDE.md` — 저장소에서 작업할 때의 아키텍처 가이드.
- `include/fq.h` — API의 정본(선언 + 주석).
