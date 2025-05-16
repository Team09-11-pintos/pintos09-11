#include "threads/thread.h" // PintOS 스레드 구조체와 스케줄러 인터페이스
#include <debug.h>            // ASSERT 매크로 등 디버그 유틸리티
#include <stddef.h>            // size_t, NULL 등의 기본 정의
#include <random.h>            // 난수 생성기(테스트용)
#include <stdio.h>             // printf 등 기본 입출력
#include <string.h>            // 문자열 처리 함수
#include "threads/flags.h"    // CPU 플래그 제어 헬퍼
#include "threads/interrupt.h"// 인터럽트 레벨 관리
#include "threads/intr-stubs.h"// 어셈블리 인터럽트 스텁
#include "threads/palloc.h"   // 페이지 할당기
#include "threads/synch.h"    // 동기화 프리미티브(세마포어·락 등)
#include "threads/vaddr.h"    // 가상 주소 매크로
#include "intrinsic.h"        // 저수준 어셈블리 헬퍼
#ifdef USERPROG
#include "userprog/process.h" // 사용자 프로세스 관리 루틴
#endif

/* struct thread 의 magic 필드에 저장되는 임의의 값.
   스택 오버플로우를 감지하는 데 사용된다. 자세한 내용은
   thread.h 상단의 큰 주석을 참조. */
#define THREAD_MAGIC 0xcd6abf4b // 스택 덮어쓰기 검사용 값

/* 기본(basic) 스레드용 임의의 값. 수정 금지. */
#define THREAD_BASIC 0xd42df210

/* READY 상태(실행 가능하지만 아직 CPU를 얻지 못한)의
   스레드들을 보관하는 리스트. */
static struct list ready_list; // 러너블 큐

/* idle 스레드(아무 일도 하지 않는 대기용 스레드)의 포인터. */
static struct thread *idle_thread;

/* 최초 스레드, 즉 init.c:main() 을 실행 중인 스레드 포인터. */
static struct thread *initial_thread;

/* allocate_tid() 에서 사용하는 락. */
static struct lock tid_lock;

/* 파괴 대기 중인 스레드들을 보관하는 리스트. */
static struct list destruction_req;

/* 슬립리스트*/
static struct list sleep_list;//

int64_t global_tick=0;




/* 통계용 카운터 */
static long long idle_ticks;   /* idle 스레드가 소비한 타이머 틱 */
static long long kernel_ticks; /* 순수 커널 스레드가 소비한 틱 */
static long long user_ticks;   /* 사용자 프로그램이 소비한 틱 */

/* 스케줄링 상수 및 변수 */
#define TIME_SLICE 4           /* 각 스레드에 할당할 타임슬라이스(틱) */
static unsigned thread_ticks;  /* 마지막 yield 이후 경과한 틱 */

/* false(기본) → 라운드로빈, true → MLFQ 스케줄러.
   커맨드라인 옵션 "-o mlfqs" 로 제어. */
bool thread_mlfqs;

/* 내부 함수 프로토타입 */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);
bool cmp_awake(const struct list_elem *a,const struct list_elem *b,void *aux UNUSED);
bool cmp_priority (const struct list_elem*a,const struct list_elem *b,void *aus UNUSED);
void thread_sleep(int64_t ticks);
void thread_awake(int64_t ticks);
void thread_test_preemption(void);


/* T 가 유효한 thread 구조체를 가리키면 true 반환. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드의 thread 구조체 주소를 얻는다.
 * CPU 스택 포인터(rsp)를 읽은 뒤 페이지 경계로 내림(pg_round_down)
 * 하면 struct thread 가 위치한 페이지 첫 주소가 나온다.
 * 각 스레드는 자신의 커널 스택과 같은 페이지에 thread 구조체가 있다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))

// thread_start 에서 사용할 임시 GDT.
// thread_init 이후 실제 GDT(gdt_init)가 들어오기 전까지 사용.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* =============================
   1. 스레딩 시스템 초기화
   ============================= */
/* 현재 실행 중인 코드를 스레드로 변환해 스레딩 시스템을 초기화.
   loader.S 가 스택 하단을 페이지 경계에 맞춰 두었기 때문에 가능하다.

   - run queue 와 tid_lock 초기화
   - page allocator 초기화 전에 반드시 호출돼야 함
   - 함수가 끝나기 전까지 thread_current() 사용 금지 */
