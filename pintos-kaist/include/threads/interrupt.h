#ifndef THREADS_INTERRUPT_H
#define THREADS_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

/* 인터럽트 상태: 켜짐 또는 꺼짐 */
enum intr_level {
	INTR_OFF,             /* 인터럽트가 비활성화된 상태 */
	INTR_ON               /* 인터럽트가 활성화된 상태 */
};

/* 인터럽트 레벨 조회 및 설정 함수 원형 */
enum intr_level intr_get_level (void);
enum intr_level intr_set_level (enum intr_level);
enum intr_level intr_enable (void);
enum intr_level intr_disable (void);

/* 범용 레지스터 저장용 구조체 */
struct gp_registers {
	uint64_t r15;
	uint64_t r14;
	uint64_t r13;
	uint64_t r12;
	uint64_t r11;
	uint64_t r10;
	uint64_t r9;
	uint64_t r8;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t rbp;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t rbx;
	uint64_t rax;
} __attribute__((packed));

/* 인터럽트 스택 프레임 */
struct intr_frame {
	/* intr-stubs.S 의 intr_entry 가 푸시한 값들
	   인터럽트가 걸린 작업의 레지스터를 저장해 둔다 */
	struct gp_registers R;
	uint16_t es;
	uint16_t __pad1;
	uint32_t __pad2;
	uint16_t ds;
	uint16_t __pad3;
	uint32_t __pad4;

	/* intr-stubs.S 의 intrNN_stub 이 푸시한 값 */
	uint64_t vec_no; /* 인터럽트 벡터 번호 */

	/* 어떤 경우에는 CPU 가, 어떤 경우에는 intrNN_stub 이 0 을 넣는다.
	   CPU 는 원래 eip 바로 아래에 두지만, 구조체 정렬을 위해 여기로 옮겨 놓았다. */
	uint64_t error_code;

	/* CPU 가 푸시한 값들
	   인터럽트가 걸린 작업의 레지스터를 저장한다 */
	uintptr_t rip;
	uint16_t cs;
	uint16_t __pad5;
	uint32_t __pad6;
	uint64_t eflags;
	uintptr_t rsp;
	uint16_t ss;
	uint16_t __pad7;
	uint32_t __pad8;
} __attribute__((packed));

/* 인터럽트 핸들러 함수 포인터 타입 */
typedef void intr_handler_func (struct intr_frame *);

/* 인터럽트 서브시스템 초기화 */
void intr_init (void);

/* 외부(하드웨어) 인터럽트 등록 */
void intr_register_ext (uint8_t vec, intr_handler_func *, const char *name);

/* 내부(소프트웨어) 인터럽트 등록
   vec  : 벡터 번호
   dpl  : Descriptor Privilege Level
   level: 인터럽트 활성화 여부(INTR_OFF/INTR_ON) */
void intr_register_int (uint8_t vec, int dpl, enum intr_level,
                        intr_handler_func *, const char *name);

/* 현재 코드가 인터럽트 컨텍스트인지 확인 */
bool intr_context (void);

/* 인터럽트 핸들러가 끝난 뒤 자동으로 스레드 양보하도록 표시 */
void intr_yield_on_return (void);

/* intr_frame을 보기 좋게 출력 */
void intr_dump_frame (const struct intr_frame *);

/* 벡터 번호에 해당하는 인터럽트 이름 반환 */
const char *intr_name (uint8_t vec);

#endif /* threads/interrupt.h */