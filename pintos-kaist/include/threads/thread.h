#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>              // 디버그 관련 매크로 및 함수 포함
#include <list.h>               // 리스트 자료구조 관련 함수 포함
#include <stdint.h>             // 정수형 타입 정의 포함
#include "threads/interrupt.h"  // 인터럽트 관련 정의 포함

#ifdef VM
#include "vm/vm.h"              // 가상 메모리 관련 정의 포함 (VM이 정의되어 있을 경우)
#endif

void thread_sleep(int64_t ticks);
void thread_awake(int64_t ticks);

/* 스레드 생명 주기 상태 열거형 정의 */
enum thread_status {
  THREAD_RUNNING,     /* 현재 실행 중인 스레드 */
  THREAD_READY,       /* 실행 준비가 된 스레드 */
  THREAD_BLOCKED,     /* 이벤트를 기다리며 블록된 스레드 */
  THREAD_DYING        /* 곧 종료될 예정인 스레드 */
};

/* 스레드 식별자 타입 정의 */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)  /* TID 에러 값을 -1로 정의 */

/* 스레드 우선순위 관련 매크로 */
#define PRI_MIN 0              /* 최소 우선순위 값 */
#define PRI_DEFAULT 31         /* 기본 우선순위 값 */
#define PRI_MAX 63             /* 최대 우선순위 값 */

/* 커널 스레드 또는 사용자 프로세스를 나타내는 구조체 정의 */
/*
 * 각 스레드 구조체는 4KB의 페이지에 저장됨.
 * 구조체 자체는 페이지의 맨 아래(0바이트)에 위치.
 * 페이지의 나머지 공간은 해당 스레드의 커널 스택 용도로 사용되며,
 * 위쪽(4KB)에서 아래쪽으로 자람.
 *
 * 구조는 아래와 같음:
 *
 *   4 kB +-----------------------------+
 *        |       커널 스택 영역         |
 *        |          ...              |
 *        |         (아래로 성장)         |
 *        +-----------------------------+
 *        |           magic           | <- 스택 오버플로 감지용
 *        |         intr_frame        | <- 인터럽트 프레임 저장
 *        |             :             |
 *        |             :             |
 *        |            name           | <- 스레드 이름
 *        |           status          | <- 스레드 상태
 *   0 kB +-----------------------------+
 *
 * 주의할 점:
 * 1. 구조체 크기가 너무 크면 커널 스택 공간이 부족해짐.
 *    커널 함수에서는 큰 지역변수 사용 금지 → malloc() 또는 palloc 사용 권장
 * 2. 스택 오버플로가 발생하면 magic 값이 변경되어 에러 발생
 */

/*
 * elem은 이중 용도로 사용됨:
 * - 스레드가 준비 상태일 경우: run queue에 포함됨 (thread.c)
 * - 블록 상태일 경우: 세마포어 대기 리스트에 포함됨 (synch.c)
 * 두 상태는 동시에 발생하지 않으므로 elem을 겹쳐 사용 가능
 */


struct thread {
  /* thread.c 에서 사용하는 필드들 */
  tid_t tid;                          /* 스레드 식별자 */
  enum thread_status status;         /* 스레드 상태 */
  char name[16];                     /* 스레드 이름 (디버깅용) */
  int priority;                      /* 스레드 우선순위 */
  int64_t wakeup;// 지역변수 선언 웨이크업 틱
  /* thread.c 와 synch.c 에서 공유하는 필드 */
  struct list_elem elem;             /* 리스트 요소 (큐/세마포어 등에서 사용) */
  int init_priority; // 스레드가 원래 가지고 있던 초기 우선순위 (기부받기 전 값)
  struct lock *wait_on_lock; // 현재 이 스레드가 얻으려고 기다리는 락 (기부의 대상자 찾기용)
  struct list donations; // 다른 스레드들이 이 스레드에게 기부한 우선순위 정보 리스트
  struct list_elem donations_elem; // 이 스레드가 다른 스레드의 donations 리스트에 들어갈 때 쓰이는 리스트 노드

#ifdef USERPROG
  /* userprog/process.c 에서 사용하는 필드 */
  uint64_t *pml4;                    /* 사용자 주소 공간 (4단계 페이지 테이블 포인터) */
#endif

#ifdef VM
  /* 해당 스레드가 소유한 전체 가상 메모리 구조 */
  struct supplemental_page_table spt;
#endif

  /* thread.c 에서 사용하는 필드 */
  struct intr_frame tf;              /* 인터럽트 프레임 저장용 구조체 */
  unsigned magic;                    /* 스택 오버플로 감지용 값 (THREAD_MAGIC) */
};

/* 스케줄러 선택 매크로
 * false (기본값)일 경우 round-robin 스케줄러 사용
 * true일 경우 MLFQ (멀티레벨 피드백 큐) 스케줄러 사용
 * 이는 커널 명령줄 옵션 -o mlfqs 로 설정 가능 */
extern bool thread_mlfqs;

/* 스레드 시스템 초기화 함수 */
void thread_init (void);
/* 첫 번째 스레드를 시작시키는 함수 */
void thread_start (void);

/* 타이머 틱이 발생했을 때 호출되는 함수 */
void thread_tick (void);
/* 스레드 상태 통계 출력 함수 */
void thread_print_stats (void);

/* 새로운 스레드를 생성하는 함수 */
typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

/* 현재 스레드를 블록 상태로 전환하는 함수 */
void thread_block (void);
/* 블록 상태의 스레드를 언블록 상태로 전환하는 함수 */
void thread_unblock (struct thread *);

/* 현재 실행 중인 스레드를 반환하는 함수 */
struct thread *thread_current (void);
/* 현재 스레드의 tid 반환 */
tid_t thread_tid (void);
/* 현재 스레드의 이름 반환 */
const char *thread_name (void);

/* 현재 스레드를 종료시키는 함수 (리턴하지 않음) */
void thread_exit (void) NO_RETURN;
/* 현재 스레드가 CPU를 양보하는 함수 */
void thread_yield (void);

/* 현재 스레드의 우선순위 반환 */
int thread_get_priority (void);
/* 현재 스레드의 우선순위 설정 */
void thread_set_priority (int);

/* nice 값 관련 getter/setter */
int thread_get_nice (void);
void thread_set_nice (int);

/* 최근 사용한 CPU 시간 반환 */
int thread_get_recent_cpu (void);
/* 시스템 평균 부하 반환 */
int thread_get_load_avg (void);

/* 인터럽트 리턴 시 상태 복원 함수 */
void do_iret (struct intr_frame *tf);

void thread_sleep(int64_t ticks);
void thread_wakeup(int64_t ticks);
bool cmp_priority (const struct list_elem*a,const struct list_elem *b,void *aus UNUSED);
bool cmp_cond_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
bool cmp_d_priority(const struct list_elem *a,const struct list_elem *b,void *aux UNUSED);
void thread_test_preemption(void);
extern int64_t global_tick;
void donations_priority(void);
void refresh_priority(void);
void remove_with_lock(struct lock *lock);





#endif /* threads/thread.h */
