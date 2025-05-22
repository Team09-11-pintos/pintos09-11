#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "lib/string.h"
#include "threads/palloc.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void exit (int status);
void check_addr(const char *addr);
int exec (const char *file);
int sys_write(int fd, void* buf, size_t size);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
void seek(int fd, unsigned position);
unsigned tell(int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	switch(f->R.rax){
		case SYS_EXIT:
			exit(f->R.rdi);
			break;

		case SYS_EXEC:
			if(exec(f->R.rdi) == -1)
				exit(-1);
			break;
		
		case SYS_WAIT:
			wait(f->R.rdi);
			break;

		case SYS_WRITE:
			{int fd = (int)f->R.rdi;
			void *buf = (void*)f->R.rsi;
			size_t size = (size_t)f->R.rdx;

			f->R.rax = sys_write(fd, buf, size);
			break;}

		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;

		case SYS_SEEK:
			seek((int)f->R.rdi, (unsigned)f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
	}

}

void exit (int status){//이건 값만 전달 0,-1,등 은 fork 부모가 받아서 씀
	struct thread *cur = thread_current();
	struct child *c;
	c = cur->my_self;

	if (status != -1 && c != NULL){ //exit status가 -1이 아니고 child가 존재할 때 
		c->is_exit = true;			//child 구조체 안에 값들 수정
		c->exit_status = status;
		sema_up(&c->sema);
	}
	printf("%s: exit(%d)\n",cur->name,status);//로그
	thread_exit();
}

void
check_addr(const char *addr){
	if (!is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL)
		exit(-1);
}
int
wait(tid_t pid){
	return process_wait();
}
int
exec (const char *file){
	if (file == NULL)
		exit(-1);
	
	check_addr(file);
	
	char *file_name = palloc_get_page(0);
	if (file_name == NULL)
		palloc_free_page(file_name);
		exit(-1);
	strlcpy(file_name, file, PGSIZE); //copy file, user->kernal

	if (process_exec(file_name) == -1){
		palloc_free_page(file_name);
		exit(-1);
	}
	NOT_REACHED();
	return -1;
}

int
sys_write(int fd, void* buf, size_t size){
	if(!is_user_vaddr(buf)){
		printf("Invaild buf address");
		return -1;
	}
	if((fd<=0) || (fd>=128)){
		printf("Invaild file descriptor value");
		return -1;
	}

	if(fd == 1 || fd == 2){
		putbuf((char *)buf, size);
		return size;
	}
	// else if(fd >= 3){
	// 	// file descriptor 
	// 	struct thread* curr = thread_current();
	// 	struct file* file_addr = is_open_file(curr, fd);

	// 	if(file_addr == NULL){
	// 		printf("File is not open!! Please file open first\n");
	// 		return -1;
	// 	}

	// 	int32_t written = file_write(file_addr, buf, size);
		
	// 	if(written < 0) return -1;

	// 	return written;
	// }
	return -1;
}

bool
create(const char *file, unsigned initial_size){
	filesys_create(file, initial_size);
}

bool
remove(const char *file){
	filesys_remove(file);
}

void
seek(int fd, unsigned position){
	struct file *f = thread_current()->file_table[fd];
	if (f == NULL)
		exit(-1);
	file_seek(f, position);
}

unsigned
tell(int fd){
	struct file *f = thread_current()->file_table[fd];
	if (f == NULL)
		exit(-1);
	file_tell(f);
}