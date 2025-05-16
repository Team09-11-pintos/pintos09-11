/* ---------- 헤더 포함 ---------- */
#include "devices/timer.h"          // 타이머 관련 선언 모음
#include <debug.h>                  // ASSERT 등 디버그 매크로
#include <inttypes.h>               // 정수형 출력 포맷 매크로
#include <round.h>                  // 반올림 매크로
#include <stdio.h>                  // printf 사용
#include "threads/interrupt.h"      // 인터럽트 등록/제어
#include "threads/io.h"             // inb/outb I/O 포트 접근
#include "threads/synch.h"          // 세마포어·락·조건변수
#include "threads/thread.h"         // 스레드 제어 함수

/* ---------- 8254 타이머(Programmable Interval Timer) 주석 ---------- */
/* See [8254] for hardware details of the 8254 timer chip. */

/* ---------- 컴파일‑타임 검증 ---------- */
#if TIMER_FREQ < 19                 // 매크로 TIMER_FREQ(Hz)가 19 미만이면
#error 8254 timer requires TIMER_FREQ >= 19   // 컴파일 중단
#endif
#if TIMER_FREQ > 1000               // 1 kHz 초과면
#error TIMER_FREQ <= 1000 recommended         // 경고
#endif

/* ---------- 정적 변숫값 ---------- */
static int64_t ticks;               // 부팅 이후 누적 타이머 틱 수


/* loops_per_tick: busy‑wait 보정용 (timer_calibrate()에서 측정) */
static unsigned loops_per_tick;

/* ---------- 내부 함수 forward 선언 ---------- */
static intr_handler_func timer_interrupt;     // 인터럽트 핸들러
static bool too_many_loops (unsigned loops);  // 캘리브레이션 보조
static void busy_wait (int64_t loops);        // 짧은 딜레이 루프
static void real_time_sleep (int64_t num, int32_t denom); // ns/ms/us 슬립