void thread_init (void) {
    ASSERT (intr_get_level () == INTR_OFF); // 인터럽트 비활성 상태 확인

    /* 커널용 임시 GDT 로드 (유저 영역 세그먼트 없음) */
    struct desc_ptr gdt_ds = {
        .size = sizeof (gdt) - 1,
        .address = (uint64_t) gdt
    };
    lgdt (&gdt_ds); // GDTR 레지스터에 로드

    /* 글로벌 컨텍스트 초기화 */
    lock_init (&tid_lock);      // tid 할당용 락 생성
    list_init (&ready_list);    // ready 큐 초기화
    list_init (&destruction_req); // 파괴 대기 리스트 초기화
	
	list_init (&sleep_list);// 자는 리스트 초기화

    /* 현재 실행 중인 "main" 스레드 구조체 설정 */
    initial_thread = running_thread ();            // 현재 스택으로부터 thread 구조체 추출
    init_thread (initial_thread, "main", PRI_DEFAULT); // 기본 우선순위로 초기화
    initial_thread->status = THREAD_RUNNING;       // 상태 갱신
    initial_thread->tid = allocate_tid ();         // tid 부여
}

/* =============================
   2. 스레드 시스템 시작
   ============================= */
/* 선점형 스케줄링을 켜고 idle 스레드를 만든다. */
void thread_start (void) {
    /* idle 스레드 생성 */
    struct semaphore idle_started;
    sema_init (&idle_started, 0);                  // 초기값 0 세마포어
    thread_create ("idle", PRI_MIN, idle, &idle_started); // idle 스레드 실행

    /* 인터럽트(타이머)를 활성화 → 선점 시작 */
    intr_enable ();

    /* idle 스레드가 초기화 완료할 때까지 대기 */
    sema_down (&idle_started);
}

/* =============================3
   3. 타이머 틱 처리
   ============================= */
/* 타이머 인터럽트마다 호출. (인터럽트 컨텍스트) */
void thread_tick (void) {
    struct thread *t = thread_current ();

    /* 통계 업데이트 */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pml4 != NULL)
        user_ticks++;           // 유저 모드 스레드 실행 시간
#endif
    else
        kernel_ticks++;         // 커널 스레드 실행 시간

    /* 타임 슬라이스 소모 → 선점 여부 확인 */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return (); // 인터럽트 핸들러 복귀 시 yield
}

/* =============================
   4. 통계 출력
   ============================= */
