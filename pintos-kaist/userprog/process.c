#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* 프로세스 정리 함수 프로토타입 */
static void process_cleanup (void);
/* ELF 파일을 메모리에 적재 */
static bool load (const char *file_name, struct intr_frame *if_);
/* 첫 번째 사용자 프로세스(initd) 스레드 본체 */
static void initd (void *f_name);
/* fork 내부 동작을 수행하는 실제 스레드 함수 */
static void __do_fork (void *);

struct thread *get_child_process(int pid);

/* initd 및 기타 프로세스를 위한 공통 프로세스 초기화 함수 */
static void
process_init (void) {
    struct thread *current = thread_current ();
}

/* FILE_NAME에서 "initd"라는 첫 사용자 프로그램을 시작합니다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄될 수도 있고
 * 심지어 종료될 수도 있습니다.
 * 성공 시 initd의 스레드 ID를, 실패 시 TID_ERROR를 반환합니다.
 * 반드시 한 번만 호출되어야 함에 유의하세요. */
tid_t
process_create_initd (const char *file_name) {
    char *fn_copy,*name;
    tid_t tid;

    /* FILE_NAME의 복사본을 만듭니다.
     * 그렇지 않으면 호출자와 load() 사이에 경쟁 상태가 발생할 수 있습니다. */
    fn_copy = palloc_get_page (0);
    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy (fn_copy, file_name, PGSIZE);

    char* save_ptr;
    strtok_r(file_name," ",&save_ptr); // 이걸로 파일전체이름이 아니라 파일이름만 파싱함

    /* FILE_NAME을 실행할 새 스레드를 생성합니다. */
    tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
    if (tid == TID_ERROR){
        palloc_free_page (fn_copy);
        return TID_ERROR;
    }
#ifdef USERPROG
    // struct thread *parent = thread_current();
    // struct child *c = malloc(sizeof(child));
    // c->child_tid = tid;
    // c->is_waited = false;
    // c->is_exit = false;
    // sema_init(&c->sema,0);
    // list_push_back(&parent->child_list,&c->elem);
    // struct thread *child_thread=get_child_thread_tid(tid);
    // child_thread->my_child=c;

#endif
return tid;
}

/* 첫 번째 사용자 프로세스를 시작하는 스레드 함수 */
static void
initd (void *f_name) {
#ifdef VM
    supplemental_page_table_init (&thread_current ()->spt);
#endif

    process_init ();

    if (process_exec (f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED ();
}

/* 현재 프로세스를 `name`으로 복제(fork)합니다.
 * 새 프로세스의 스레드 ID를 반환하며, 생성 실패 시 TID_ERROR를 반환합니다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
    /* 현재 스레드를 새 스레드로 복제합니다. */
    return thread_create (name,
            PRI_DEFAULT, __do_fork, thread_current ());
}

#ifndef VM
/* 부모의 주소 공간을 복제하기 위해 pml4_for_each에 전달되는 함수입니다.
 * 프로젝트 2에서만 사용됩니다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
    struct thread *current = thread_current ();
    struct thread *parent = (struct thread *) aux;
    void *parent_page;
    void *newpage;
    bool writable;

    /* 1. TODO: parent_page가 커널 페이지라면 즉시 반환합니다. */

    /* 2. 부모의 pml4에서 VA를 해석합니다. */
    parent_page = pml4_get_page (parent->pml4, va);

    /* 3. TODO: 자식용으로 PAL_USER 페이지를 새로 할당하고 NEWPAGE에 설정합니다. */

    /* 4. TODO: 부모의 페이지를 새 페이지로 복사하고
     *    TODO: 부모 페이지가 쓰기 가능 여부를 확인하여 WRITABLE에 설정합니다. */

    /* 5. WRITABLE 권한으로 자식의 페이지 테이블에 VA 주소로 새 페이지를 추가합니다. */
    if (!pml4_set_page (current->pml4, va, newpage, writable)) {
        /* 6. TODO: 페이지 삽입 실패 시 오류 처리 */
    }
    return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수
 * 힌트) parent->tf는 사용자 영역 컨텍스트를 담고 있지 않으므로
 *        process_fork()의 두 번째 인자를 이 함수로 전달해야 합니다. */
static void
__do_fork (void *aux) {
    struct intr_frame if_;
    struct thread *parent = (struct thread *) aux;
    struct thread *current = thread_current ();
    /* TODO: parent_if를 전달하세요 (즉, process_fork()의 if_) */
    struct intr_frame *parent_if;
    bool succ = true;

    /* 1. CPU 컨텍스트를 로컬 스택에 복사 */
    memcpy (&if_, parent_if, sizeof (struct intr_frame));

    /* 2. 페이지 테이블 복제 */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL)
        goto error;

    process_activate (current);
