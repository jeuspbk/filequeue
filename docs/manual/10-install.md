# 설치 및 빌드

## 요구 사항 (툴체인)

| 도구 | 용도 | 비고 |
|---|---|---|
| C 컴파일러 | 라이브러리/테스트 빌드 | C11. gcc / clang / MSVC |
| CMake ≥ 3.15 | 빌드 구성 | |
| make 또는 nmake | 빌드 실행 | 생성기에 따라 |
| bash | 동시성 테스트 스크립트(선택) | `test_concurrency.sh` |
| pandoc | 이 매뉴얼 빌드(선택) | `docs/build_manual.sh` |

개발 기준 환경은 **Cygwin gcc + cmake + make**다.

## 빌드

```bash
cd /path/to/filequeue
cmake -S . -B build -G "Unix Makefiles"   # 최초 1회 (CMakeLists.txt 변경 시 재실행)
cmake --build build                        # 빌드
```

산출물:

- `build/libfilequeue.a` — 정적 라이브러리
- `build/test_atomicity`, `build/test_basic`, `build/test_consume`, `build/test_twophase`,
  `build/test_clock`, `build/test_order`, `build/test_dlq`, `build/test_batch` — 단위 테스트 (`ctest` 등록)
- `build/test_election` — 리더 선출 경합(split-brain) 검사. 동시성 스크립트가 자동으로 사용
- `build/fq_producer`, `build/fq_consumer` — CLI 예제 겸 테스트 드라이버
- `build/fq_bench` — 처리량 벤치마크 (`BENCHMARK.md` 참고)

## 테스트 실행

```bash
# C 단위 테스트 (원자성, 기본 라운드트립/failover, consume·하트비트, 2단계 발행, FS 시각, 순서 지정, DLQ, 배치 발행·묶음 소비)
ctest --test-dir build --output-on-failure

# 단일 테스트 직접 실행
./build/test_atomicity [작업디렉터리]
./build/test_basic [큐루트]

# 다중 프로세스 동시성 + failover 테스트 (bash 필요)
bash tests/test_concurrency.sh ./build/fq_producer ./build/fq_consumer
# [C] 구간은 ./build/test_election 을 자동으로 찾아 리더 선출 경합(split-brain)을 검사한다

# 변이 테스트 (선택, 약 45분): 방어 로직을 하나씩 끈 코드로 위 테스트를 돌려 잡아내는지 확인
bash tests/mutate.sh      # 결과: build/mut/result.txt ("놓침" = 그 방어를 검사하는 테스트가 없음)
```

> 동시성 테스트의 [B]는 활성 소비자가 메시지를 처리했지만 ack하기 전에 `kill -9`로 죽이고, 새 리더가 그
> 메시지를 회수해 다시 전달하는지(중복 1건)까지 확인한다. "유실 없음"만 보면 회수가 고장 나도 통과하기 때문이다.
> [D]는 같은 일을 묶음 소비(20건씩)로 해서, 죽은 쪽이 들고 있던 묶음만 재전달되는지(중복이 묶음 크기 이하) 본다.

> 동시성 테스트는 `ctest`에 등록돼 있지 않다. 혼합 셸 환경(cygwin `ctest` ↔ git-bash)에서
> 경로 표기(`/cygdrive/d` vs `/d`)가 달라 자동 실행이 불안정하기 때문이며, 스크립트 자체는
> 정상 동작한다. 그래서 수동 실행으로 분리했다.

## 플랫폼별 FS 구현 선택

큐 로직은 `fq_fs.h` 인터페이스에만 의존하고, 그 구현은 플랫폼에 따라 CMake가 자동 선택한다.

| 플랫폼 | 사용 구현 | 핵심 API |
|---|---|---|
| 네이티브 Windows (MSVC) | `src/fq_fs_win32.c` | `MoveFileEx`, `SetFileInformationByHandle(FileRenameInfoEx)`, `CreateFile(CREATE_NEW)`, `LockFileEx`, `FlushFileBuffers` |
| POSIX / **Cygwin** 포함 | `src/fq_fs_posix.c` | `rename`, `renameat2(RENAME_NOREPLACE)`(Linux), `open(O_EXCL)`, `fcntl(F_SETLK)`, `fsync` |

Cygwin은 POSIX를 제공하므로 항상 POSIX 경로로 빌드된다. 네이티브 Windows 구현은 Cygwin의 MinGW 크로스 컴파일러
(`x86_64-w64-mingw32-gcc`)로 네이티브 실행 파일을 만들어 검증한다(절차는 `CLAUDE.md`). MSVC로는 아직 빌드해 보지 않았다. 새 FS 연산이 필요하면 `fq_fs.h`에
인터페이스를 추가하고 **양쪽 구현을 모두** 채워야 추상화가 유지된다.

## 라이브러리 사용하기

`include/`를 인클루드 경로에 추가하고 `libfilequeue.a`를 링크한다.

```bash
cc myapp.c -I/path/to/filequeue/include -L/path/to/filequeue/build -lfilequeue -o myapp -pthread
```

CMake 프로젝트라면 `filequeue` 타깃에 링크하면 인클루드 경로가 자동 전파된다. 라이브러리는 `fq_publish_batch`의
병렬 기록과 `fq_consume_batch`·`fq_ack_batch`의 병렬 읽기·삭제에 스레드를 쓰므로 POSIX에서는 `-pthread`가 필요하다(CMake 타깃은 `Threads::Threads`를 함께 전파한다).
