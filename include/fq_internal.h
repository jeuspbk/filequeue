/* fq_internal.h - 내부 공유 정의 (공개 API 아님) */
#ifndef FQ_INTERNAL_H
#define FQ_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "fq.h"

/* 큐 하위 디렉터리 이름 */
#define FQ_DIR_TMP      "tmp"
#define FQ_DIR_INCOMING "incoming"
#define FQ_DIR_INFLIGHT "inflight"
#define FQ_DIR_DEAD     "dead"
#define FQ_DIR_CONTROL  "control"

#define FQ_LEADER_INFO  "control/leader.info"
#define FQ_ELECTION_LCK "control/election.lock"

/* 메시지명 시각 보정: 로컬 벽시계와 공유 FS 시각의 오프셋을 이 주기마다 다시 잰다. */
#define FQ_CLOCK_SYNC_MS 30000

struct fq_queue {
    char     root[1024];
    char     node_id[64];    /* 이 노드의 식별자. 메시지 파일명(producer)과 리더십 id 겸용. */
    char     instance[40];   /* 핸들마다 새로 만드는 nonce. leader.info 4번째 필드.
                                node_id가 같은 두 프로세스(컨테이너의 pid 1 등)를 구분해,
                                한쪽이 상대의 유효한 리스를 "내 것"으로 보고 빼앗지 않게 한다. */
    uint32_t seq;            /* 발행 시퀀스 (프로세스 로컬, 비스레드세이프 - 스캐폴드) */

    /* 메시지명 시각 = 로컬 벽시계 + 오프셋(공유 FS 시각 - 로컬 벽시계).
     * 노드 간 clock skew를 흡수하면서도 발행마다 FS를 건드리지 않는다.
     * last_msg_ms: 단조 보정 — 로컬 시계가 뒤로 가도 이름의 시각은 역행하지 않는다. */
    int64_t  clock_offset_ms;
    uint64_t clock_next_sync_ms;   /* 다음 재동기 시각 (로컬 벽시계 ms, 0이면 즉시) */
    uint64_t last_msg_ms;

    /* claim 리스트 캐시: incoming/ 디렉터리 나열을 claim마다 반복하지 않도록
     * 정렬된 이름 배치를 보관하고, 소진되면 1회 재나열한다. (O(n^2) -> 분할상환 O(n)) */
    char   **claim_cache;
    size_t   claim_n;
    size_t   claim_idx;
    size_t   claim_cap;

    /* fq_consume() 통합 wrapper 상태 */
    int      lead_held;            /* 현재 리더십 보유 여부 */
    fq_lease lead_lease;           /* 보유 중인 리스 */
    uint64_t lead_next_renew_ms;   /* 다음 하트비트 시각 (단조 시계 ms) */
    uint64_t lead_last_renew_ms;   /* 마지막으로 리스를 성공적으로 썼던 시각 (단조 시계 ms).
                                      이후 리스 길이만큼 지났으면 FS상 만료됐을 수 있으므로
                                      리더십을 내려놓고 election.lock을 거쳐 다시 얻는다. */
};

/* ---- 유틸 (fq_util.c) ---- */
uint64_t fq_now_wall_ms(void);                 /* 벽시계 ms (메시지 정렬용) */
uint64_t fq_now_mono_ms(void);                 /* 단조 시계 ms (로컬 경과 시간 측정용, 시계 점프 무관) */
uint64_t fq_lease_ms(void);                    /* 유효 리스(ms). FQ_LEASE_MS_OVERRIDE 존중 */
uint64_t fq_election_stale_ms(void);           /* election.lock 회수 임계 = min(FQ_ELECTION_STALE_MS, 리스/3) */

/* 교체형 rename으로 갱신되는 파일(leader.info) 읽기. ENOENT면 1ms 간격으로 몇 번 다시 읽는다. */
#define FQ_REPLACE_READ_RETRIES 5
int      fq_read_replaced(const char *path, void **out, size_t *out_len);
/* 파일명에 들어가는 식별자 검증: [A-Za-z0-9._-], 1..max_len자, "__" 금지. 1=유효 */
int      fq_valid_ident(const char *s, size_t max_len);
uint32_t fq_pid(void);
void     fq_gen_id(char *buf, size_t n);       /* 유일 토큰 hex (pid+salt+카운터+time). 스레드 안전 */
void     fq_default_node_id(char *buf, size_t n); /* "<호스트명>-<pid>" (식별자 규칙에 맞게 정리) */
void     fq_sleep_ms(unsigned ms);
/* 경로 앞의 만들 수 없는 접두부 길이: 드라이브 "X:"는 2, UNC "\\srv\share"·"//srv/share"는
 * share 뒤 구분자의 위치, 그 밖에는 0. fq_fs_mkdirs가 이 부분을 건너뛴다. */
size_t   fq_path_root_len(const char *path);
char    *fq_strdup(const char *s);

/* "<root>/<sub>/<name>" 조합. name이 NULL이면 "<root>/<sub>". */
void fq_path(char *out, size_t n, const char *root, const char *sub, const char *name);

/* 메시지 이름에서 재시도 접미사 ".a<N>"를 파싱.
 * base_out에는 접미사를 제거한 논리 이름을, *attempt_out에는 N(없으면 0)을 채움. */
void fq_parse_attempt(const char *name, char *base_out, size_t base_n, uint32_t *attempt_out);

/* inflight 이름 "<logical>__t<token>" 에서 token을 파싱. 실패 시 0 반환, logical_out 채움. */
uint64_t fq_parse_inflight(const char *name, char *logical_out, size_t logical_n);

/* ---- FS 시각 추정 (fq_queue.c) ----
 * 로컬 벽시계 + (FS 시각 - 벽시계) 오프셋. 오프셋은 FQ_CLOCK_SYNC_MS마다 fq_fs_now_ms로 다시 잰다.
 * FS에 파일을 만들지 않으므로 잦은 호출에 쓴다. 정확해야 하는 판정(리스 인수)에는 쓰지 말 것. */
uint64_t fq_fs_clock_est_ms(fq_queue *q);

#endif /* FQ_INTERNAL_H */
