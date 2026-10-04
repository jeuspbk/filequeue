/* fq.h - filequeue public API
 *
 * 공유 디스크 기반 Active-Passive 파일 큐. At-least-once 전달.
 * 설계 근거는 DESIGN.md 참고.
 */
#ifndef FQ_H
#define FQ_H

#include <stddef.h>
#include <stdint.h>

/* 반환 코드: 0(FQ_OK) 성공, 음수 실패 */
#define FQ_OK         0
#define FQ_ERR       -1   /* 일반 오류 */
#define FQ_EEXIST    -2   /* 대상이 이미 존재 (rename/create 경합) */
#define FQ_ENOENT    -3   /* 대상 없음 (claim 경합에서 패배 등) */
#define FQ_EEMPTY    -4   /* 큐가 비어 있음 */
#define FQ_ELOCKED   -5   /* 락/리더십을 다른 쪽이 보유 */
#define FQ_ENOLEADER -6   /* 리더십을 잃음 (fencing) */
#define FQ_EINVAL    -7   /* 잘못된 인자 (node_id / stream_key 형식) */

/* 튜닝 노브 */
#define FQ_MAX_ATTEMPTS    5      /* 초과 시 DLQ(dead/)로 이동 */
#define FQ_LEASE_MS        15000  /* 리스 유효시간 */
#define FQ_HEARTBEAT_MS    3000   /* 리더 하트비트 주기 (LEASE의 1/3~1/5 권장) */
#define FQ_ELECTION_STALE_MS 30000/* election.lock 강제 회수 임계의 상한. 실제 = min(이 값, 리스/3):
                                     하트비트도 이 락을 쓰므로 리스가 끝나기 전에 회수돼야 한다 */

typedef struct fq_queue fq_queue;

/* 리더십 리스 토큰 (fencing). token은 단조 증가. */
typedef struct {
    char     leader_id[64];
    uint64_t token;
    uint64_t lease_expiry_ms;   /* FS 시각 기준 */
} fq_lease;

/* claim된 메시지 핸들. data는 fq_ack/fq_nack/fq_msg_free 시 해제된다. */
typedef struct {
    char     name[768];   /* inflight 파일명 (root 기준 상대) */
    void    *data;        /* 페이로드 (소유) */
    size_t   len;
    uint32_t attempt;     /* 현재까지 재처리 시도 횟수 */
} fq_msg;

/* ---- 생애주기 ---- */
/* node_id: 이 노드의 식별자. 메시지 파일명과 리더십 id에 함께 쓰인다.
 *          NULL이면 환경변수 FQ_NODE_ID, 그것도 없으면 "<호스트명>-<pid>"로 자동 설정.
 *          node_id가 같은 프로세스가 둘이어도 리더십은 핸들마다 구분된다(leader.info의 instance).
 *          허용 문자: 영문자·숫자·'.'·'-'·'_' (1~63자, "__" 금지). 어기면 FQ_EINVAL.
 *          공백은 leader.info 파싱을, "__"는 inflight 이름의 "__t<token>" 파싱을, '/'는 경로를 깨뜨린다.
 * root:    NULL·빈 문자열이거나 900자를 넘으면 FQ_EINVAL (하위 경로가 버퍼에서 잘리지 않도록).
 * fq_close: fq_consume으로 얻은 리더십을 쥐고 있으면 반납한다(대기 노드가 리스 만료를 기다리지 않음).
 *          저수준 API(fq_acquire_leadership)로 얻은 리스는 fq_release_leadership으로 직접 반납할 것. */
int  fq_open(const char *root, const char *node_id, fq_queue **out);
void fq_close(fq_queue *q);

/* ---- Producer ---- */
/* stream_key: NULL 허용. 같은 key는 같은 파티션으로 묶여 순서 보존에 사용 가능.
 *             node_id와 같은 문자 규칙(1~127자). 어기면 FQ_EINVAL (fq_adopt도 동일). */
int  fq_publish(fq_queue *q, const void *data, size_t len, const char *stream_key);

/* ---- Consumer / 리더십 (Active-Passive) ---- */
/* node_id는 fq_open에서 설정한 q->node_id를 사용한다. */
int  fq_acquire_leadership(fq_queue *q, fq_lease *lease);
/* fq_renew_lease: 하트비트. election.lock 안에서 갱신한다.
 *   FQ_OK 연장됨, FQ_ENOLEADER 리더십을 잃음, FQ_ELOCKED 선출이 진행 중이라 이번엔 못 함
 *   (리더십은 아직 유지될 수 있음 → 잠시 후 재시도), FQ_ERR 오류. */
int  fq_renew_lease(fq_queue *q, fq_lease *lease);
int  fq_recover_stale(fq_queue *q, const fq_lease *lease);    /* 선임자 inflight 회수 */
/* 리더십 반납: 아직 내 리스면 만료시각을 0으로 써서 대기 노드가 곧바로 인수하게 한다(token 유지).
 * FQ_OK 반납됨, FQ_ENOLEADER 이미 내 리스가 아님, FQ_ELOCKED 선출 진행 중(재시도), FQ_ERR 오류. */
int  fq_release_leadership(fq_queue *q, const fq_lease *lease);
int  fq_claim(fq_queue *q, const fq_lease *lease, fq_msg **out); /* FQ_EEMPTY 가능 */
/* fq_ack: inflight unlink (처리 완료). m은 항상 해제된다.
 *   FQ_ENOENT = 파일이 이미 없음: 리스를 놓쳐 그 사이 회수됐다면 이 메시지는 다시 전달된다
 *   (중복 처리 신호. 소비자가 멱등이면 무해). */
int  fq_ack(fq_queue *q, fq_msg *m);
int  fq_nack(fq_queue *q, fq_msg *m);   /* 즉시 requeue (attempt+1) */
void fq_msg_free(fq_msg *m);

/* ---- 골라서 소비 ----
 * fq_claim_if: incoming을 오래된 순으로 보며 want(data, len, ud)가 1을 돌려주는 첫 메시지를
 *              claim한다(0이면 건너뜀). 후보마다 파일을 읽으므로 fq_claim보다 비싸다.
 *              FQ_EEMPTY = 원하는 메시지가 없음.
 * fq_release : claim한 메시지를 원래 이름 그대로 incoming에 되돌린다(attempt·순서 유지:
 *              들여다보기 용). 성공하면 m은 해제된다. */
typedef int (*fq_want_fn)(const void *data, size_t len, void *ud);
int  fq_claim_if(fq_queue *q, const fq_lease *lease, fq_want_fn want, void *ud, fq_msg **out);
int  fq_release(fq_queue *q, fq_msg *m);

/* ---- 통합 소비 wrapper ---- */
/* 리더십 획득(또는 유지·갱신) → 인수 시 stale 복구 → 하트비트 → claim 을 한 번에 처리한다.
 * 호출자는 다음 메시지만 받으면 되고, 리더십/리스 관리는 큐 핸들 내부 상태로 자동 처리된다.
 * 반환:
 *   FQ_OK      : *out 에 메시지. 처리 후 반드시 fq_ack / fq_nack 호출.
 *   FQ_EEMPTY  : 내가 활성 리더이나 큐가 비어 있음.
 *   FQ_ELOCKED : 다른 노드가 활성 리더(이 노드는 대기 상태). 잠시 후 재호출하면 됨.
 *   FQ_ERR     : 오류.
 * 노드 식별자는 fq_open에서 설정한 q->node_id를 사용한다. */
int  fq_consume(fq_queue *q, fq_msg **out);
/* fq_consume과 같되 fq_claim_if로 고른다. */
int  fq_consume_if(fq_queue *q, fq_want_fn want, void *ud, fq_msg **out);
/* fq_consume 사용자용 하트비트. 메시지 하나를 처리하는 데 리스(FQ_LEASE_MS)보다 오래 걸릴 수
 * 있으면 처리 도중 주기적으로 호출해 리스를 연장한다(즉시 갱신, 주기 무시).
 * FQ_OK = 연장됨, FQ_ELOCKED = 리더가 아님(리더십을 잃었거나 아직 없음),
 * FQ_ERR = 오류 또는 선출 락이 잠시 잡혀 있음(리더십은 유지 → 다시 호출하면 된다). */
int  fq_heartbeat(fq_queue *q);

/* ---- 2단계 발행 / 꺼내기 (트랜잭션 큐용) ----
 * 트랜잭션 관리자 쪽이 큐 밖(같은 파일시스템)의 파일에 메시지를 보관했다가
 * 결정에 따라 원자적 rename 한 번으로 큐에 넣거나 뺀다. 둘 다 원본이 옮겨지면
 * 사라지므로, 크래시 후 같은 호출을 다시 해도 중복이 생기지 않는다(멱등).
 *
 * fq_adopt: 이미 영속된(fsync된) 파일 path를 새 메시지로 큐에 넣는다(발행 시각은 지금).
 *           FQ_ENOENT = path가 없음(이미 넣었음).
 * fq_take : claim한 메시지 m을 큐에서 꺼내 path로 옮긴다(ack 대신). 성공하면 m은 해제되고
 *           path의 디렉터리를 fsync한다. 실패하면 m은 claim 상태 그대로(fq_nack 가능).
 *           POSIX에서는 link+unlink 두 단계라, 그 사이에 크래시하면 path와 inflight에 같은
 *           메시지가 남고 inflight 쪽은 나중에 복구되어 다시 전달된다(at-least-once 범위의 중복).
 * fq_return: fq_take로 꺼낸 파일을 fq_nack처럼 되돌린다. attempt = 꺼낼 때의 m->attempt;
 *           attempt+1로 incoming에 넣고, FQ_MAX_ATTEMPTS에 닿으면 dead/로 옮긴다.
 *           이름은 새로 붙는다(발행 시각 = 지금, stream_key 없음): 원래 순서와 파티션은 유지되지 않는다.
 *           FQ_ENOENT = path가 없음(이미 되돌렸음). */
int  fq_adopt(fq_queue *q, const char *path, const char *stream_key);
int  fq_take(fq_queue *q, fq_msg *m, const char *path);
int  fq_return(fq_queue *q, const char *path, uint32_t attempt);

/* ---- 유지보수 ---- */
/* 크래시 잔재 중 mtime이 tmp_max_age_ms보다 오래된 것 정리: tmp/의 모든 파일(발행 중 크래시),
 * control/의 .now-*(FS 시각 측정), leader-*.tmp(leader.info 교체 중), election.lock.stale-*.
 * tmp_max_age_ms는 정상 발행·하트비트 한 번보다 충분히 길게(예: 수 분) 줄 것: 진행 중인 임시
 * 파일을 지우면 그 발행이나 하트비트가 실패한다.
 * 반환: 삭제한 개수(>=0), 오류 시 음수. */
int  fq_gc(fq_queue *q, uint64_t tmp_max_age_ms);

#endif /* FQ_H */
