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
#include "userprog/process.h"
#include "devices/input.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void exit (int status);
int wait(pid_t pid);
void check_address(const void *addr);
struct lock file_lock;

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
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = read(f->R.rdi,f->R.rsi,f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi,f->R.rsi,f->R.rdx);//rax는 syscall 진입직후(유저가 호출한 시스템 콜 번호)와 처리후(시스템 콜의 리턴값)가 다르다. 유저가 요청한 기능번호에요! -> 그 기능의 결과에요!
			break;
		case SYS_SEEK:
			seek(f->R.rdi,f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
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
	if(fd==-1)//fd는 정상적으론 0~3이상의 숫자가 배정됨. -1이면 파일디스크립터를 배정받을 수 있는 자리가 없다는 뜻. fdt가 꽉 찼거나 오류 발생. -> 이건 위에 process_add_file에서 if (cur->fd_idx >= FDCOUNT_LIMIT)  이 구문을 실행함
		file_close(f);
	return fd;    
}

int write (int fd, const void *buffer, unsigned length){//fd에서 length만큼의 길이를 읽어서 buffer에 저장한다 || 유저가 넘긴 buffer에 있는 데이터를 fd로 연결된 대상에 length만큼 출력한다.
	check_address(buffer);//버퍼가 유효하고 매핑된 메모리인지 확인
	off_t bytes = -1; // offset을 표현하는 정수형 타입, -1로 초기화하는 이유는 실패 대기.
	if (fd<=0) // 파일디스크립터가 0이나 음수일때 -1을 리턴한다
		return -1;//stdin(0)이나 음수 fd는 쓰기 불가하므로 -1 반환
	if (fd<3){//1,2일때 파일을 읽기 때문에 putbuf : 콘솔에 buffer 내용을 출력하는 함수(stdout 처럼 동작)
	//1:stdout 2:stderr 경우 콘솔 출력
		putbuf(buffer,length);
		return length;//콘솔 출력은 실패 여부를 따지지 않으므로 length리턴
	}
	struct file *f = process_get_file(fd); //유저의 fdt에서 fd에 해당하는 파일포인터를 가져온다 f에
	if (f ==NULL)//파일이 없다면 -1 리턴
		return -1;
	lock_acquire(&file_lock);//파일이 있다면 쓰기위해 파일락을 건다 || 파일 시스템은 공유자원이라 락으로 보호
	bytes = file_write(f,buffer,length);//파일에 데이터를 length만큼 쓴다 그리고 실제 쓴 바이트 수를 반환
	lock_release(&file_lock);//파일잠금을 해제한다
	return bytes;//실제로 쓴 바이트 수 리턴
	//struct thread* cur = thread_current();
	// if((fd<=0) || (fd>=64)){
	// 	return -1;
	// }

	// if(fd == 1){
	// 	putbuf((char *)buffer, length);
	// 	return length;
	// }else if(fd >= 2){
	// 	// file descriptor 
	// 	struct file* file_addr = is_open_file(cur, fd);

	// 	if(file_addr == NULL){
	// 		printf("File is not open!! Please file open first\n");
	// 		return -1;
	// 	}
	// 	lock_acquire(&file_lock);
	// 	int32_t written = file_write(file_addr, buffer, length);
	// 	lock_release(&file_lock);
		
	// 	if(written < 0) return -1;

	// 	return written;
	// }
	// return -1;
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

void check_address(const void *addr){
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
	struct file *f = process_get_file(fd);//파일디스크립터를 받아서 파일을 가져오고 그 포인터 주소를 f에다 담는다  ||  fd로 열린 파일의 포인터를 가져온다
	if(f==NULL)//f가 널이라면 리턴 -1을 한다 
		return -1;
	lock_acquire(&file_lock);//널이 아니라면 파일을 읽어서 사이즈를 가져와야해서 다른 프로세스가 진입하지 못하게 락을 건다 || 파일 접근 보호용 락
	int size = file_length(f);// inode가 뭘까 암튼 f파일의 사이즈를 int size에 저장한다  || 파일의 실제 길이(바이트 단위)를 가져오는 함수.(inode 기반) || inode는 디스크 상에 저장된 파일의 메타데이터(크기, 타입, 위치 등)를 담고 있는 구조체. -> 파일의 바이트 단위 크기를 읽어온다.
	lock_release(&file_lock);// 저장하고 락을 푼다 해제한다
	return size;//저장한 사이즈값 리턴
}
int read(int fd, void *buffer, unsigned length){//buffer -> 데이터를 임시로 저장해두는 유저 공간의 메모리 주소(배열,포인터) | fd에서 length 바이트만큼 데이터를 읽어서 buffer에 저장
	check_address(buffer);//buffer가 유효한 주소(유저영역에 있고,페이지 테이블에 매핑된 공간)인지 확인 || 유효하지 않으면 프로세스 종료(exit(-1))
	if (fd == 0){ // fd == 0 -> 표준입력 (키보드 입력 받기) 
		int i =0; //새로 입력받은 값의 번호라 0으로 초기화
		char c;// 
		unsigned char *buf = buffer;//왜 void가 아닌 char를 쓴거지? -> void는 자료형이 명시되지 않아 직접 연산 불가 그래서 포인터 타입 캐스팅 사용
		for(;i<length;i++){//for문 처음이 왜 없지? 위에서 0으로 초기화해서? -> 내 생각이 맞음
			//c=input_getc();//이게 c에다 키보드입력을 넣은거지? -> 키보드에서 1글자 입력받음
			//*buf++=c; // 후위연산. buf에다가 입력받은 값을 넣는다 근데 그럼 *buf++ = input_getc(); 이렇게 하면 안되나?
			if ((*buf++ = input_getc())=='\0')//입력값이 없으면 (개행문자뿐이면) break;
				break;
		}
		return i;//i는 fd인걸까?X || i == 입력된 문자 수 (바이트 수) 를 의미한다
	}
	if(fd<3) // 0이왜의 1은 읽기 2는 오류 그리고 음수들도 오류. 1은 왜 -1리턴이지? || 0은 stdin(읽기) 1,2 는 stdout,stderr (쓰기) 1빼곤 쓰기라 읽기 불가.
		return -1;
	struct file *f = process_get_file(fd);//해당 파일에 접근해서 *f에 포인터 전달 || 유저가 가진 fdt에서 해당 fd에 해당하는 file* 포인터를 꺼냄
	//process_get_file 이게 유저가 가진 FDT라는것.
	off_t bytes = -1; // off_t 는 파일의 길이, offset을 표현하는 타입(정수) || 실패 대비로 -1로 초기화
	if (f == NULL)//파일이 null이면 -1 리턴 || 유효하지 않은 파일
		return -1;
	lock_acquire(&file_lock);//파일을 읽어서 byte 즉 길이를 읽기 위해 락을 건다
	bytes = file_read(f,buffer,length);//버퍼에 길이를 저장하고 그걸 byte에 저장 || 파일에서 읽어 buffer에 저장
	lock_release(&file_lock);//락을 해제
	return bytes;//바이트를 리턴
}
void seek(int fd,unsigned position){
	/*
	- 열린 파일의 위치(offset)를 이동하는 시스템 콜
	- position : 현재 위치(offset)를 기준으로 이동할 거리
	*/
	struct file *f = process_get_file(fd); //fdt에서 fd의 번호를 가진 파일포인터를 가져온다 f에
	if(fd<3 || f==NULL)//파일번호는 3부터이기 때문에 (1쓰기,2,0=읽기 , 음수) 나머지는 그냥 리턴
		return;
	file_seek(f,position);//f가 가리키는 열린 파일의 현재 위치를 position으로 이동.
}
unsigned tell(int fd){
	struct file *f = process_get_file(fd);//현재 스레드의 fdt에서 fd에 해당하는 파일 포인터 가져옴
	if(fd<3 || f==NULL)//파일번호는 3부터이기 때문에 (1쓰기,2,0=읽기 , 음수) 나머지는 그냥 리턴
		return -1;
	return file_tell(f); //파일의 현재 오프셋 위치 반환
}
void close(int fd){
	struct file *f = process_get_file(fd);
	if (fd<3||f==NULL)
		return;
	process_close_file(f);
	file_close(f);
}