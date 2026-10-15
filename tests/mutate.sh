#!/usr/bin/env bash
# mutate.sh - 변이 테스트: 방어 로직을 하나씩 끈 코드로 단위 테스트·동시성 테스트를 돌려 잡아내는지 본다.
#
# 사용: bash tests/mutate.sh [변이목록=tests/mutations.txt]   (저장소 루트에서)
#       MUT_PLAT=win32 bash tests/mutate.sh [변이목록=tests/mutations_win32.txt]
# 작업 디렉터리 build/mut/에 트리를 복사해 변이마다 gcc로 빌드한다(src/는 건드리지 않음).
# MUT_PLAT=win32면 MinGW(x86_64-w64-mingw32-gcc)로 네이티브 Win32 실행 파일을 만들어 돌린다 — fq_fs_win32.c의
# 방어는 기본(POSIX) 모드에서는 컴파일조차 되지 않아 검사되지 않는다.
# 결과: build/mut/result.txt (win32: result_win32.txt) — 변이마다 잡아낸 테스트 이름, 아무도 못 잡으면 "놓침".
# 변이 하나에 1분 안팎(동시성 테스트 포함)이다.
set -u
PLAT="${MUT_PLAT:-posix}"
M=build/mut; mkdir -p $M
if [ "$PLAT" = win32 ]; then
  LIST="${1:-tests/mutations_win32.txt}"
  CC=x86_64-w64-mingw32-gcc; CSTD=c11
  OUT=$M/result_win32.txt
  export TMPDIR="$PWD/$M/tmpd"; mkdir -p "$TMPDIR"   # 동시성 스크립트의 작업 디렉터리: 네이티브 exe가 읽을 수 있는 곳
else
  LIST="${1:-tests/mutations.txt}"
  CC=gcc; CSTD=gnu11
  OUT=$M/result.txt
fi
: > $OUT
UNIT="test_atomicity test_basic test_consume test_twophase test_clock test_order test_dlq test_batch"
grep -v '^#' "$LIST" | while IFS='#' read -r label file from to; do
  [ -z "$label" ] && continue
  T=$M/tree; rm -rf $T; mkdir -p $T; cp -r src include tests examples $T/
  n=$(grep -cF -- "$from" $T/$file)
  if [ "$n" = 0 ]; then echo "$label :: 적용 실패(원문 없음)" | tee -a $OUT; continue; fi
  FROM="$from" TO="$to" perl -0pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' $T/$file
  B=$T/bin; mkdir -p $B
  if ! (cd $B && $CC -std=$CSTD -O1 -I../include -c ../src/*.c 2>cc.err); then
    echo "$label :: 빌드 실패" | tee -a $OUT; continue
  fi
  for t in $UNIT test_election; do $CC -std=$CSTD -O1 -I$T/include -o $B/$t.exe $T/tests/$t.c $B/*.o 2>/dev/null; done
  for e in fq_producer fq_consumer; do $CC -std=$CSTD -O1 -I$T/include -o $B/$e.exe $T/examples/$e.c $B/*.o; done
  caught=""
  for t in $UNIT; do
    R=$M/r_$t; rm -rf $R $R-*                 # test_basic은 <root>-lead 같은 형제 디렉터리도 쓴다
    # 종료 코드만 믿지 말 것: git-bash에서 띄운 Cygwin 프로그램은 크래시해도 0으로 끝난다(출력 버퍼도 잃음).
    # 마지막 요약 줄("0 실패" / "0 fail" / "== OK ==")이 있어야 통과다. 없으면 크래시로 보고 표시한다.
    if ! timeout 120 $B/$t.exe $R > $M/$t.log 2>&1; then caught="$caught $t"
    elif ! tail -n 3 $M/$t.log | grep -qE '[(]0 (실패|fail)[)]|== OK =='; then caught="$caught $t(요약 없음)"; fi
  done
  { timeout 300 bash $T/tests/test_concurrency.sh $B/fq_producer.exe $B/fq_consumer.exe > $M/conc.log 2>&1 \
      && tail -n 1 $M/conc.log | grep -q 'PASS (0 실패)'; } \
    || { caught="$caught concurrency($(grep -c FAIL $M/conc.log))"; cp $M/conc.log "$M/conc_${label%% *}.log"; }
  echo "$label ::${caught:- 놓침}" | tee -a $OUT
done
