#include "mm.h"
#include "memlib.h"
#include <string.h>

team_t team = {
    "熊泽恩",
    "熊泽恩",
    "2022011223",
    "",
    ""
};

#define WSIZE (sizeof(unsigned long))
#define DSIZE (2 * WSIZE)
#define MIN_SIZE (4 * WSIZE)
// 对齐大小
#define ALIGNMENT DSIZE
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

// 间接引用以及赋值
#define GET(p) (*(unsigned long *)(p))
#define PUT(p, val) (*(unsigned long *)(p) = val)

// 块的组成部分
#define HEADER(bp) ((unsigned long *)(bp) - 1)
#define BLOCK_SIZE(hp) (GET(hp) & -4)
#define PAYLOAD(hp) ((unsigned long *)(hp) + 1)
#define FOOTER(hp) ((char *)(hp) + BLOCK_SIZE(hp) - WSIZE)

#define IS_ALLOCATED(hp) (GET(hp) & 1)
#define SET_ALLOCATED(hp) (GET(hp) |= 1)
#define UNSET_ALLOCATED(hp) (GET(hp) &= ~1)
#define IS_PREV_ALLOCATED(hp) (GET(hp) & 2)
#define SET_PREV_ALLOCATED(hp) (GET(hp) |= 2)
#define UNSET_PREV_ALLOCATED(hp) (GET(hp) &= ~2)
#define SET_BLOCK_SIZE(hp, val) (GET(hp) = ((GET(hp) & 3) | (val)))
#define SET_FOOTER(hp, val) (PUT(FOOTER(hp), (val)))

#define PREV_HEADER(hp) ((char *)(hp) - *((unsigned long *)(hp) - 1))
#define NEXT_HEADER(hp) ((char *)(hp) + BLOCK_SIZE(hp))
#define PREV_BLK(hp) (*((void **)(hp) + 1))
#define NEXT_BLK(hp) (*((void **)(hp) + 2))

// SFL 的相关参数
#define L 0
#define R 61
#define LOG2(size) (sizeof(size) * 8 - __builtin_clzll(size))
#define GUARD(k) ((unsigned long *)mem_heap_lo() + (k - L) * 2 - 1)

// Magic Numbers 调参的参数
#define PARA1 4096
#define PARA2 100

// 将 hp 从 SFL 中删除
static void remove_from_sfl(void *hp) {
    NEXT_BLK(PREV_BLK(hp)) = NEXT_BLK(hp);
    PREV_BLK(NEXT_BLK(hp)) = PREV_BLK(hp);
}

// 将 hp 添加到 SFL 末尾
static void insert_into_sfl(void *hp) {
    void *guard = GUARD(LOG2(BLOCK_SIZE(hp)));
    void *prev = PREV_BLK(guard);
    
    PREV_BLK(hp) = prev;
    NEXT_BLK(prev) = hp;
    NEXT_BLK(hp) = guard;
    PREV_BLK(guard) = hp;
}

// 向前合并空闲块
static void *merge_previous(void *hp) {
    unsigned long merged = BLOCK_SIZE(hp);
    void *prev = PREV_HEADER(hp);
    if (!IS_PREV_ALLOCATED(hp)) {
        merged += BLOCK_SIZE(prev);
        remove_from_sfl(prev);
        hp = prev;
    }
    SET_BLOCK_SIZE(hp, merged);
    UNSET_ALLOCATED(hp);
    SET_FOOTER(hp, merged);
    return hp;
}

// 向后合并空闲块
static void *merge_next(void *hp) {
    unsigned long merged = BLOCK_SIZE(hp);
    void *next = NEXT_HEADER(hp);
    if (!IS_ALLOCATED(next)) {
        merged += BLOCK_SIZE(next);
        remove_from_sfl(next);
    }
    SET_BLOCK_SIZE(hp, merged);
    UNSET_ALLOCATED(hp);
    SET_FOOTER(hp, merged);
    return hp;
}

// 合并前后的空闲块
static void *coalesce(void *hp) {
    hp = merge_previous(hp);
    hp = merge_next(hp);
    return hp;
}

// 新建一个空闲块
static void *extend(unsigned long size) {
    unsigned long *hp = mem_sbrk(size);
    if (hp == (void *)(-1))
        return NULL;
    hp--;
    SET_BLOCK_SIZE(hp, size);

    // 设置新的尾部边界值
    SET_ALLOCATED((char *)hp + size);
    return hp;
}