void thread_print_stats (void) {
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

/* =============================
   5. 스레드 생성
   ============================= */
/* 새 커널 스레드(이름 NAME, 우선순위 PRIORITY)를 만들어 ready 큐에 넣는다.
   thread_start() 호출 이후라면, 새 스레드는 thread_create()가
   return 하기 전에도 스케줄될 수 있다. 따라서 동기화 필요 시 세마포어 등 사용.

   현재는 우선순위 스케줄링이 구현돼 있지 않지만
   Problem 1‑3 과제에서 구현 대상이다. */
tid_t thread_create (const char *name, int priority,
                     thread_func *function, void *aux) {
    struct thread *t;
    tid_t tid;

    ASSERT (function != NULL);

    /* 1) thread 구조체가 들어갈 새 페이지 할당 */
    t = palloc_get_page (PAL_ZERO); // 0으로 초기화된 페이지
    if (t == NULL)
        return TID_ERROR;

    /* 2) thread 구조체 초기화 */
    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    /* 3) 스레드 시작 시 kernel_thread() 실행하도록 setup
       rdi = function, rsi = aux (SysV AMD64 ABI) */
    t->tf.rip = (uintptr_t) kernel_thread; // 첫 진입점
    t->tf.R.rdi = (uint64_t) function;     // 실제 사용자 함수
    t->tf.R.rsi = (uint64_t) aux;          // 인자
    t->tf.ds = SEL_KDSEG; // 데이터 세그먼트
    t->tf.es = SEL_KDSEG;
    t->tf.ss = SEL_KDSEG;
    t->tf.cs = SEL_KCSEG; // 코드 세그먼트
    t->tf.eflags = FLAG_IF; // 인터럽트 플래그 on

    /* 4) ready 큐에 삽입 */
    thread_unblock (t);
    // if (t->priority>thread_current()->priority)
    //     thread_yield;
    thread_test_preemption();

    return tid;
}

/* =============================
   6. 스레드 상태 전환 API
   ============================= */
/* 현재 스레드를 BLOCKED 상태로 바꾸고 스케줄링에서 제외.
   인터럽트가 꺼진 상태(INTR_OFF)에서만 호출해야 안전.
   일반적으로 synch.h 의 동기화 프리미티브를 사용하는 편이 낫다. */
void thread_block (void) {
    ASSERT (!intr_context ());                  // 인터럽트 컨텍스트 아님
    ASSERT (intr_get_level () == INTR_OFF);     // 인터럽트 비활성
    thread_current ()->status = THREAD_BLOCKED; // 상태 갱신
    schedule ();                                // 다음 스레드로 교체
}

/* BLOCKED 스레드 T 를 READY 상태로 전환. (깨우기)
   호출 시 T 는 반드시 BLOCKED 여야 하며, 이 함수는 현재 스레드를
   선점하지 않는다. 이는 호출자가 인터럽트를 직접 끈 상태에서
   원자적으로 여러 구조를 갱신할 수 있게 하기 위함. */
void thread_unblock (struct thread *t) {
    enum intr_level old_level;

    ASSERT (is_thread (t));

    old_level = intr_disable ();           // 원자적 처리 위해 인터럽트 off
    ASSERT (t->status == THREAD_BLOCKED);
    list_insert_ordered (&ready_list, &t->elem,cmp_priority,NULL); // ready 큐 끝에 삽입
    //list_push_back(&ready_list, &t->elem);

    t->status = THREAD_READY;
    intr_set_level (old_level);            // 인터럽트 복원
}

/* =============================
   7. 편의 함수들
   ============================= */
const char * thread_name (void) {
    return thread_current ()->name;        // 현재 스레드 이름 반환
}

struct thread * thread_current (void) {
    struct thread *t = running_thread ();  // 스택으로부터 구조체 얻기

    /* 유효성 검사: magic 값·상태 체크 */
    ASSERT (is_thread (t));
    ASSERT (t->status == THREAD_RUNNING);
    return t;
}

tid_t thread_tid (void) {
    return thread_current ()->tid;         // 현재 스레드의 tid 반환
}

/* =============================
   8. 스레드 종료
   ============================= */
void thread_exit (void) {
    ASSERT (!intr_context ());             // 인터럽트 컨텍스트에서 호출 금지

#ifdef USERPROG
    process_exit ();                       // 유저 프로세스 정리
#endif

    /* 상태를 DYING 으로 두고 스케줄러에 제어권 반환.
       schedule_tail() 에서 실제 구조체가 파괴된다. */
    intr_disable ();
    do_schedule (THREAD_DYING);            // 스케줄 교체 + 상태 갱신
    NOT_REACHED ();                        // 이 이하로 절대 실행 안 됨
}

/* CPU 양보(yield). 현재 스레드를 READY 큐 뒤에 넣고 즉시 스케줄 변경. */
void thread_yield (void) {
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (cur != idle_thread)
        //list_push_back (&ready_list, &cur->elem); // 다시 ready 큐
        list_insert_ordered(&ready_list,&cur->elem,cmp_priority,NULL);
    do_schedule (THREAD_READY);                    // 상태 변경·스케줄
    intr_set_level (old_level);
}

/* =============================
   9. 우선순위 관련 (과제에서 확장)
   ============================= */
void thread_set_priority (int new_priority) {
    thread_current ()->priority = new_priority;    // 단순 대입 (기본 구현)
    thread_test_preemption();
}

int thread_get_priority (void) {
    return thread_current ()->priority;
}

void thread_set_nice (int nice UNUSED) {
    /* MLFQS 구현 과제 부분 */
}

int thread_get_nice (void) {
    return 0; // TODO: 구현
}

int thread_get_load_avg (void) {
    return 0; // TODO: 구현
}

int thread_get_recent_cpu (void) {
    return 0; // TODO: 구현
}

/* =============================
   10. idle 스레드 구현
   ============================= */
static void idle (void *idle_started_ UNUSED) {
    struct semaphore *idle_started = idle_started_;

    idle_thread = thread_current ();
    sema_up (idle_started);                // thread_start() 대기 해제

    for (;;) {
        /* CPU 양보 후 인터럽트 대기 → HLT 로 절전 */
        intr_disable ();
        thread_block ();                   // 자신을 BLOCKED 로

        /* 다음 인터럽트까지 대기. sti; hlt 는 원자적으로 실행됨. */
        asm volatile ("sti; hlt" : : : "memory");
    }
}

/* =============================
   11. kernel_thread 래퍼
   ============================= */
static void kernel_thread (thread_func *function, void *aux) {
    ASSERT (function != NULL);

    intr_enable ();      // 스케줄러는 인터럽트 off 상태에서 진입함
    function (aux);      // 실제 함수 실행
    thread_exit ();      // 함수가 리턴하면 스레드 종료
}

/* =============================
   12. thread 구조체 내부 초기화
   ============================= */
static void init_thread (struct thread *t, const char *name, int priority) {
    ASSERT (t != NULL);
    ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT (name != NULL);

    memset (t, 0, sizeof *t);              // 메모리 0으로 초기화
    t->status = THREAD_BLOCKED;            // 최초 상태 BLOCKED
    strlcpy (t->name, name, sizeof t->name); // 이름 복사
    t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *); // 최상단 스택 준비

    
    t->priority = t->init_priority = priority;                // 우선순위 설정
    list_init(&t->donations);
    t->wait_on_lock=NULL;
    t->magic = THREAD_MAGIC;               // 무결성 체크용 값
}