#ifdef VM
    supplemental_page_table_init (&current->spt);
    if (!supplemental_page_table_copy (&current->spt, &parent->spt))
        goto error;
#else
    if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
        goto error;
#endif

    /* TODO: 여기에 필요한 추가 작업을 구현하세요.
     * TODO: 파일 객체를 복제하려면 include/filesys/file.h의 `file_duplicate`를 사용하세요.
     * TODO: 부모 프로세스는 이 함수가 부모의 자원을 성공적으로 복제할 때까지
     * TODO: fork()로부터 반환되면 안 됩니다.*/

    process_init ();

    /* 마지막으로, 새로 생성된 프로세스로 전환합니다. */
    if (succ)
        do_iret (&if_);
error:
    thread_exit ();
}

/* 현재 실행 컨텍스트를 f_name으로 전환합니다.
 * 실패 시 -1을 반환합니다. */
int
process_exec (void *f_name) {
    char *file_name = f_name;
    bool success;

    /* thread 구조체 내 intr_frame은 사용할 수 없습니다.
     * 현재 스레드가 스케줄링되면 실행 정보를 그 멤버에 저장하기 때문입니다. */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* 현재 컨텍스트를 제거합니다. */
    process_cleanup ();

    /*parsing*/
    char * save_ptr;//strtok_r에 쓸 포인터변수.
    char* L_token[64];//단어별로 저장할 배열
    int argc = 0;
    char *token = strtok_r(file_name, " ", &save_ptr);
    while(token != NULL && argc < 64){
        L_token[argc++] = token;//단어 저장
        token = strtok_r(NULL, " ", &save_ptr);
    }

    //thread_set_name(L_token[0]);
    /* 바이너리를 로드합니다. */
    success = load (L_token[0], &_if);

    /*passing*/
    char* argv[argc];
    for (int i = argc - 1; i >= 0; i--) {
        _if.rsp -= strlen(L_token[i])+1;//null 포함
        //이게 잘못된듯 문자열주소가 아니라 첫 문자만 저장 _if.rsp = *L_token[i];
        memcpy(_if.rsp, L_token[i],strlen(L_token[i])+1);
        argv[i]=_if.rsp;
    }
    _if.rsp -= _if.rsp %8; // 패딩
    //패싱
    _if.rsp -= sizeof(char *);
    memset(_if.rsp,0,sizeof(char *));

    // addr_list를 역순으로 push → 이게 argv[i]
    for (int i = argc - 1; i >= 0; i--) {
        _if.rsp -= sizeof(char *);
        memcpy(_if.rsp, &argv[i], sizeof(char *));//+1?
    }

    _if.R.rdi = argc;
    _if.R.rsi = _if.rsp;

    _if.rsp -= sizeof(char *);
    memset(_if.rsp,0,sizeof(char *));

    

    /* 로드 실패 시 종료합니다. */
    palloc_free_page (file_name);
    if (!success)
        return -1;
    
    /* 프로세스를 시작합니다. */
    do_iret (&_if);
    NOT_REACHED ();//do_iret이 리턴값을 제대로 못해 커널패드가 일어난것
}


/* 스레드 TID가 종료될 때까지 대기하고 그 종료 코드를 반환합니다.
 * 커널에 의해 종료된 경우(예: 예외로 kill)에는 -1을 반환합니다.
 * TID가 유효하지 않거나, 호출 프로세스의 자식이 아니거나,
 * 이미 process_wait()가 호출된 TID라면 즉시 -1을 반환합니다.
 *
 * 이 함수는 문제 2-2에서 구현됩니다. 현재는 아무것도 하지 않습니다. */
int
process_wait (tid_t child_tid) {
    /* XXX: 힌트) pintos가 process_wait(initd)에서 종료되므로
     * XXX:       구현 전에는 여기에 무한 루프를 두기를 권장합니다. */
    for (int i = 0; i < 300000000; i++){
    }
    // struct thread *cur = thread_current();//현재 실행중인 스레드의 포인터를 가져온다
    // struct list_elem *e;//list_elem을 참조하기 위해 e선언 -> 자식리스트를 순회하기 위한 리스트 요소 포인터
    // for(e=list_begin(&cur->child_list);e!=list_end(&cur->child_list);e=list_next(e)){//child_list에 현재 child_tid가 있는지 확인 -> child_list에서 자식 스레드를 찾기 위한 반복문
    //     struct child *c=list_entry(e,struct child,elem);//e로 참조해 child 정보를 불러온다 -> 리스트요소 e를 child 구조체로 변환하여 정보에 접근
    //     if(c->child_tid==child_tid){//해당 tid를 가진 자식이라면? -> 해당 child_tid를 가진 자식 프로세스를 찾은 경우
    //         if(c->is_waited)//이미 대기중인 자식이라면
    //             return -1;//바로 리턴-1
    //         c->is_waited = true;//대기중이 아니라면 대기시키기 위해 true로 변경
    //         if(!c->is_exit){//만약 종료되기까지 시간이 남았다면
    //             sema_down(&c->sema);//부모스레드를 블럭처리 (wait시킴)
    //         }   
    //         return c->exit_status;//자식 스레드의 종료 상태를 반환
    //     }
    // }
    // struct thread *child = get_child_process(child_tid);
    // if (child == NULL)
    //     return -1;
    // sema_down()
    // return exit_status;
    return -1;
}
    



/* 프로세스를 종료합니다. thread_exit()에서 호출됩니다. */
void
process_exit (void) {
    struct thread *cur = thread_current ();
    /* TODO: 여기에 코드 작성
     * TODO: 프로세스 종료 메시지 구현 (project2/process_termination.html 참고)
     * TODO: 프로세스 자원 정리를 이곳에 구현하는 것을 권장합니다. */
    struct thread *parent = cur->parent;
    if(parent!=NULL){
        struct list *clist = &parent->child_list;
        struct list_elem *e;
        for(e=list_begin(clist);e!=list_end(clist);e=list_next(e)){
            struct child *c = list_entry(e,struct child,elem);
            if(c->child_tid==cur->tid){//자신에 해당하는 child 구조체를 찾은 경우
                c->exit_status= cur->exit_status;
                c->is_exit=true;
                sema_up(&c->sema);//부모가 wait이면 깨운다
                break;
            }
        }
    }
    process_cleanup ();
}

/* 현재 프로세스의 자원을 해제합니다. */
static void
process_cleanup (void) {
    struct thread *curr = thread_current ();

#ifdef VM
    supplemental_page_table_kill (&curr->spt);
#endif

    uint64_t *pml4;
    /* 현재 프로세스의 페이지 디렉터리를 파괴하고
     * 커널 전용 페이지 디렉터리로 되돌립니다. */
    pml4 = curr->pml4;
    if (pml4 != NULL) {
        /* 순서가 매우 중요합니다.
         * timer 인터럽트가 프로세스 페이지 디렉터리로 전환되지 않도록
         * cur->pagedir를 NULL로 설정한 후 페이지 디렉터리를 전환해야 합니다.
         * 프로세스의 페이지 디렉터리를 파괴하기 전에 커널 전용 페이지 디렉터리를
         * 활성화해야, 우리가 활성화한 디렉터리가 해제되는 일을 방지할 수 있습니다. */
        curr->pml4 = NULL;
        pml4_activate (NULL);
        pml4_destroy (pml4);
    }
}

/* 다음 스레드에서 사용자 코드를 실행하기 위해 CPU를 준비합니다.
 * 이 함수는 컨텍스트 스위치마다 호출됩니다. */
void
process_activate (struct thread *next) {
    /* 스레드의 페이지 테이블을 활성화 */
    pml4_activate (next->pml4);

    /* 인터럽트 처리에 사용할 커널 스택을 설정 */
    tss_update (next);
}

/* 우리는 ELF 실행 파일을 로드합니다. 다음 정의는 ELF 명세에서 거의 그대로 가져왔습니다. */

/* ELF 타입. [ELF1] 1-2 참조 */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시 */
#define PT_LOAD    1            /* 로드 가능한 세그먼트 */
#define PT_DYNAMIC 2            /* 동적 링킹 정보 */
#define PT_INTERP  3            /* 동적 로더 이름 */
#define PT_NOTE    4            /* 보조 정보 */
#define PT_SHLIB   5            /* 예약됨 */
#define PT_PHDR    6            /* 프로그램 헤더 테이블 */
#define PT_STACK   0x6474e551   /* 스택 세그먼트 */

#define PF_X 1          /* 실행 가능 */
#define PF_W 2          /* 쓰기 가능 */
#define PF_R 4          /* 읽기 가능 */

/* 실행 파일 헤더. [ELF1] 1-4 ~ 1-8 참조
 * ELF 바이너리의 가장 앞부분에 위치합니다. */
struct ELF64_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
        uint32_t read_bytes, uint32_t zero_bytes,
        bool writable);

/* ELF 실행 파일을 현재 스레드에 로드합니다.
 * 엔트리 포인터를 *RIP에, 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공 시 true, 실패 시 false를 반환합니다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
    struct thread *t = thread_current ();
    struct ELF ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* 페이지 디렉터리를 할당하고 활성화 */
    t->pml4 = pml4_create ();
    if (t->pml4 == NULL)
        goto done;
    process_activate (thread_current ());

    /* 실행 파일 열기 */
    file = filesys_open (file_name);
    if (file == NULL) {
        printf ("load: %s: open failed\n", file_name);
        goto done;
    }

    /* 실행 파일 헤더 읽기 및 검증 */
    if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
            || memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
            || ehdr.e_type != 2
            || ehdr.e_machine != 0x3E // amd64
            || ehdr.e_version != 1
            || ehdr.e_phentsize != sizeof (struct Phdr)
            || ehdr.e_phnum > 1024) {
        printf ("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* 프로그램 헤더 읽기 */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length (file))
            goto done;
        file_seek (file, file_ofs);

        if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
            case PT_NULL:
            case PT_NOTE:
            case PT_PHDR:
            case PT_STACK:
            default:
                /* 이 세그먼트는 무시 */
                break;
            case PT_DYNAMIC:
            case PT_INTERP:
            case PT_SHLIB:
                goto done;
            case PT_LOAD:
                if (validate_segment (&phdr, file)) {
                    bool writable = (phdr.p_flags & PF_W) != 0;
                    uint64_t file_page = phdr.p_offset & ~PGMASK;
                    uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                    uint64_t page_offset = phdr.p_vaddr & PGMASK;
                    uint32_t read_bytes, zero_bytes;
                    if (phdr.p_filesz > 0) {
                        /* 일반 세그먼트: 디스크에서 일부 읽고 나머지는 0으로 */
                        read_bytes = page_offset + phdr.p_filesz;
                        zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                    } else {
                        /* 전부 0으로 채워진 세그먼트 */
                        read_bytes = 0;
                        zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                    }
                    if (!load_segment (file, file_page, (void *) mem_page,
                                read_bytes, zero_bytes, writable))
                        goto done;
                }
                else
                    goto done;
                break;
        }
    }

    /* 스택 설정 */
    if (!setup_stack (if_))
        goto done;

    /* 시작 주소 */
    if_->rip = ehdr.e_entry;

    /* TODO: 여기에 인자 전달 구현 (project2/argument_passing.html 참조) */
	

    success = true;

done:
    /* 성공 여부와 관계없이 여기로 옵니다. */
    file_close (file);
    return success;
}


/* PHDR이 FILE에서 유효하고 로드 가능한 세그먼트를 설명하는지 검사 */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
    /* p_offset과 p_vaddr는 같은 페이지 오프셋을 가져야 합니다. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset은 FILE 내를 가리켜야 합니다. */
    if (phdr->p_offset > (uint64_t) file_length (file))
        return false;

    /* p_memsz는 p_filesz보다 크거나 같아야 합니다. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* 세그먼트는 비어 있으면 안 됩니다. */
    if (phdr->p_memsz == 0)
        return false;

    /* 가상 메모리 영역은 사용자 주소 공간 범위 내에서 시작하고 끝나야 합니다. */
    if (!is_user_vaddr ((void *) phdr->p_vaddr))
        return false;
    if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* 영역이 커널 주소 공간을 넘어 "랩어라운드"해서는 안 됩니다. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* 페이지 0 매핑 금지.
       NULL 포인터를 시스템 콜에 전달할 때 커널 패닉을 방지하기 위함. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* 통과 */
    return true;
}

#ifndef VM
/* 이 블록의 코드는 프로젝트 2에서만 사용됩니다. */