static void *split(void *hp, unsigned long occupied, char upper) {
    unsigned long vacancy = BLOCK_SIZE(hp) - occupied;
    if (vacancy >= MIN_SIZE) {
        if (upper) {
            // 分离出未分配的 hp 和已分配的 hq
            SET_BLOCK_SIZE(hp, vacancy);
            UNSET_ALLOCATED(hp);
            SET_FOOTER(hp, vacancy);
            insert_into_sfl(hp);

            void *hq = (char *)hp + vacancy;
            SET_BLOCK_SIZE(hq, occupied);
            SET_ALLOCATED(hq);
            UNSET_PREV_ALLOCATED(hq);
            SET_PREV_ALLOCATED(NEXT_HEADER(hq));

            hp = hq;
        } else {
            // 分离已分配的 hp 和未分配的 hq
            SET_BLOCK_SIZE(hp, occupied);

            void *hq = (char *)hp + occupied;
            SET_BLOCK_SIZE(hq, vacancy);
            SET_PREV_ALLOCATED(hq);
            UNSET_ALLOCATED(hq);
            SET_FOOTER(hq, vacancy);
            insert_into_sfl(hq);
        }
    }
    return hp;
}

int mm_init(void) {
    unsigned long end = ALIGN((unsigned long)mem_heap_lo() + ((R - L) * 2 + 3) * WSIZE);
    if (mem_sbrk(end - (unsigned long)mem_heap_lo()) == (void *)(-1))
        return -1;

    // 初始化哨兵
    for (int k = L; k <= R; ++k) {
        void *guard = GUARD(k);
        NEXT_BLK(guard) = PREV_BLK(guard) = guard;
    }

    // 设置尾部边界值
    SET_ALLOCATED(end - WSIZE);
    SET_PREV_ALLOCATED(end - WSIZE);
    return 0;
}

void *mm_malloc(unsigned long size) {
    // 进行对齐
    size = MAX(ALIGN(size + WSIZE), MIN_SIZE);

    void *hp;
    char found = 0;

    // 找最合适的空余块
    for (int k = LOG2(size); k <= R && !found; ++k) {
        void *guard = GUARD(k);
        for (void *it = NEXT_BLK(guard); it != guard && !found; it = NEXT_BLK(it)) {
            if (BLOCK_SIZE(it) >= size) {
                remove_from_sfl(it);
                found = 1;
                hp = it;
            }
        }
    }

    // 如果没找到空余块，就新开一个
    if (!found) {
        hp = extend(MAX(size, PARA1));
        if (hp == NULL)
            return NULL;
    }

    SET_ALLOCATED(hp);
    SET_PREV_ALLOCATED(NEXT_HEADER(hp));

    return PAYLOAD(split(hp, size, size > PARA2));
}

// 释放一个以前分配了的块
void mm_free(void *ptr) {
    if (ptr == NULL)
        return;
    void *hp = coalesce(HEADER(ptr));
    insert_into_sfl(hp);
    UNSET_PREV_ALLOCATED(NEXT_HEADER(hp));
}

// 重新分配
void *mm_realloc(void *ptr, unsigned long size) {
    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }
    if (ptr == NULL)
        return mm_malloc(size);

    void *hp = HEADER(ptr);
    unsigned long needed = MAX(ALIGN(size + WSIZE), MIN_SIZE);
    unsigned long merged = BLOCK_SIZE(hp);

    if (merged < needed) {
        void *next = NEXT_HEADER(hp);
        if (!IS_ALLOCATED(next)) {
            merged += BLOCK_SIZE(next);
            remove_from_sfl(next);
        }
        if (merged >= needed) {
            SET_PREV_ALLOCATED(NEXT_HEADER(next));
            GET(hp) += BLOCK_SIZE(next);
        } else {
            void *oldhp = hp;
            if (!IS_PREV_ALLOCATED(hp)) {
                hp = PREV_HEADER(hp);
                merged += BLOCK_SIZE(hp);
                remove_from_sfl(hp);
            }
            if (merged >= needed) {
                SET_BLOCK_SIZE(hp, merged);
                SET_ALLOCATED(hp);
                SET_PREV_ALLOCATED(NEXT_HEADER(hp));
                memmove(PAYLOAD(hp), PAYLOAD(oldhp), BLOCK_SIZE(oldhp) - WSIZE);
            } else {
                SET_BLOCK_SIZE(hp, merged);
                SET_ALLOCATED(hp);
                SET_PREV_ALLOCATED(NEXT_HEADER(hp));

                void *bp = mm_malloc(size);
                if (bp == NULL)
                    return NULL;
                memcpy(bp, PAYLOAD(oldhp), BLOCK_SIZE(oldhp) - WSIZE);

                UNSET_ALLOCATED(hp);
                SET_FOOTER(hp, merged);
                UNSET_PREV_ALLOCATED(NEXT_HEADER(hp));
                insert_into_sfl(hp);

                return bp;
            }
        }
    }

    return PAYLOAD(split(hp, needed, 0));
}