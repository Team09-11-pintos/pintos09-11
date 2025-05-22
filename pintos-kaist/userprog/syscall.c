#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "../include/threads/vaddr.h"
#include "../include/lib/kernel/console.h"
#include "../include/filesys/file.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "lib/kernel/console.h"
#include "lib/user/syscall.h"
#include "filesys/filesys.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void exit (int status);
int wait(pid_t pid);
void check_address(void *addr);
struct lock *file_lock;

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
	lock_init(&file_lock);
}

/* 핵심 시스템 콜 인터페이스 */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: 여기에 구현을 작성하십시오.
	int syscall_num = f->R.rax;
	switch(syscall_num){
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit(f->R.rdi);
			break;
		// case SYS_FORK:
		// 	f->R.rax=fork(f->R.rdi);
		// 	break;
		// case SYS_EXEC:
		// 	f->R.rax = exec(f->R.rdi);
		// case SYS_WAIT:
		//  	f->R.rax = wait(f->R.rdi);
		// 	break;
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi,f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax=open(f->R.rdi);
			break;
		// case SYS_FILESIZE:
		// 	f->R.rax = filesize(f->R.rdi);
		// 	break;
		// case SYS_READ:
		// 	f->R.rax = read(f->R.rdi,f->R.rsi,f->R.rdx);
		// 	break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi,f->R.rsi,f->R.rdx);//rax는 syscall 진입직후(유저가 호출한 시스템 콜 번호)와 처리후(시스템 콜의 리턴값)가 다르다. 유저가 요청한 기능번호에요! -> 그 기능의 결과에요!
			break;
		// case SYS_SEEK:
		// 	seek(f->R.rdi,f->R.rsi);
		// 	break;
		// case SYS_TELL:
		// 	f->R.rax = tell(f->R.rdi);
		// 	break;
		// case SYS_CLOSE:
		// 	close(f->R.rdi);
		// 	break;
		default:
			exit(-1);
	
	}
	//thread_exit ();
}
void halt(void){
	power_off();
}


// int write (int fd, const void *buffer, unsigned size){
// 	// fd가 가르키는곳 , buffer에 들어있는 데이터 , 사이즈  --> 기록(write)
// 	struct thread *cur = thread_current();
// 	if(!is_user_vaddr(buffer)||pm14_get_page(cur->pml4,buffer)==NULL){
// 		exit(-1);
// 	}

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
int open (const char *file){
	struct thread *cur = thread_current();
	check_address(file);//주소 검증
	lock_acquire(&file_lock);//파일 시스템에 접근하기 때문에 락 걸기
	struct file *f = filesys_open(file);// 파일 열기
	lock_release(&file_lock);// 파일 얼었으니 락 해제

	if(f==NULL)//파일이 없으면 -1을 반환
		return -1;
	
	if(strcmp(file,cur->name)==0)//strcmp()는 두 문자열이 같은지 비교하는 함수
									//strcmp(a, b)는 a와 b가 같으면 0을 반환
	//→ "지금 열려고 하는 파일이 현재 실행 중인 프로그램이랑 같은 파일인지 확인"
		file_deny_write(f);

	int fd = process_add_file(f);//파일이 있다면 find_descriptor를 통해 fd를 얻음
	if(fd==-1)//fd는 정상적으론 0~3이상의 숫자가 배정됨. -1이면 파일디스크립터를 배정받을 수 있는 자리가 없다는 뜻. fdt가 꽉 찼거나 오류 발생.
		file_close(f);
	return fd;    
}

int write (int fd, const void *buffer, unsigned length){
	struct thread* cur = thread_current();
	check_address(buffer);
	if((fd<=0) || (fd>=64)){
		return -1;
	}

	if(fd == 1){
		putbuf((char *)buffer, length);
		return length;
	}else if(fd >= 2){
		// file descriptor 
		struct file* file_addr = is_open_file(cur, fd);

		if(file_addr == NULL){
			printf("File is not open!! Please file open first\n");
			return -1;
		}
		lock_acquire(&file_lock);
		int32_t written = file_write(file_addr, buffer, length);
		lock_release(&file_lock);
		
		if(written < 0) return -1;

		return written;
	}
	return -1;
}
void exit (int status){//이건 값만 전달 0,-1,등 은 fork 부모가 받아서 씀
	struct thread *cur = thread_current();
	cur->exit_status = status;//구조체 추가후 종료된 시점의 status 저장
	printf("%s: exit(%d)\n",cur->name,status);//로그
	thread_exit();
}
// int wait(pid_t pid){
// 	return process_wait(pid);
// }

void check_address(void *addr){
	if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(thread_current()->pml4,addr)==NULL)
		exit(-1);
}
bool create(const char *file,unsigned initial_size){
	check_address(file);
	return filesys_create(file, initial_size);
	/*
	- 파일을 생성하는 시스템 콜
	- 성공 일 경우 true, 실패 일 경우 false 리턴
	- file : 생성할 파일의 이름 및 경로 정보
	- initial_size : 생성할 파일의 크기
	*/
}
bool remove(const char *file){
	check_address(file);
	return filesys_remove(file);
	/*
	- 파일을 삭제하는 시스템 콜
	- file : 제거할 파일의 이름 및 경로 정보
	- 성공 일 경우 ture, 실패 일 경우 false 리턴
	*/
}
int filesize(int fd){
	struct file *f = process_get_file(fd);
	if(f==NULL)
		return -1;
	lock_acquire(&file_lock);
	int size = file_length(f);
	lock_release(&file_lock);
	
	return size;
}