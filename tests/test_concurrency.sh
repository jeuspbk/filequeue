#!/usr/bin/env bash
# test_concurrency.sh - 다중 프로세스 동시성 + failover 검증
#
# 인자: <fq_producer 경로> <fq_consumer 경로> [test_election 경로]
# 검증 1) 다중 소비자가 동시에 떠도 Active-Passive로 단 하나만 활성, 메시지 유실 없음
# 검증 2) 활성 소비자를 kill -9 한 뒤 다른 노드가 인수(failover)하여 잔여 메시지 회수, 유실 없음
# 검증 3) 살아 있는 리더가 하트비트를 치는 동안 대기 노드가 리더십을 빼앗지 못함 (split-brain 없음)
#
# At-least-once 계약: 모든 메시지가 최소 1회 소비되어야 함(유실 0). 중복은 허용.

set -u
PRODUCER="${1:?producer 경로 필요}"
CONSUMER="${2:?consumer 경로 필요}"
# test_election 경로: 3번째 인자, 없으면 consumer 경로에서 이름만 바꿔 추정
ELECTION="${3:-${CONSUMER/fq_consumer/test_election}}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0
pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1"; fails=$((fails+1)); }

# 소비된 로그(중복 포함)에서 고유 메시지 수 세기
uniq_count() { cat "$@" 2>/dev/null | sort -u | grep -c . ; }
total_count() { cat "$@" 2>/dev/null | grep -c . ; }

echo "== [A] 다중 소비자 동시성 (유실 없음 / Active-Passive) =="
ROOT="$WORK/qA"
N=300
"$PRODUCER" "$ROOT" "$N" "a-" >/dev/null 2>&1

# 소비자 3개 동시 기동, 각자 자기 로그에 기록
"$CONSUMER" "$ROOT" nodeA "$WORK/a1.log" 4000 2>"$WORK/a1.err" &
"$CONSUMER" "$ROOT" nodeB "$WORK/a2.log" 4000 2>"$WORK/a2.err" &
"$CONSUMER" "$ROOT" nodeC "$WORK/a3.log" 4000 2>"$WORK/a3.err" &
wait

U=$(uniq_count "$WORK"/a1.log "$WORK"/a2.log "$WORK"/a3.log)
T=$(total_count "$WORK"/a1.log "$WORK"/a2.log "$WORK"/a3.log)
LEADERS=$(grep -l "leader=yes" "$WORK"/a1.err "$WORK"/a2.err "$WORK"/a3.err 2>/dev/null | wc -l)
LEFT_IN=$(ls "$ROOT/incoming" 2>/dev/null | grep -c . )
LEFT_FL=$(ls "$ROOT/inflight" 2>/dev/null | grep -c . )

echo "  발행=$N 고유소비=$U 총소비(중복포함)=$T 활성리더수=$LEADERS 잔여(incoming=$LEFT_IN inflight=$LEFT_FL)"
[ "$U" -eq "$N" ] && pass "유실 없음 (고유 소비 = 발행 $N)" || fail "유실 발생 (고유 $U != $N)"
[ "$LEFT_IN" -eq 0 ] && [ "$LEFT_FL" -eq 0 ] && pass "큐 비워짐" || fail "잔여 메시지 존재"
# 기본 리스(15s)는 4s 안에 만료되지 않으므로 활성 리더는 정확히 1
[ "$LEADERS" -le 1 ] && pass "Active-Passive (활성 리더 <= 1)" || fail "리더 다중 활성($LEADERS)"

echo "== [B] Failover (활성 소비자 강제 종료 후 인수) =="
ROOT="$WORK/qB"
N=300
"$PRODUCER" "$ROOT" "$N" "b-" >/dev/null 2>&1

# 짧은 리스로 failover를 빠르게: 1.5s
export FQ_LEASE_MS_OVERRIDE=1500

# nodeX 기동(활성 리더가 되어 소비 시작) 후 곧바로 강제 종료
"$CONSUMER" "$ROOT" nodeX "$WORK/bX.log" 60000 2>"$WORK/bX.err" &
PID=$!
sleep 0.6                       # 일부 처리 + 일부 inflight 점유 상태 만들기
kill -9 "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

PROCESSED_X=$(total_count "$WORK/bX.log")
echo "  nodeX 강제종료 전 처리=$PROCESSED_X"

# 리스 만료 대기 후 nodeY 인수
sleep 2
"$CONSUMER" "$ROOT" nodeY "$WORK/bY.log" 5000 2>"$WORK/bY.err" &
wait

unset FQ_LEASE_MS_OVERRIDE
U=$(uniq_count "$WORK/bX.log" "$WORK/bY.log")
LEFT_IN=$(ls "$ROOT/incoming" 2>/dev/null | grep -c . )
LEFT_FL=$(ls "$ROOT/inflight" 2>/dev/null | grep -c . )
DEAD=$(ls "$ROOT/dead" 2>/dev/null | grep -c . )
echo "  발행=$N 고유소비(X+Y)=$U 잔여(incoming=$LEFT_IN inflight=$LEFT_FL dead=$DEAD)"

# 유실 없음: X가 처리한 것 + Y가 회수·처리한 것의 합집합이 전부를 덮어야 함
COVERED=$((U + DEAD))
[ "$PROCESSED_X" -gt 0 ] && pass "nodeX가 활성화되어 일부 처리($PROCESSED_X)" || fail "nodeX가 아무것도 처리 못함"
[ "$COVERED" -eq "$N" ] && pass "failover 후 유실 없음 (고유$U + dead$DEAD = $N)" || fail "유실 발생 (덮은 $COVERED != $N)"
[ "$LEFT_FL" -eq 0 ] && pass "inflight 비워짐(회수 완료)" || fail "inflight 잔존($LEFT_FL)"

if [ -x "$ELECTION" ]; then
  echo "== [C] 리더 선출 경합 (살아 있는 리더를 대기 노드가 빼앗지 못함) =="
  ROOT="$WORK/qC"
  "$ELECTION" "$ROOT" A 3000 >"$WORK/cA.out" 2>&1 &
  PA=$!
  "$ELECTION" "$ROOT" B 3000 >"$WORK/cB.out" 2>&1 &
  PB=$!
  wait "$PA"; RA=$?
  wait "$PB"; RB=$?
  echo "  $(cat "$WORK/cA.out")"
  echo "  $(cat "$WORK/cB.out")"
  [ "$RA" -eq 0 ] && pass "리더 A가 리더십을 한 번도 잃지 않음" || fail "리더 A가 리더십을 잃음"
  [ "$RB" -eq 0 ] && pass "대기 노드 B의 탈취 0회" || fail "대기 노드 B가 리더십을 탈취함"
else
  echo "== [C] 건너뜀: test_election 없음 ($ELECTION) =="
fi

echo ""
echo "== 결과: $([ $fails -eq 0 ] && echo PASS || echo FAIL) ($fails 실패) =="
exit $([ $fails -eq 0 ] && echo 0 || echo 1)
