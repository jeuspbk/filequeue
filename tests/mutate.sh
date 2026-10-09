#!/usr/bin/env bash
# mutate.sh - 변이 테스트: 방어 로직을 하나씩 끈 코드로 단위 테스트·동시성 테스트를 돌려 잡아내는지 본다.
#
# 사용: bash tests/mutate.sh [변이목록=tests/mutations.txt]   (저장소 루트에서)
# 작업 디렉터리 build/mut/에 트리를 복사해 변이마다 gcc로 빌드한다(src/는 건드리지 않음).
# 결과: build/mut/result.txt — 변이마다 잡아낸 테스트 이름, 아무도 못 잡으면 "놓침".
# 변이 하나에 1분 안팎(동시성 테스트 포함)이다.
set -u
LIST="${1:-tests/mutations.txt}"
M=build/mut; mkdir -p $M
OUT=$M/result.txt; : > $OUT
UNIT="test_atomicity test_basic test_consume test_twophase test_clock test_order test_dlq"
grep -v '^#' "$LIST" | while IFS='#' read -r label file from to; do
  [ -z "$label" ] && continue
  T=$M/tree; rm -rf $T; mkdir -p $T; cp -r src include tests examples $T/
  n=$(grep -cF -- "$from" $T/$file)
  if [ "$n" = 0 ]; then echo "$label :: 적용 실패(원문 없음)" | tee -a $OUT; continue; fi
  FROM="$from" TO="$to" perl -0pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' $T/$file
  B=$T/bin; mkdir -p $B
  if ! (cd $B && gcc -std=gnu11 -O1 -I../include -c ../src/*.c 2>cc.err); then
    echo "$label :: 빌드 실패" | tee -a $OUT; continue
  fi
  for t in $UNIT test_election; do gcc -std=gnu11 -O1 -I$T/include -o $B/$t.exe $T/tests/$t.c $B/*.o 2>/dev/null; done
  for e in fq_producer fq_consumer; do gcc -std=gnu11 -O1 -I$T/include -o $B/$e.exe $T/examples/$e.c $B/*.o; done
  caught=""
  for t in $UNIT; do
    R=$M/r_$t; rm -rf $R $R-*                 # test_basic은 <root>-lead 같은 형제 디렉터리도 쓴다
    timeout 120 $B/$t.exe $R > $M/$t.log 2>&1 || caught="$caught $t"
  done
  timeout 300 bash $T/tests/test_concurrency.sh $B/fq_producer.exe $B/fq_consumer.exe > $M/conc.log 2>&1 \
    || caught="$caught concurrency($(grep -c FAIL $M/conc.log))"
  echo "$label ::${caught:- 놓침}" | tee -a $OUT
done
