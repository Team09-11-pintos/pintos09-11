#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/init.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/* 페이지 할당기.
   한 번에 페이지 크기(또는 그 배수)만큼 메모리를 분배한다.
   더 작은 블록이 필요할 때는 malloc.h 의 할당기를 사용하라.

   시스템 메모리는 커널 풀과 유저 풀, 두 “풀(pool)”로 나뉜다.
   유저 풀은 사용자(가상) 메모리 페이지용,
   커널 풀은 그 외 **모든 것**을 위한 공간이다.
   이렇게 나누는 이유는, 사용자 프로세스가 스와핑을 미친 듯이 해도
   커널이 자체 작업에 쓸 메모리를 확보해 두기 위함이다.

   기본 설정은 시스템 RAM 의 절반을 커널 풀에,
   나머지 절반을 유저 풀에 준다.
   실제론 커널 풀에 과도하게 넉넉하지만
   교육‧실습 목적이라 이렇게 해도 충분하다. */

/* 메모리 풀을 나타내는 구조체 */
struct pool {
	struct lock lock;        /* 상호 배제를 위한 락 */
	struct bitmap *used_map; /* 페이지 사용 여부 비트맵 */
	uint8_t *base;           /* 풀의 시작 주소 */
};

/* 커널 데이터용 풀과 유저 페이지용 풀, 두 개를 운용 */
static struct pool kernel_pool, user_pool;

/* 유저 풀에 넣을 수 있는 최대 페이지 수 */
size_t user_page_limit = SIZE_MAX;

static void init_pool (struct pool *p, void **bm_base,
                       uint64_t start, uint64_t end);
static bool page_from_pool (const struct pool *, void *page);

/* 멀티부트 정보 구조체 (부트로더가 넘겨줌) */
struct multiboot_info {
	uint32_t flags;
	uint32_t mem_low;
	uint32_t mem_high;
	uint32_t __unused[8];
	uint32_t mmap_len;
	uint32_t mmap_base;
};

/* e820 메모리 맵 항목 */
struct e820_entry {
	uint32_t size;
	uint32_t mem_lo;
	uint32_t mem_hi;
	uint32_t len_lo;
	uint32_t len_hi;
	uint32_t type;
};

/* basemem / extmem 범위를 표현하는 구조체 */
struct area {
	uint64_t start;
	uint64_t end;
	uint64_t size;
};

#define BASE_MEM_THRESHOLD 0x100000 /* 1 MB 경계 */
#define USABLE             1        /* e820 타입: 사용 가능 */
#define ACPI_RECLAIMABLE   3        /* e820 타입: ACPI 회수 가능 */
#define APPEND_HILO(hi, lo) (((uint64_t) (hi) << 32) + (lo))

/* e820 항목을 돌며 basemem 과 extmem 범위를 파싱한다. */
static void
resolve_area_info (struct area *base_mem, struct area *ext_mem) {
	struct multiboot_info *mb_info = ptov (MULTIBOOT_INFO);
	struct e820_entry *entries = ptov (mb_info->mmap_base);
	uint32_t i;

	for (i = 0; i < mb_info->mmap_len / sizeof (struct e820_entry); i++) {
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE) {
			uint64_t start = APPEND_HILO (entry->mem_hi, entry->mem_lo);
			uint64_t size  = APPEND_HILO (entry->len_hi, entry->len_lo);
			uint64_t end   = start + size;
			printf ("%llx ~ %llx %d\n", start, end, entry->type);

			struct area *area =
				(start < BASE_MEM_THRESHOLD) ? base_mem : ext_mem;

			/* 해당 영역의 첫 번째 항목이라면 그대로 복사 */
			if (area->size == 0) {
				*area = (struct area) {
					.start = start,
					.end   = end,
					.size  = size,
				};
			} else {        /* 이후 항목이면 범위 확장 */
				if (area->start > start) area->start = start; /* 시작점 확장 */
				if (area->end   < end)   area->end   = end;   /* 끝점 확장 */
				area->size += size;                         /* 크기 누적 */
			}
		}
	}
}

/* -------------------------------------------------------------
 * 풀 채우기(populate)
 *  - 코드 페이지까지 포함한 **모든 페이지**를 이 할당기가 관리한다.
 *  - 기본 정책: 전체 메모리 절반은 커널, 절반은 유저.
 *  - basemem(하위 1 MB)은 최대한 커널 풀로 밀어 넣는다.
 * ----------------------------------------------------------- */
