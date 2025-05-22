#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread 의 `magic` 멤버에 넣는 임의 값.
   스택 오버플로 감지용. 자세한 내용은 thread.h 맨 위 주석 참고. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드용 임의 값
   수정하지 말 것. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태(실행 대기)의 프로세스 목록 */
static struct list ready_list;

/* THREAD_BLOCKED‧sleep 상태 스레드들을 위한 리스트 */
static struct list sleep_list;

/* Idle 스레드 */
static struct thread *idle_thread;

/* 초기 스레드(init.c:main()을 수행) */
static struct thread *initial_thread;

/* allocate_tid()에서 쓰는 락 */
static struct lock tid_lock;

/* 파괴 요청이 들어온 스레드 목록 */
static struct list destruction_req;

//static struct list child_list;.//////

/* 통계용 카운터 */
static long long idle_ticks;    /* Idle 상태로 보낸 타이머 틱 수 */
static long long kernel_ticks;  /* 커널 스레드에서 보낸 타이머 틱 수 */
static long long user_ticks;    /* 사용자 프로세스에서 보낸 타이머 틱 수 */

/* 스케줄링 관련 상수·변수 */
#define TIME_SLICE 4            /* 스레드 당 부여하는 타이머 틱 */
static unsigned thread_ticks;   /* 마지막 yield 이후 지난 틱 수 */

/* 기본 = 라운드 로빈, "-o mlfqs" 옵션 시 MLFQ 스케줄러 */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule (int status);
static void schedule (void);
static tid_t allocate_tid (void);

static bool insert_less (const struct list_elem *a,
                         const struct list_elem *b, void *aux);
static bool cmp_priority (const struct list_elem *a,
                          const struct list_elem *b, void *aux);

int64_t global_tick = INT64_MAX;

/* T가 유효한 스레드를 가리키면 true */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드 반환.
   CPU 스택 포인터 rsp를 읽어 페이지 시작 주소로 내림(align)한다.
   struct thread 는 항상 페이지 맨 앞에 있으므로
   이렇게 하면 현재 스레드를 얻을 수 있다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


/* thread_start 전 임시로 쓸 GDT
   thread_init 이후 실제 GDT를 다시 세팅한다. */
static uint64_t gdt[3] =
	{ 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* ---------------------------------------------------------------------
 *  스레드 서브시스템 초기화
 *    • 현재 실행 중인 코드를 스레드 객체로 변환
 *    • ready_list · tid_lock 등 초기화
 *    • allocator(page) 초기화 전까지 thread_create 금지
 * ------------------------------------------------------------------ */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널 전용 임시 GDT 로드 (유저 세그먼트 없음) */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 구조 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);
	list_init (&child_list);

	/* 현재 실행 중인 코드를 'main' 스레드로 등록 */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* ------------------------------------------------------------------
 *  thread_start
 *    • idle 스레드 생성
 *    • 인터럽트(선점 스케줄러) 활성화
 *    • idle 스레드 초기화 완료까지 대기
 * ----------------------------------------------------------------- */
void
thread_start (void) {
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	intr_enable ();          /* 선점 스케줄링 시작 */

	sema_down (&idle_started);/* idle 초기화 완료 대기 */
}

/* ------------------------------------------------------------------
 *  타이머 인터럽트마다 호출
 *  통계 갱신 + time slice 초과 시 선점 요청
 * ----------------------------------------------------------------- */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계 업데이트 */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* time slice 끝났으면 선점 */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* ---------------------------------------------------------------
 * 현재 스레드를 sleep_list 로 옮기고 BLOCK 상태로 전환.
 * new_local_tick 시각까지 잠든다.
 * ------------------------------------------------------------ */
void
thread_sleep (int64_t new_local_tick) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());
	old_level = intr_disable ();

	if (global_tick == INT64_MAX || global_tick > new_local_tick)
		global_tick = new_local_tick;

	if (curr != idle_thread) {
		curr->wakeup_tick = new_local_tick;
		list_insert_ordered (&sleep_list, &curr->elem, insert_less, NULL);
		thread_block ();
	}
	intr_set_level (old_level);
}

/* wakeup_tick 오름차순 비교 함수 (sleep_list 정렬용) */
static bool
insert_less (const struct list_elem *a,
             const struct list_elem *b, void *aux UNUSED) {
	struct thread *ta = list_entry (a, struct thread, elem);
	struct thread *tb = list_entry (b, struct thread, elem);
	return ta->wakeup_tick <= tb->wakeup_tick;
}

/* tick 시각에 깨워야 할 스레드들을 READY 상태로 */
void
wakeup_thread (int64_t tick) {
	struct list_elem *e = list_begin (&sleep_list);
	enum intr_level old_level = intr_disable ();

	while (e != list_end (&sleep_list)) {
		struct thread *t = list_entry (e, struct thread, elem);
		struct list_elem *next = list_next (e);

		if (t->wakeup_tick <= tick) {
			list_remove (e);
			thread_unblock (t);
			e = next;      /* 다음 요소로 이동 */
		} else
			break;         /* 정렬 리스트 특성: 이후는 모두 더 큼 */
	}

	/* global_tick 갱신 */
	if (!list_empty (&sleep_list)) {
		struct thread *head =
			list_entry (list_front (&sleep_list), struct thread, elem);
		global_tick = head->wakeup_tick;
	} else
		global_tick = INT64_MAX;

	intr_set_level (old_level);
}