/* =============================
   13. 다음에 실행할 스레드 선택
   ============================= */
static struct thread * next_thread_to_run (void) {
    if (list_empty (&ready_list))
        return idle_thread;                // 아무것도 없으면 idle
    else
        return list_entry (list_pop_front (&ready_list), struct thread, elem); // FCFS
}

/* =============================
   14. x86-64 컨텍스트 스위치 헬퍼 (do_iret)
   ============================= */
void do_iret (struct intr_frame *tf) {
    __asm __volatile(
        "movq %0, %%rsp\n"         // 스택포인터를 새 intr_frame 으로 이동
        /* 일반 레지스터 복원 */
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
        "addq $120,%%rsp\n"        // intr_frame 구조체 크기 보정
        /* 세그먼트 복원 */
        "movw 8(%%rsp),%%ds\n"
        "movw (%%rsp),%%es\n"
        "addq $32, %%rsp\n"        // 레거시 세그먼트 영역 스킵
        "iretq"                      // 링레벨 전환 포함 복귀
        : : "g" ((uint64_t) tf) : "memory");
}

/* =============================
   15. 스레드 전환(thread_launch)
   ============================= */
static void thread_launch (struct thread *th) {
    uint64_t tf_cur = (uint64_t) &running_thread ()->tf; // 현재 intr_frame 주소
    uint64_t tf = (uint64_t) &th->tf;                   // 대상 스레드 intr_frame 주소
    ASSERT (intr_get_level () == INTR_OFF);

    /* 어셈블리 블록: 현재 컨텍스트 저장 후 do_iret 호출로 전환 */
    __asm __volatile (
        /* 레지스터 일부 스택에 저장 */
        "push %%rax\n"
        "push %%rbx\n"
        "push %%rcx\n"
        /* 입력 파라미터 준비 */
        "movq %0, %%rax\n"
        "movq %1, %%rcx\n"
        /* 현재 레지스터를 tf_cur 에 저장 */
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
        "pop %%rbx\n"              // rcx 복원
        "movq %%rbx, 96(%%rax)\n"
        "pop %%rbx\n"              // rbx 복원
        "movq %%rbx, 104(%%rax)\n"
        "pop %%rbx\n"              // rax 복원
        "movq %%rbx, 112(%%rax)\n"
        "addq $120, %%rax\n"
        "movw %%es, (%%rax)\n"
        "movw %%ds, 8(%%rax)\n"
        "addq $32, %%rax\n"
        "call __next\n"            // 현재 RIP 저장
        "__next:\n"
        "pop %%rbx\n"
        "addq $(out_iret -  __next), %%rbx\n" // 복귀 RIP 계산
        "movq %%rbx, 0(%%rax)\n"
        "movw %%cs, 8(%%rax)\n"
        "pushfq\n"
        "popq %%rbx\n"
        "mov %%rbx, 16(%%rax)\n" // EFLAGS
        "mov %%rsp, 24(%%rax)\n" // RSP
        "movw %%ss, 32(%%rax)\n"
        "mov %%rcx, %%rdi\n"      // 1st arg = tf
        "call do_iret\n"          // 실제 전환
        "out_iret:\n"
        : : "g"(tf_cur), "g" (tf) : "memory"
    );
}

/* =============================
   16. 스케줄러
   ============================= */