static void
populate_pools (struct area *base_mem, struct area *ext_mem) {
	extern char _end;                 /* 커널 이미지 끝 (linker 기호) */
	void *free_start = pg_round_up (&_end);

	uint64_t total_pages = (base_mem->size + ext_mem->size) / PGSIZE;
	uint64_t user_pages  = total_pages / 2 > user_page_limit
	                       ? user_page_limit
	                       : total_pages / 2;
	uint64_t kern_pages  = total_pages - user_pages;

	/* E820 맵을 순회하며 각 풀에 메모리 영역을 할당 */
	enum { KERN_START, KERN, USER_START, USER } state = KERN_START;
	uint64_t rem = kern_pages;
	uint64_t region_start = 0, end = 0, start, size, size_in_pg;

	struct multiboot_info *mb_info = ptov (MULTIBOOT_INFO);
	struct e820_entry *entries = ptov (mb_info->mmap_base);

	uint32_t i;
	for (i = 0; i < mb_info->mmap_len / sizeof (struct e820_entry); i++) {
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE) {
			start = (uint64_t) ptov (APPEND_HILO (entry->mem_hi, entry->mem_lo));
			size  = APPEND_HILO (entry->len_hi, entry->len_lo);
			end   = start + size;
			size_in_pg = size / PGSIZE;

			if (state == KERN_START) {
				region_start = start;
				state = KERN;
			}

			switch (state) {
				case KERN:
					if (rem > size_in_pg) {           /* 아직 커널 풀 페이지가 남음 */
						rem -= size_in_pg;
						break;
					}
					/* 커널 풀 완성 */
					init_pool (&kernel_pool,
					           &free_start,
					           region_start,
					           start + rem * PGSIZE);
					/* 다음 상태로 전이 */
					if (rem == size_in_pg) {         /* 정확히 채웠을 때 */
						rem = user_pages;
						state = USER_START;
					} else {                         /* 페이지 일부가 유저 풀 몫 */
						region_start = start + rem * PGSIZE;
						rem = user_pages - size_in_pg + rem;
						state = USER;
					}
					break;

				case USER_START:
					region_start = start;
					state = USER;
					break;

				case USER:
					if (rem > size_in_pg) {          /* 아직 유저 풀 페이지가 남음 */
						rem -= size_in_pg;
						break;
					}
					ASSERT (rem == size_in_pg);      /* 마지막 조각이어야 함 */
					break;

				default:
					NOT_REACHED ();
			}
		}
	}

	/* 유저 풀 생성 */
	init_pool (&user_pool, &free_start, region_start, end);

	/* ----------------------------------------
	 * usable 페이지 비트맵 갱신
	 * 커널 이미지가 차지한 영역 등을 ‘사용 중’ 처리
	 * -------------------------------------- */
	uint64_t usable_bound = (uint64_t) free_start;
	struct pool *pool;
	void *pool_end;
	size_t page_idx, page_cnt;

	for (i = 0; i < mb_info->mmap_len / sizeof (struct e820_entry); i++) {
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE) {
			uint64_t start = (uint64_t)
				ptov (APPEND_HILO (entry->mem_hi, entry->mem_lo));
			uint64_t size = APPEND_HILO (entry->len_hi, entry->len_lo);
			uint64_t end = start + size;

			/* 0x1000 ~ 0x200000 구간은 커널이 이미 쓰므로 제외 */
			if (end < usable_bound)
				continue;

			/*  사용 가능한 첫 페이지 경계로 올림 */
			start = (uint64_t)
				pg_round_up (start >= usable_bound ? start : usable_bound);
split:
			if (page_from_pool (&kernel_pool, (void *) start))
				pool = &kernel_pool;
			else if (page_from_pool (&user_pool, (void *) start))
				pool = &user_pool;
			else
				NOT_REACHED ();

			pool_end = pool->base + bitmap_size (pool->used_map) * PGSIZE;
			page_idx = pg_no (start) - pg_no (pool->base);
			if ((uint64_t) pool_end < end) {
				/* 풀 끝까지 한 번에 표시하고 나머지 구간으로 루프 */
				page_cnt = ((uint64_t) pool_end - start) / PGSIZE;
				bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
				start = (uint64_t) pool_end;
				goto split;
			} else {
				page_cnt = ((uint64_t) end - start) / PGSIZE;
				bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
			}
		}
	}
}