/* 통계 출력 */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
	        idle_ticks, kernel_ticks, user_ticks);
}

/* ----------------------------------------------------------------
 * thread_create
 *   • NAME, PRIORITY, 실행 함수 FUNCTION(AUX) 로 새로운 커널 스레드 생성
 *   • ready_list 에 넣고 TID 반환
 *   • thread_start() 이후에는 생성 직후 스케줄될 수도 있음
 *   • Priority scheduling(Problem 1-3)은 아직 미구현
 * -------------------------------------------------------------- */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 구조체 페이지 할당 */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 초기화 */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 커널 스레드 entry 설정
	   rdi → 첫 번째 인자, rsi → 두 번째 인자 */
	t->tf.rip     = (uintptr_t) kernel_thread;
	t->tf.R.rdi   = (uint64_t) function;
	t->tf.R.rsi   = (uint64_t) aux;
	t->tf.ds      = SEL_KDSEG;
	t->tf.es      = SEL_KDSEG;
	t->tf.ss      = SEL_KDSEG;
	t->tf.cs      = SEL_KCSEG;
	t->tf.eflags  = FLAG_IF;

	/* ready 큐에 추가 */
	thread_unblock (t);

	/* 현재 스레드보다 우선순위 높으면 즉시 양보 */
	preempt_priority ();
	#ifdef USERPROG
		t->fd_idx = 3;
		t->fdt = palloc_get_multiple(PAL_ZERO,FDT_PAGES);
		if (t->fdt == NULL)
			return TID_ERROR;
	#endif

	return tid;
}

/* ------------------------------------------------------------
 * thread_block
 *   • 현재 스레드를 BLOCKED 로 바꾸고 스케줄러 호출
 *   • 인터럽트 OFF 상태여야 함
 * ---------------------------------------------------------- */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* ------------------------------------------------------------
 * thread_unblock
 *   • BLOCKED 스레드 T를 READY 상태로
 *   • 직접 선점(스케줄)은 하지 않음
 * ---------------------------------------------------------- */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);

	list_insert_ordered (&ready_list, &t->elem, cmp_priority, NULL);
	t->status = THREAD_READY;

	intr_set_level (old_level);
}

/* 우선순위 비교 함수 (내림차순) */
static bool
cmp_priority (const struct list_elem *a,
              const struct list_elem *b, void *aux UNUSED) {
	struct thread *ta = list_entry (a, struct thread, elem);
	struct thread *tb = list_entry (b, struct thread, elem);
	return ta->priority > tb->priority;
}

/* 현재 실행 중인 스레드 이름 반환 */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* thread_current + 유효성 검증 */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 현재 스레드 tid 반환 */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* ------------------------------------------------------------
 * thread_exit  (절대 복귀하지 않음)
 *   • USERPROG: process_exit() 호출
 *   • 상태 DYING 으로 바꾸고 스케줄러 호출
 * ---------------------------------------------------------- */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU 양보 (READY 상태로 넣고 스케줄) */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem,
		                     cmp_priority, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재보다 높은 우선순위 READY 스레드가 있으면 선점 */
void
preempt_priority (void) {
	if (thread_current () == idle_thread || list_empty (&ready_list))
		return;

	struct thread *curr  = thread_current ();
	struct thread *front =
		list_entry (list_front (&ready_list), struct thread, elem);

	if (curr->priority < front->priority)
		thread_yield ();
}

/* 현재 스레드 우선순위 갱신 */
void
thread_set_priority (int new_priority) {
	thread_current ()->original_priority = new_priority;
	update_donations_priority (); /* donation 로직 함수 (다른 파일) */
	preempt_priority ();
}

/* 현재 스레드 우선순위 조회 */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* ----- 이하 NICE, LOAD_AVG, RECENT_CPU(MLFQS) 미구현 ----- */
void thread_set_nice (int nice UNUSED) {}
int  thread_get_nice (void)         { return 0; }
int  thread_get_load_avg (void)     { return 0; }
int  thread_get_recent_cpu (void)   { return 0; }

/* ------------------------------------------------------------------
 * idle 스레드
 *   • ready_list 가 비면 next_thread_to_run() 에서 반환
 *   • thread_start() 단계에서 한 번만 READY 상태로 들어감
 * ---------------------------------------------------------------- */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드에게 CPU 양보 */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 켜고 다음 인터럽트까지 HLT */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드 엔트리 래퍼 */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();
	function (aux);
	thread_exit ();
}

/* ------------------------------------------------------------
 * init_thread
 *   • BLOCKED 상태로 구조체 T 기본 초기화
 * ---------------------------------------------------------- */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp  = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	t->original_priority = priority;
	t->wait_on_lock = NULL;
	list_init (&t->donations);
	list_init(&t->child_list);
#ifdef USERPROG
t->exit_status = 0;
#endif
}

/* run queue가 비어 있으면 idle 반환 */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list),
		                   struct thread, elem);
}



/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
int
find_descriptor(struct thread* t){
	for(int i=3;i<128;i++){
		if(t->file_table[i] == NULL){
			return i;
		}
	}
	return -1;
}

struct file*
is_open_file(struct thread* t, int fd){
	if(t->file_table[fd] != NULL){
		return t->file_table[fd];
	}else{
		return NULL;
	}
}