static void do_schedule(int status) {
    ASSERT (intr_get_level () == INTR_OFF);
    ASSERT (thread_current()->status == THREAD_RUNNING);

    /* destruction_req 리스트에 있는 죽은 스레드 메모리 회수 */
    while (!list_empty (&destruction_req)) {
        struct thread *victim = list_entry (list_pop_front (&destruction_req), struct thread, elem);
        palloc_free_page(victim);
    }

    thread_current ()->status = status; // 현재 스레드 상태 갱신
    schedule ();                        // 다음 스레드 선택
}

static void schedule (void) {
    struct thread *cur = running_thread ();
    struct thread *next = next_thread_to_run ();

    ASSERT (intr_get_level () == INTR_OFF);
    ASSERT (cur->status != THREAD_RUNNING);
    ASSERT (is_thread (next));

    next->status = THREAD_RUNNING; // 새 스레드 RUNNING 표시
    thread_ticks = 0;              // 새 타임슬라이스 시작

#ifdef USERPROG
    process_activate (next);      // 페이지 테이블 등 유저 주소공간 활성화
#endif

    if (cur != next) {
        /* DYING 스레드는 schedule 진입 시까지 구조체가 살아 있으므로
           여기에서 파괴를 예약하고, 실제 free 는 다음 타임에 수행. */
        if (cur && cur->status == THREAD_DYING && cur != initial_thread) {
            ASSERT (cur != next);
            list_push_back (&destruction_req, &cur->elem);
        }

        /* 컨텍스트 스위치 */
        thread_launch (next);
    }
}

/* =============================
   17. tid 할당기
   ============================= */
static tid_t allocate_tid (void) {
    static tid_t next_tid = 1;    // 정적 변수: 시스템 전체 고유 ID
    tid_t tid;

    lock_acquire (&tid_lock);     // 동시성 보호
    tid = next_tid++;
    lock_release (&tid_lock);

    return tid;
}
bool cmp_awake(const struct list_elem *a,const struct list_elem *b,void *aux UNUSED){
    struct thread *t_a=list_entry(a,struct thread,elem);
    struct thread *t_b=list_entry(b,struct thread,elem);
    return t_a->wakeup < t_b->wakeup;
}
// bool cmp_priority (struct list_elem*a,struct list_elem *b,void *aus UNUSED){
//     return list_entry (a,struct thread,elem)->priority > list_entry(b,struct thread,elem)->priority;
// }
bool cmp_priority(const struct list_elem *a,const struct list_elem *b,void *aux UNUSED){
    struct thread *st_a=list_entry(a,struct thread, elem);
    struct thread *st_b=list_entry(b,struct thread, elem);
    return st_a->priority > st_b->priority;
}

void thread_sleep(int64_t ticks){//스레드를 재우는 함수
	enum intr_level old_level=intr_disable();//이거 인터럽트 닫음
	struct thread *cur = thread_current();//cur 변수를 만들음

	ASSERT(cur != idle_thread);
	cur->wakeup=ticks;
	list_insert_ordered(&sleep_list,&cur->elem,cmp_awake,NULL);
    //list_push_back(&sleep_list,&cur->elem);
    if (global_tick == 0 || ticks < global_tick)
        global_tick = ticks;
	thread_block();//이 스레드는 블럭상태로 바꾼다
	intr_set_level(old_level);
	//schedule();//
}
void thread_awake(int64_t ticks){
    enum intr_level old_level = intr_disable(); // 인터럽트 비활성
	struct list_elem *e=list_begin(&sleep_list);
	while (e != list_end(&sleep_list)){
		struct thread *t = list_entry(e, struct thread, elem);
		if (t->wakeup<=ticks){
			// e=list_remove(e);
			thread_unblock(t);
            thread_test_preemption();
		}else{
			break;
            //e = list_next(e);
		}
	}
    intr_set_level(old_level); // 인터럽트 상태를 원래 상태로 변경
}
// void thread_test_preemption(void){
//     if(!list_empty (&ready_list)&&thread_current()->priority<list_entry(list_front(&ready_list),
//     struct thread,elem)->priority)
//         thread_yield();
// }
void thread_test_preemption(void){
    //ASSERT(thread_current()!=idle_thread);
    //ASSERT(!list_empty(&ready_list));
    if (thread_current() == idle_thread)
        return;
    if (list_empty(&ready_list))
        return;
    struct thread *cur  = thread_current();
    struct thread *ready = list_entry(list_front(&ready_list),struct thread,elem);
    if(cur->priority < ready->priority)
        thread_yield();
}