/* 페이지 할당기 초기화 & 메모리 크기 반환 */
uint64_t
palloc_init (void) {
	extern char _end;                /* 링크 스크립트가 기록한 커널 끝 */
	struct area base_mem = { .size = 0 };
	struct area ext_mem  = { .size = 0 };

	resolve_area_info (&base_mem, &ext_mem);

	printf ("Pintos booting with:\n");
	printf ("\tbase_mem: 0x%llx ~ 0x%llx (usable: %'llu kB)\n",
	        base_mem.start, base_mem.end, base_mem.size / 1024);
	printf ("\text_mem : 0x%llx ~ 0x%llx (usable: %'llu kB)\n",
	        ext_mem.start,  ext_mem.end,  ext_mem.size  / 1024);

	populate_pools (&base_mem, &ext_mem);
	return ext_mem.end;
}

/* -------------------------------------------------------------
 * 페이지 묶음(page_cnt 개)을 얻어 시작 주소 반환
 *   PAL_USER  : 유저 풀에서 할당
 *   PAL_ZERO  : 0으로 초기화
 *   PAL_ASSERT: 실패 시 커널 패닉
 * ----------------------------------------------------------- */
void *
palloc_get_multiple (enum palloc_flags flags, size_t page_cnt) {
	struct pool *pool = (flags & PAL_USER) ? &user_pool : &kernel_pool;

	lock_acquire (&pool->lock);
	size_t page_idx = bitmap_scan_and_flip (pool->used_map,
	                                        0, page_cnt, false);
	lock_release (&pool->lock);

	void *pages = (page_idx != BITMAP_ERROR)
	              ? pool->base + PGSIZE * page_idx
	              : NULL;

	if (pages) {
		if (flags & PAL_ZERO)
			memset (pages, 0, PGSIZE * page_cnt);
	} else if (flags & PAL_ASSERT) {
		PANIC ("palloc_get: out of pages");
	}
	return pages;
}

/* 단일 페이지를 얻어 반환 (래퍼 함수) */
void *
palloc_get_page (enum palloc_flags flags) {
	return palloc_get_multiple (flags, 1);
}

/* PAGE_CNT 만큼의 페이지를 해제 */
void
palloc_free_multiple (void *pages, size_t page_cnt) {
	struct pool *pool;
	size_t page_idx;

	ASSERT (pg_ofs (pages) == 0);
	if (pages == NULL || page_cnt == 0)
		return;

	if (page_from_pool (&kernel_pool, pages))
		pool = &kernel_pool;
	else if (page_from_pool (&user_pool, pages))
		pool = &user_pool;
	else
		NOT_REACHED ();

	page_idx = pg_no (pages) - pg_no (pool->base);

#ifndef NDEBUG
	memset (pages, 0xcc, PGSIZE * page_cnt);  /* 패턴 채워 디버깅 도움 */
#endif
	ASSERT (bitmap_all (pool->used_map, page_idx, page_cnt));
	bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
}

/* 단일 페이지 해제 (래퍼) */
void
palloc_free_page (void *page) {
	palloc_free_multiple (page, 1);
}

/* 풀 P를 START~END 범위로 초기화 */
static void
init_pool (struct pool *p, void **bm_base,
           uint64_t start, uint64_t end) {
	/* 비트맵을 풀 시작부에 배치.
	   필요한 비트맵 크기를 계산해 풀 크기에서 차감 */
	uint64_t pgcnt    = (end - start) / PGSIZE;
	size_t   bm_pages = DIV_ROUND_UP (bitmap_buf_size (pgcnt), PGSIZE) * PGSIZE;

	lock_init (&p->lock);
	p->used_map = bitmap_create_in_buf (pgcnt, *bm_base, bm_pages);
	p->base     = (void *) start;

	/* 일단 전부 ‘사용 중’(true)으로 마킹 */
	bitmap_set_all (p->used_map, true);

	*bm_base += bm_pages;
}

/* PAGE 가 POOL 에서 할당된 것인지 확인 */
static bool
page_from_pool (const struct pool *pool, void *page) {
	size_t page_no   = pg_no (page);
	size_t start_pg  = pg_no (pool->base);
	size_t end_pg    = start_pg + bitmap_size (pool->used_map);
	return page_no >= start_pg && page_no < end_pg;
}
