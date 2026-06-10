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

/* 튜닝 노브 */
#define FQ_MAX_ATTEMPTS    5      /* 초과 시 DLQ(dead/)로 이동 */
#define FQ_LEASE_MS        15000  /* 리스 유효시간 */
#define FQ_HEARTBEAT_MS    3000   /* 리더 하트비트 주기 (LEASE의 1/3~1/5 권장) */
#define FQ_ELECTION_STALE_MS 30000/* election.lock 강제 회수 임계 */

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
 *          NULL이면 환경변수 FQ_NODE_ID, 그것도 없으면 "node-<pid>"로 자동 설정. */
int  fq_open(const char *root, const char *node_id, fq_queue **out);
void fq_close(fq_queue *q);

/* ---- Producer ---- */
/* stream_key: NULL 허용. 같은 key는 같은 파티션으로 묶여 순서 보존에 사용 가능. */
int  fq_publish(fq_queue *q, const void *data, size_t len, const char *stream_key);

/* ---- Consumer / 리더십 (Active-Passive) ---- */
/* node_id는 fq_open에서 설정한 q->node_id를 사용한다. */
int  fq_acquire_leadership(fq_queue *q, fq_lease *lease);
int  fq_renew_lease(fq_queue *q, fq_lease *lease);            /* 하트비트 */
int  fq_recover_stale(fq_queue *q, const fq_lease *lease);    /* 선임자 inflight 회수 */
int  fq_claim(fq_queue *q, const fq_lease *lease, fq_msg **out); /* FQ_EEMPTY 가능 */
int  fq_ack(fq_queue *q, fq_msg *m);    /* inflight unlink (처리 완료) */
int  fq_nack(fq_queue *q, fq_msg *m);   /* 즉시 requeue (attempt+1) */
void fq_msg_free(fq_msg *m);

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

/* ---- 유지보수 ---- */
/* tmp/의 고아 임시파일(발행 중 크래시 잔재) 중 mtime이 tmp_max_age_ms보다 오래된 것 정리.
 * 반환: 삭제한 개수(>=0), 오류 시 음수. */
int  fq_gc(fq_queue *q, uint64_t tmp_max_age_ms);

#endif /* FQ_H */
