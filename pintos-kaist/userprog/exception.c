#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 처리된 페이지 폴트 수 */
static long long page_fault_cnt;

static void kill        (struct intr_frame *);
static void page_fault  (struct intr_frame *);

/* -------------------------------------------------------------
 * 사용자 프로그램이 야기할 수 있는 예외(interrupt) 핸들러 등록
 *
 * 실제 Unix 계열 OS는 이런 예외를 ‘시그널’ 형태로
 * 사용자 프로세스에 돌려보낸다([SV-386] 3-24, 3-25).
 * Pintos는 시그널을 구현하지 않으므로 일단 프로세스를 즉시 종료시킨다.
 *
 * 페이지 폴트(#PF)는 가상 메모리 구현 시 별도 처리해야 하므로
 * 지금은 다른 예외와 동일하게 다루지만 이후 수정해야 한다.
 *
 * 각 예외에 대한 자세한 설명은 [IA32-v3a] 5.15
 * “Exception and Interrupt Reference” 참고.
 * ----------------------------------------------------------- */
void
exception_init (void) {
	/* INT, INT3, INTO, BOUND 명령처럼
	   사용자 프로세스가 명시적으로 발생시킬 수 있는 예외.
	   DPL==3 → 사용자 수준에서 호출 허용 */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
	                   "#BR BOUND Range Exceeded Exception");

	/* DPL==0 → INT 명령으로 직접 호출 불가.
	   하지만 #DE(0으로 나누기)처럼 간접적으로 발생할 수 있음. */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
	                   "#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
	                   "#XF SIMD Floating-Point Exception");

	/* 페이지 폴트는 CR2 레지스터 내용을 보존해야 하므로
	   인터럽트를 끈 상태(INTR_OFF)에서 처리 등록 */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계 출력 */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* -------------------------------------------------------------
 * 사용자 프로세스가 일으킨(것으로 추정되는) 예외 핸들러
 * ----------------------------------------------------------- */
static void
kill (struct intr_frame *f) {
	/* 코드 세그먼트(CS) 값으로 예외 발생 위치 판별 */
	switch (f->cs) {
		case SEL_UCSEG:  /* 유저 영역 → 예상한 상황, 프로세스 종료 */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
			        thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:  /* 커널 영역 → 커널 버그, 즉시 패닉 */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:         /* 알 수 없는 세그먼트 → 역시 패닉 */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
			        f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* ----------------------------------------------------------------
 * 페이지 폴트 핸들러 (가상 메모리 구현 전용 골격 코드)
 *
 * • fault_addr  : CR2 레지스터의 폴트 주소
 * • error_code  : intr_frame 의 에러 코드 (PF_* 매크로 참고)
 * ---------------------------------------------------------------- */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* true → 존재하지 않는 페이지 접근 */
	bool write;        /* true → 쓰기 접근, false → 읽기 접근 */
	bool user;         /* true → 유저 모드, false → 커널 모드 */
	void *fault_addr;  /* 폴트가 발생한 가상 주소 */

	/* CR2에서 폴트 주소 읽기
	   (f->rip이 아닌, 실제 접근 주소일 수도 있음) */
	fault_addr = (void *) rcr2 ();

	/* CR2 내용이 바뀌기 전에 인터럽트 재허용 */
	intr_enable ();

	/* 원인 분석 */
	not_present = (f->error_code & PF_P) == 0;
	write       = (f->error_code & PF_W) != 0;
	user        = (f->error_code & PF_U) != 0;

#ifdef VM
	/* 프로젝트 3(가상 메모리) 이후 처리 */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* 폴트 카운터 증가 */
	page_fault_cnt++;

	/* 복구 불가한 폴트면 정보 출력 후 종료 */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
	        fault_addr,
	        not_present ? "not present" : "rights violation",
	        write       ? "writing"      : "reading",
	        user        ? "user"         : "kernel");
	kill (f);
}