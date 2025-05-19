#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* 스레드 생애 주기의 상태 값 */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드 */
	THREAD_READY,       /* 실행 대기 중인 스레드 */
	THREAD_BLOCKED,     /* 이벤트를 기다리며 블록된 스레드 */
	THREAD_DYING        /* 곧 파괴될(소멸 직전) 스레드 */
};

/* 스레드 식별자 타입
   필요하다면 다른 형식으로 재정의해도 무방하다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t 오류 값 */

/* 스레드 우선순위 범위 */
#define PRI_MIN 0                       /* 최소 우선순위 */
#define PRI_DEFAULT 31                  /* 기본 우선순위 */
#define PRI_MAX 63                      /* 최대 우선순위 */

/* 커널 스레드 또는 사용자 프로세스
 *
 * 각 스레드 구조체는 **자신만의 4 kB 페이지** 안에 배치된다.
 * 구조체 자체는 페이지의 맨 아래(오프셋 0)에 위치하고,
 * 나머지 공간은 스레드의 **커널 스택** 용도로 예약된다.
 * 스택은 페이지 상단(4 kB)부터 아래쪽으로 성장한다.
 *
 *      4 kB +---------------------------------+
 *           |          커널 스택               |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         아래쪽으로 성장          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이 설계에는 두 가지 중요한 함의가 있다.
 *
 * 1. **`struct thread`가 너무 커지면 안 된다.**
 *    커지면 커널 스택 공간이 부족해진다. 기본 구조체는 몇 바이트뿐이며,
 *    1 kB를 넘기지 않는 편이 안전하다.
 *
 * 2. **커널 스택도 과도하게 커져서는 안 된다.**
 *    스택 오버플로는 곧바로 스레드 상태를 망가뜨린다.
 *    그러므로 커널 함수에서 거대한 지역 배열‧구조체를 선언하지 말고,
 *    `malloc()`이나 `palloc_get_page()` 같은 동적 할당을 사용해야 한다.
 *
 * 두 문제의 첫 징후는 대개 `thread_current()`의 assertion 실패다.
 * 이 함수는 실행 중 스레드의 `magic` 값이 `THREAD_MAGIC`인지 확인하는데,
 * 스택이 넘치면 이 값이 변조되어 assert가 트리거된다.
 *
 * ----------------------------------------------------------------
 * `elem` 멤버는 두 가지 용도로 쓰인다.
 *   • run queue(thread.c)의 노드
 *   • 세마포어 wait list(synch.c)의 노드
 * 두 용도가 **동시에** 쓰이지 않는데,
 * run queue에는 READY 상태 스레드만,
 * wait list에는 BLOCKED 상태 스레드만 들어가기 때문이다.
 */
struct thread {
	/* thread.c 소유 영역 */
	tid_t tid;                          /* 스레드 ID */
	enum thread_status status;          /* 현재 상태 */
	char name[16];                      /* 디버깅용 이름 */
	int64_t wakeup_tick;                /* 깨울 시각(tick) */
	int priority;                       /* 현재 우선순위 */
	int original_priority;              /* 기부 이전 고유 우선순위 */
	struct list donations;              /* 기부받은 우선순위 목록 */
	struct lock *wait_on_lock;          /* 대기 중인 락 */
	struct list_elem donation_elem;     /* donations 리스트용 elem */

	/* thread.c & synch.c 공동 사용 영역 */
	struct list_elem elem;              /* run queue 혹은 wait list 노드 */

#ifdef USERPROG
	/* userprog/process.c 소유 영역 */
	uint64_t *pml4;                     /* 4단계 페이지 맵 */
#endif
#ifdef VM
	/* 해당 스레드가 소유한 전체 가상 메모리 정보 */
	struct supplemental_page_table spt;
#endif

	/* thread.c 소유 영역 */
	struct intr_frame tf;               /* 문맥 전환 정보 */
	unsigned magic;                     /* 스택 오버플로 감지용 값 */

	/* 시스템콜 */
	int exit_status;
};

extern int64_t global_tick;

/* 기본은 라운드 로빈,
   커맨드라인 "-o mlfqs" 옵션이 있으면 MLFQ 스케줄러 사용 */
extern bool thread_mlfqs;

/* 스레드 서브시스템 함수 선언 */
void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority,
                     thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void preempt_priority (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */