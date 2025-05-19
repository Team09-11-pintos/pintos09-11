#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "lib/kernel/console.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void exit (int status);

/* 시스템 콜.
 *
 * 과거에는 시스템 콜 서비스를 인터럽트 핸들러가 처리했습니다
 * (예: 리눅스의 int 0x80). 하지만 x86-64 아키텍처에서는
 * 제조사가 `syscall` 명령어라는 훨씬 효율적인 시스템 콜 경로를
 * 제공합니다.
 *
 * `syscall` 명령어는 MSR(Model-Specific Register)에 저장된 값을
 * 읽어 동작합니다. 자세한 내용은 매뉴얼을 참고하세요. */

#define MSR_STAR          0xc0000081 /* 세그먼트 셀렉터 MSR */
#define MSR_LSTAR         0xc0000082 /* 롱 모드 SYSCALL 타깃 */
#define MSR_SYSCALL_MASK  0xc0000084 /* EFLAGS 마스킹용 값 */

void
syscall_init (void) {
	/* 사용자 모드 → 커널 모드 진입 시 사용할 세그먼트와
	 * 리턴 세그먼트를 MSR에 설정합니다. */
	write_msr (MSR_STAR,
	           ((uint64_t) SEL_UCSEG - 0x10) << 48  |
	           ((uint64_t) SEL_KCSEG) << 32);
	/* SYSCALL 진입 지점 등록 */
	write_msr (MSR_LSTAR, (uint64_t) syscall_entry);

	/* syscall_entry 가 커널 스택으로 스택 전환을 끝내기 전까지는
	 * 인터럽트를 받아서는 안 됩니다. 따라서 FLAG_IF 등을
	 * 마스킹하여 중단시킵니다. */
	write_msr (MSR_SYSCALL_MASK,
	           FLAG_IF | FLAG_TF | FLAG_DF |
	           FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 핵심 시스템 콜 인터페이스 */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: 여기에 구현을 작성하십시오.
	// int syscall_num = f->R.rax;
	// switch(syscall_num){
	// 	case SYS_HALT:
	// 		halt();
	// 		break;
	// 	case SYS_EXIT:
	// 		exit(f->R.rdi);
	// 		break;
	// 	case SYS_EXEC:
	// 		f->R.rax = exec(f->R.rdi);
	// 	case SYS_WRITE:
	// 		f->R.rax = write(f->R.rdi,f->R.rsi,f->R.rdx);//rax는 syscall 진입직후(유저가 호출한 시스템 콜 번호)와 처리후(시스템 콜의 리턴값)가 다르다. 유저가 요청한 기능번호에요! -> 그 기능의 결과에요!
	// 		break;
		
	// }

	printf ("system call!\n");
	thread_exit ();
}
// int write (int fd, const void *buffer, unsigned size){
// 	// fd가 가르키는곳 , buffer에 들어있는 데이터 , 사이즈  --> 기록(write)
// 	struct thread *cur = thread_current();
// 	struct file *f = cur->fd_table[fd];
// 	//fd의 3가지 1:출력 2:오류?
// 	if (!buffer ==NULL){
// 		exit(-1);
// 	}
   
// 	if (fd == 1){
// 		putbuf(buffer,size);
// 		return (int)size;
// 	}
// 	if (fd ==2 || fd<=0 || fd_table[fd]==NULL){
// 		exit(-1);
// 	}
	
	
	
	
// }
// void exit (int status){//이건 값만 전달 0,-1,등 은 fork 부모가 받아서 씀
// 	struct thread *cur = thread_current();
// 	cur->exit_status = status;//구조체 추가후 종료된 시점의 status 저장
// 	printf("%s: exit(%d)\n",cur->name,status);//로그
// 	thread_exit();
// }



