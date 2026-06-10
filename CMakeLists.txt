cmake_minimum_required(VERSION 3.15)
project(filequeue C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# 플랫폼별 FS 구현 선택:
#  - 네이티브 Windows(MSVC) → fq_fs_win32.c
#  - 그 외(POSIX, Cygwin 포함) → fq_fs_posix.c
if(WIN32 AND NOT CYGWIN)
  set(FQ_FS_SRC src/fq_fs_win32.c)
else()
  set(FQ_FS_SRC src/fq_fs_posix.c)
endif()

add_library(filequeue STATIC
  src/fq_queue.c
  src/fq_leader.c
  src/fq_consume.c
  src/fq_util.c
  ${FQ_FS_SRC}
)
target_include_directories(filequeue PUBLIC include)

enable_testing()

add_executable(test_atomicity tests/test_atomicity.c)
target_link_libraries(test_atomicity filequeue)
add_test(NAME atomicity COMMAND test_atomicity)

add_executable(test_basic tests/test_basic.c)
target_link_libraries(test_basic filequeue)
add_test(NAME basic COMMAND test_basic)

add_executable(test_consume tests/test_consume.c)
target_link_libraries(test_consume filequeue)
add_test(NAME consume COMMAND test_consume)

# 다중 프로세스 드라이버 (CLI 예제 겸 동시성/failover 테스트용)
add_executable(fq_producer examples/fq_producer.c)
target_link_libraries(fq_producer filequeue)

add_executable(fq_consumer examples/fq_consumer.c)
target_link_libraries(fq_consumer filequeue)

add_executable(fq_bench examples/fq_bench.c)
target_link_libraries(fq_bench filequeue)

# 다중 프로세스 동시성 + failover 테스트는 tests/test_concurrency.sh 로 별도 실행한다
# (ctest에 등록하지 않음: cygwin ctest와 git-bash 간 경로 표기(/cygdrive vs /d) 불일치로
#  ctest 자동 실행이 불안정. 스크립트 자체는 정상 동작):
#   bash tests/test_concurrency.sh ./build/fq_producer.exe ./build/fq_consumer.exe
