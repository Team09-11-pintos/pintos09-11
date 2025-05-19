#ifndef USERPROG_EXCEPTION_H
#define USERPROG_EXCEPTION_H

/* 페이지 폴트 에러 코드 비트: 예외의 원인을 설명한다. */
#define PF_P 0x1    /* 0: 페이지 없음, 1: 접근 권한 위반 */
#define PF_W 0x2    /* 0: 읽기, 1: 쓰기 */
#define PF_U 0x4    /* 0: 커널 모드, 1: 사용자 프로세스 */

void exception_init (void);
void exception_print_stats (void);

#endif /* userprog/exception.h */
