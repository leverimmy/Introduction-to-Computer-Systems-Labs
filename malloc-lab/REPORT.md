## Malloc 实验报告

### 分离的空闲链表（Segregate Free Lists）

#### 内存布局

- **空闲块**
  - header：$64$ 位
    - 块大小：$62$ 位
    - 前一块是否已分配：$1$ 位
    - 当前块是否已分配：$1$ 位
  - 指向空闲链表中的上一个元素的指针：$64$ 位
  - 指向空闲链表中的下一个元素的指针：$64$ 位
  - footer：$64$ 位
- **已分配的块**
  - header：$64$ 位
    - 块大小：$62$ 位
    - 前一块是否已分配：$1$ 位
    - 当前块是否已分配：$1$ 位
  - payload

#### 具体实现

**宏**

由于在编写代码的过程中需要使用大量的位运算，因此可以像 CS:APP 一样，编写一些常用的宏来简化代码。

```c
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
```

**空闲链表的哨兵节点**

在堆的开头存放每个空闲链表的哨兵节点。在堆的结尾放有一个已分配的、块大小为 $0$ 的尾部边界值，来 **避免边界情况的特殊处理**。

```c
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
```

**分割块的策略**

在分割块的时候，可以根据块大小来选择分配到前半段或后半段，这样就可以使大小相近的块位置相邻，减少外部碎片。在没有使用这样的分割策略之前，我在集群上测试的总分仅为 $51\%$；使用这样的分割策略使总利用率达到了 $92\%$。

```c
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
```

### 分离适配（Segregate Fit）

C 标准库中提供的 GNU malloc 包就是釆用的这种方法，因为这种方法既快速，对内存的使用也很有效率。搜索时间减少了，因为搜索被限制在堆的某个部分，而不是整个堆。

#### 分配

为了分配一个块，必须确定请求的大小类，并且对适当的空闲链表做首次适配，査找一个合适的块。

- 如果找到了一个，那么就分割它，并将剩余的部分插入到适当的空闲链表中。这对应代码中的 `void *split(void *hp, unsigned long occupied, char upper);` 函数。

- 如果找不到合适的块，那么就搜索下一个更大的大小类的空闲链表。

如此重复，直到找到一个合适的块。这对应代码中的 `void *mm_malloc(unsigned long size);` 函数。

如果空闲链表中没有合适的块，那么就向操作系统请求额外的堆内存，从这个新的堆内存中分配出一个块，将剩余部分放置在适当的大小类中。这对应代码中的 `void *extend(unsigned long size);` 函数。

#### 释放

要释放一个块，我们执行合并，并将结果放置到相应的空闲链表中。这对应代码中的 `void mm_free(void *ptr);` 函数。

### 实验结果

```
Results for mm malloc:
trace            name     valid  util     ops      secs   Kops
 1     amptjp-bal.rep       yes   99%    5694  0.000296  19223
 2       cccp-bal.rep       yes   99%    5848  0.000276  21173
 3    cp-decl-bal.rep       yes   99%    6648  0.000370  17992
 4       expr-bal.rep       yes   99%    5380  0.000359  14978
 5 coalescing-bal.rep       yes   89%   14400  0.000339  42465
 6     random-bal.rep       yes   95%    4800  0.000417  11500
 7    random2-bal.rep       yes   94%    4800  0.000417  11522
 8     binary-bal.rep       yes   91%   12000  0.002492   4816
 9    binary2-bal.rep       yes   81%   24000  0.000669  35848
10    realloc-bal.rep       yes   89%   14401  0.000441  32685
11   realloc2-bal.rep       yes   74%   14401  0.000193  74655
Total                             92%  112372  0.006269  17925
```

注意到第 9 个点的内存利用率较低，说明这个引用模式很可能产生了大量的外部碎片。

### 参考资料

1. *Computer Systems: A Programmer's Perspective, Page 597-605*
2. 我参考了 [网上的代码实现](https://github.com/ouuan/course-assignments)，通过该代码我才得以完全理解 `mm_realloc()` 函数的编写细节。

### 总结和感想

我认为 Malloc 实验比前两个实验的难度更高，因为我在这个实验上的耗时最久。它要求我手动实现一个动态内存分配器，我在查阅了相关资料后，选择使用分离适配链表的策略来实现。理论上，使用这种方式实现的效果应该是最好的，因为它吞吐率高；但是对于某些引用模式，它的空间利用率极低。这表明 **产生了大量外部碎片**，可能需要用其他的解决策略去避免产生如此大量的外部碎片。

我觉得经过很长时间的调试以及学习，这个实验给予我的收获很大。