/* load() 보조 함수 */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 OFS에서 시작하는 세그먼트를 UPAGE에 로드합니다.
 * 총 READ_BYTES + ZERO_BYTES만큼의 가상 메모리를 초기화합니다:
 *
 * - UPAGE에서 READ_BYTES 바이트를 FILE에서 읽어옵니다.
 * - UPAGE + READ_BYTES에서 ZERO_BYTES 바이트를 0으로 채웁니다.
 *
 * WRITABLE이 true면 사용자 프로세스가 페이지를 수정할 수 있어야 하며,
 * false면 읽기 전용이어야 합니다.
 *
 * 성공 시 true, 메모리 할당 또는 디스크 읽기 오류 시 false를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
        uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
    ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT (pg_ofs (upage) == 0);
    ASSERT (ofs % PGSIZE == 0);

    file_seek (file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
        /* 이 페이지를 어떻게 채울지 계산합니다.
         * PAGE_READ_BYTES만큼 파일에서 읽고
         * 나머지 PAGE_ZERO_BYTES를 0으로 채웁니다. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* 메모리 페이지 할당 */
        uint8_t *kpage = palloc_get_page (PAL_USER);
        if (kpage == NULL)
            return false;

        /* 페이지 로드 */
        if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
            palloc_free_page (kpage);
            return false;
        }
        memset (kpage + page_read_bytes, 0, page_zero_bytes);

        /* 프로세스 주소 공간에 페이지 추가 */
        if (!install_page (upage, kpage, writable)) {
            printf("fail\n");
            palloc_free_page (kpage);
            return false;
        }

        /* 다음 페이지로 이동 */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* USER_STACK 위치에 0으로 초기화된 최소 스택을 생성 */
static bool
setup_stack (struct intr_frame *if_) {
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page (PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page (kpage);
    }
    return success;
}

/* 사용자 가상 주소 UPAGE를 커널 주소 KPAGE에 매핑합니다.
 * WRITABLE이 true이면 사용자 프로세스가 페이지를 수정할 수 있고,
 * 그렇지 않으면 읽기 전용입니다.
 * UPAGE는 아직 매핑되어 있으면 안 됩니다.
 * KPAGE는 palloc_get_page()로 얻은 페이지여야 합니다.
 * 성공 시 true, 이미 매핑되어 있거나 메모리 할당 실패 시 false를 반환합니다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
    struct thread *t = thread_current ();

    /* 해당 가상 주소에 이미 페이지가 없는지 확인한 후 매핑 */
    return (pml4_get_page (t->pml4, upage) == NULL
            && pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기서부터의 코드는 프로젝트 3 이후에 사용됩니다. */

static bool
lazy_load_segment (struct page *page, void *aux) {
    /* TODO: 파일에서 세그먼트를 지연 로드 */
    /* TODO: 이 함수는 주소 VA에서 첫 페이지 폴트 발생 시 호출됩니다. */
    /* TODO: VA는 이 함수가 호출될 때 사용 가능합니다. */
}

/* OFS 오프셋에서 시작하는 세그먼트를 UPAGE에 로드합니다.
 * READ_BYTES + ZERO_BYTES 바이트만큼 초기화합니다. (프로젝트 3)
 * 구현 상세는 위와 동일하나 지연 로드 방식을 사용합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
        uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
    ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT (pg_ofs (upage) == 0);
    ASSERT (ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0) {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* TODO: lazy_load_segment에 전달할 aux 설정 */
        void *aux = NULL;
        if (!vm_alloc_page_with_initializer (VM_ANON, upage,
                    writable, lazy_load_segment, aux))
            return false;

        /* 다음 페이지로 이동 */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* USER_STACK 위치에 스택 페이지를 생성합니다. 성공 시 true 반환. */
static bool
setup_stack (struct intr_frame *if_) {
    bool success = false;
    void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

    /* TODO: stack_bottom에 스택을 매핑하고 즉시 페이지를 확보하세요.
     * TODO: 성공 시 rsp를 적절히 설정하세요.
     * TODO: 페이지를 스택으로 표시해야 합니다. */
    /* TODO: 여기에 코드 작성 */

    return success;
}
#endif /* VM */

struct thread *get_child_process(int pid){
    struct thread *cur = thread_current();
    struct thread *t;
    for(struct list_elem *e = list_begin(&cur->child_list); e != list_end(&cur->child_list); e = list_next(e)) {
        t = list_entry(e, struct child, elem);

        if (pid == t->tid)
            return t;
    }

    return NULL;
}

#ifdef USERPROG
int process_add_file(struct file *f){
    struct thread *cur = thread_current();
    struct file **fdt = cur->fdt;
    if(cur->fd_idx >= FDCOUNT_LIMIT)
        return -1;
    fdt[cur->fd_idx++] = f;
    return cur->fd_idx -1;
}
struct file *process_get_file(int fd){//현재 스레드의 fd번째 파일 정보 얻기
    struct thread *cur = thread_current();
    if(fd >=FDCOUNT_LIMIT)
        return NULL;
    return cur->fdt[fd];
}
int process_close_file(fd){
    struct thread *cur = thread_current();
    if(fd>=FDCOUNT_LIMIT)
        return -1;
    cur->fdt[fd]=NULL;
    return 0;
}

#endif