/* ------------------------------------------------------------------ */
/*                  1. 타이머 하드웨어 초기화                           */
/* ------------------------------------------------------------------ */
void timer_init (void)
{
    /* 1193180 Hz 입력 클럭을 원하는 주파수(TIMER_FREQ)로 나눈 카운트 */
    uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

    outb (0x43, 0x34);               // 제어워드: 카운터0, LSB→MSB, 모드2, 바이너리
    outb (0x40, count & 0xff);       // 카운터0 LSB
    outb (0x40, count >> 8);         // 카운터0 MSB

    /* 0x20 (IRQ0) 인터럽트 벡터에 timer_interrupt 등록 */
    intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* ------------------------------------------------------------------ */
/*                  2. 보정(loops_per_tick 측정)                        */
/* ------------------------------------------------------------------ */
void timer_calibrate (void)
{
    unsigned high_bit, test_bit;

    ASSERT (intr_get_level () == INTR_ON);      // 인터럽트 허용 상태 확인
    printf ("Calibrating timer...  ");

    /* 1틱보다 작은 최대 2의 거듭제곱 찾기 */
    loops_per_tick = 1u << 10;                  // 1024부터 시작
    while (!too_many_loops (loops_per_tick << 1)) { // 두 배가 1틱 넘는지?
        loops_per_tick <<= 1;                   // 아직이면 두 배
        ASSERT (loops_per_tick != 0);           // 오버플로 방지
    }

    /* 이어서 8비트(세밀도) 보정 */
    high_bit = loops_per_tick;                  // 기준 비트
    for (test_bit = high_bit >> 1;              // 바로 아래 비트부터
         test_bit != high_bit >> 10;            // 8번 반복
         test_bit >>= 1)
        if (!too_many_loops (high_bit | test_bit))
            loops_per_tick |= test_bit;         // 더해도 1틱 안 넘으면 채택

    printf ("%'"PRIu64" loops/s.\n",
            (uint64_t) loops_per_tick * TIMER_FREQ);
}


/* ------------------------------------------------------------------ */
/*             3. 시간·경과 API (모노토닉 틱 기반)                       */
/* ------------------------------------------------------------------ */
int64_t timer_ticks (void)
{
    enum intr_level old_level = intr_disable (); // 원래 인터럽트 상태 저장·비활성
    int64_t t = ticks;                           // 원자적으로 복사
    intr_set_level (old_level);                  // 상태 복원
    barrier ();                                  // 컴파일러 재주입 방지
    return t;
}
int64_t timer_elapsed (int64_t then) // 현재까지 지난 틱 수
{
    return timer_ticks () - then;                // 현재‑과거 = 경과
}

/* ------------------------------------------------------------------ */
/*                4. 스레드 슬립 (틱 단위)                                */
/* ------------------------------------------------------------------ */
void timer_sleep (int64_t ticks)
{
    int64_t start = timer_ticks ();              // 시작 시각
    ASSERT (intr_get_level () == INTR_ON);       // 슬립 진입 전 인터럽트 허용
	thread_sleep(start+ticks);
    /* 목표 경과 전까지 계속 양보(yield) → busy‑wit 대신 CPU 양도 */
    // while (timer_elapsed (start) < ticks)
    //     thread_yield ();
	//if(timer_elapsed (start)<ticks)//현재 시간(start)기준으로 아직 기다려야 할 tick이 남아있다면
		//thread_sleep(start+ticks); // 그 스레드를 깨워야할 시간(start+ticks)을 인자로 주고 thread_sleep함수를 호출한다
	
	// 	이 함수는 직접 구현해야 하는 함수이고,
	// •	현재 스레드의 wakeup_tick 값을 설정하고,
	// •	sleep_list에 삽입하고,
	// •	thread_block()으로 스레드를 잠재워야 함

}

/* ------------------------------------------------------------------ */
/*                5. 실시간 슬립(ms, us, ns) → 틱 변환                    */
/* ------------------------------------------------------------------ */
void timer_msleep (int64_t ms)  { real_time_sleep (ms, 1000); }
void timer_usleep (int64_t us)  { real_time_sleep (us, 1000*1000); }
void timer_nsleep (int64_t ns)  { real_time_sleep (ns, 1000*1000*1000); }

/* 통계 출력 */
void timer_print_stats (void)
{
    printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* ------------------------------------------------------------------ */
/*           6. 인터럽트 핸들러 (1틱마다 호출)                            */
/* ------------------------------------------------------------------ */
static void timer_interrupt (struct intr_frame *args UNUSED)
{
    ticks++;             // 전역 틱 카운터 증가
    thread_tick ();      // 스케줄러에 “한 틱 지남” 알림
    if (global_tick<=ticks)
	    thread_awake(ticks);//매 인터럽트마다 리스트를 확인해 쓰레드 깨우기

	//struct list_elem *e=list_begin(&sleep_list);
	// if(timer_ticks()>=global_tick){//인터럽트 대상이 있는지 확인
	// 	while(e != list_end(&sleep_list)){
	// 		struct thread *t = list_entry(e,struct thread,elem);
	// 		if(t->wakeup_tick<=timer_ticks()){
	// 		}	
	// 	}
	// }

}

/* ------------------------------------------------------------------ */
/*           7. 보정 보조: loops 돌려보며 1틱 초과 여부 확인              */
/* ------------------------------------------------------------------ */
static bool too_many_loops (unsigned loops)
{
    int64_t start = ticks;          // 틱 변화 관찰

    /* 다음 틱까지 대기 */
    while (ticks == start)
        barrier ();

    start = ticks;                  // 기준 틱
    busy_wait (loops);              // loops 만큼 빈 루프

    barrier ();
    return start != ticks;          // 틱이 변했으면 초과(true)
}

/* ------------------------------------------------------------------ */
/*           8. 짧은 busy‑wait 루프                                     */
/* ------------------------------------------------------------------ */
static void NO_INLINE busy_wait (int64_t loops)
{
    while (loops-- > 0)
        barrier ();                 // NOP + 최적화 방지
}

/* ------------------------------------------------------------------ */
/*         9. 실시간 슬립 내부 구현 (틱 이상은 timer_sleep 이용)           */
/* ------------------------------------------------------------------ */
static void real_time_sleep (int64_t num, int32_t denom)
{
    /* num/denom 초를 틱 단위로 환산 */
    int64_t ticks = num * TIMER_FREQ / denom;

    ASSERT (intr_get_level () == INTR_ON);
    if (ticks > 0) {
        /* 1틱 이상 = cooperative sleep */
        timer_sleep (ticks);
    } else {
        /* 1틱 미만 = busy‑wait로 세밀하게 */
        ASSERT (denom % 1000 == 0);
        busy_wait (loops_per_tick * num / 1000
                    * TIMER_FREQ / (denom / 1000));
    }
}
