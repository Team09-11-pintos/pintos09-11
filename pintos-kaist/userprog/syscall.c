#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/vaddr.h"
#include "lib/kernel/console.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool sys_create(char*filename, unsigned size);
int sys_open(char *filename);
bool sys_remove(char *filename);
int sys_filesize(int fd);
void sys_exit(int status);
int sys_read(int fd, void *buffer, size_t size);
int sys_write(int fd, void *buffer, size_t size);
void sys_close(int fd);

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

	lock_init(&file_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.

	uint64_t syscall_type = f->R.rax;

	switch(syscall_type){
		case SYS_OPEN:{
			char * filename = (char*)f->R.rdi;
			f->R.rax = sys_open(filename);
			break;
		}
		case SYS_REMOVE:{
			char* filename = (char*)f->R.rdi;
			f->R.rax=sys_remove(filename);
			break;
		}
		case SYS_WRITE:{
			int fd = (int)f->R.rdi;
			void *buf = (void*)f->R.rsi;
			size_t size = (size_t)f->R.rdx;

			f->R.rax = sys_write(fd, buf, size);
			break;
		}
		case SYS_READ:{
			int fd = (int) f->R.rdi;
			void *buf = (void*)f->R.rsi;
			size_t size = (size_t)f->R.rdx;
			f->R.rax= sys_read(fd,buf,size);
			break;
		}
		case SYS_EXIT:{
			sys_exit((int)f->R.rdi);
			break;
		}
		case SYS_FILESIZE:{
			f->R.rax = sys_filesize((int)f->R.rdi);
			break;
		}
		case SYS_CREATE:{
			char *filename = (char *)f->R.rdi;
			unsigned size = (unsigned)f->R.rsi;
			f->R.rax = sys_create(filename, size);
			break;
		}
	}

	// thread_exit ();
}

bool
sys_create(char* filename, unsigned size){
	if(!is_user_vaddr(filename) || !is_user_vaddr(filename+size-1)){
		return -1;
	}

	size_t init_size = (size_t) size;
	lock_acquire(&file_lock);
	bool result = filesys_create(filename, init_size);
	lock_release(&file_lock);
	return result;
}

int
sys_open(char* filename){
	if(!is_user_vaddr(filename)){
		return -1;
	}

	struct thread *cur = thread_current();
	//file descriptor 할당
	int fd = find_descriptor(cur);
	if(fd == -1){
		return -1;
	}
    
	// enum intr_level old = intr_disable();
	lock_acquire(&file_lock);
	struct file* file = filesys_open(filename);
	lock_release(&file_lock);
	// intr_set_level(old);
	if(file == NULL){
		
		printf("%s No file in directory\n", filename);
		return -1;
	}
	
	cur->file_table[fd] = file;
	return fd;
}

int
sys_filesize(int fd){
	struct file *file_addr = is_open_file(thread_current(),fd);
	if(file_addr == NULL)
		return -1;

	lock_acquire(&file_lock);
	off_t size = file_length(file_addr);
	lock_release(&file_lock);
	return size;
}

void
sys_exit(int status){
	struct thread* curr = thread_current();
	curr->exit_status = status;
	printf("%s: exit(%d)\n",curr->name,status);//로그
	thread_exit();
}

int
sys_read(int fd, void *buffer, size_t size){
	if(size == 0){
		return 0;
	}

	if(!is_user_vaddr(buffer)){
		return -1;
	}

	if((fd<0) || (fd>=64)){
		return -1;
	}

	if(fd == 0){
		char *buf = (char *) buffer;
		for(int i=0;i<size;i++){
			buf[i] = input_getc();
		}
		return size;
	}else{
		struct thread* cur = thread_current();
		struct file *file = is_open_file(cur,fd);

		if(file == NULL){
			return -1;
		}

		lock_acquire(&file_lock);
		off_t result = file_read(file, buffer, size);
		lock_release(&file_lock);
		return result;
	}
}

int
sys_write(int fd, void* buf, size_t size){
	if(!is_user_vaddr(buf) || !is_user_vaddr((uint8_t *)buf + size - 1)){
		return -1;
	}
	if((fd<=0) || (fd>=64)){
		return -1;
	}

	if(fd == 1){
		putbuf((char *)buf, size);
		return size;
	}else if(fd >= 2){
		// file descriptor 
		struct thread* curr = thread_current();
		struct file* file_addr = is_open_file(curr, fd);
		
		if(file_addr == NULL){
			printf("Is Here!\n");
			return -1;
		}

		lock_acquire(&file_lock);
		printf("okokokok\n");
		int32_t written = file_write(file_addr, buf, size);
		lock_release(&file_lock);
		if(written < 0) return -1;

		return written;
	}
	return -1;
}

void
sys_close(int fd){
	struct thread *cur = thread_current();
	struct file *file = is_open_file(cur, fd);

	if(file == NULL)
		return -1;

	lock_acquire(&file_lock);
	file_close(file);	
	lock_release(&file_lock);
}

bool
sys_remove(char* filename){
	if(!is_user_vaddr(filename)){
		return -1;
	}

	lock_acquire(&file_lock);
	int result = filesys_remove(filename);
	lock_release(&file_lock);

	return result;
}