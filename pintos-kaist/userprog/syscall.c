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
#include "lib/user/syscall.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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
	printf ("system call!\n");

	uint64_t syscall_type = f->R.rax;

	switch(syscall_type){
		case SYS_OPEN:{
			char * filename = (char*)f->R.rdi;
			f->R.rax = sys_open(filename);
			break;
		}
		case SYS_WRITE:{
			int fd = (int)f->R.rdi;
			void *buf = (void*)f->R.rsi;
			size_t size = (size_t)f->R.rdx;

			f->R.rax = sys_write(fd, buf, size);
			break;
		}
	}

	thread_exit ();
}

bool
sys_create(char* filename, unsigned size){

}

int
sys_open(char* filename){
	if(!is_user_vaddr(filename)){
		return -1;
	}

	int fd = find_descriptor(thread_current());
    
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
	}else if(fd >= 3){
		// file descriptor 
		struct thread* curr = thread_current();
		struct file* file_addr = is_open_file(curr, fd);

		if(file_addr == NULL){
			printf("File is not open!! Please file open first\n");
			return -1;
		}

		int32_t written = file_write(file_addr, buf, size);
		
		if(written < 0) return -1;

		return written;
	}
	return -1;
}