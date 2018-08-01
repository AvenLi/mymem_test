#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "malloc.h"	


#define MEM_BLOCK_SIZE			((uint32_t)64)  	  						
#define MEM_MAX_SIZE			((uint32_t)8*1024*1024)
#define MEM_ALLOC_TABLE_SIZE	((uint32_t)(MEM_MAX_SIZE/MEM_BLOCK_SIZE))
#define MALLOC_OFFSET(X)         ((X)<<6)
#define FREE_OFFSET(X)           ((X)>>6)
#define __ALIGN  8//小块区域的上界
#define __MAX_BYTES 128//小块区域的下降
#define __NFREELISTS  16 // _MAX_BYTES/_ALIGN，有多少个区域

#define  FREELIST_INDEX(bytes)  (((bytes) + __ALIGN - 1)/__ALIGN - 1)
#define  ROUND_UP(bytes)  (((bytes) + __ALIGN - 1) & ~(__ALIGN - 1))//等价于(bytes + 7) / 8



union obj
{
    union obj * free_list_link;
    char client_data[1];
};

typedef union obj  obj_t;
static  obj_t*   volatile   free_list[__NFREELISTS];


static  char              s_MemPool[MEM_MAX_SIZE] __attribute__ ((aligned (4)));
static  uint32_t          s_MemMaps[MEM_ALLOC_TABLE_SIZE];
static  volatile bool     s_Membusy = false;
static char *start_free;//内存池可用空间的起始位置，初始化为0
static char *end_free;//内存池可用空间的结束位置,初始化为0
static size_t heap_size = 8*1024*1024;//内存池的总大小


static char *chunk_alloc(size_t size, int *nobjs);
void* refill(size_t n) ;


void memory_init(void)
{
	memset(s_MemPool,0,MEM_MAX_SIZE);
	memset(s_MemMaps,0,MEM_ALLOC_TABLE_SIZE*4);
    s_Membusy = false;
    start_free = 0;
    end_free = 0;

}


static void * allocate(size_t n)
{
    obj_t* volatile *my_free_list;
     obj_t *  result;

    if (n > (size_t) __MAX_BYTES)//大于128字节调用第一级配置器
    {
        return malloc(n);
    }
    my_free_list = &free_list[FREELIST_INDEX(n)];//根据申请空间的大小寻找相应的空闲链表（16个空闲链表中的一个）
    result = *my_free_list;
    if (result == 0)//如果该空闲链表没有空闲的数据块
    {
        void *r = refill(ROUND_UP(n));//为该空闲链表填充新的空间
        return r;
    }
    *my_free_list = result -> free_list_link;//如果空闲链表中有空闲数据块，则取出一个，并把空闲链表的指针指向下一个数据块
    return (result->client_data);
}



static void deallocate(void *p, size_t n)
{
    obj_t   *q = (obj_t *)p;
    obj_t * volatile * my_free_list;

    if (n > (size_t) __MAX_BYTES)//如果空间大于128字节，采用普通的方法析构
    {
        free(p);
        return;
    }
    my_free_list = free_list + FREELIST_INDEX(n);//否则将空间回收到相应空闲链表（由释放块的大小决定）中
    q -> free_list_link = (obj_t*)my_free_list;
    *my_free_list = q;
}


void* refill(size_t n)
{
    int nobjs = 20;
    char * chunk = chunk_alloc(n, nobjs);//从内存池里取出nobjs个大小为n的数据块,返回值nobjs为真实申请到的数据块个数，注意这里nobjs个大小为n的数据块所在的空间是连续的
    obj_t *  volatile * my_free_list;
    obj_t * result;
    obj_t * current_obj, * next_obj;
    int i;

    if (1 == nobjs)
        return(chunk);//如果只获得一个数据块，那么这个数据块就直接分给调用者，空闲链表中不会增加新节点
    my_free_list = &free_list[FREELIST_INDEX(n)];//否则根据申请数据块的大小找到相应空闲链表
    result = (obj_t *)chunk;
    *my_free_list = next_obj = (obj_t *)(chunk + n);//第0个数据块给调用者，地址访问即chunk~chunk + n - 1
    for (i = 1; ; i++)//1~nobjs-1的数据块插入到空闲链表
    {
        current_obj = next_obj;
        next_obj = (obj_t *)((char *)next_obj + n);//由于之前内存池里申请到的空间连续，所以这里需要人工划分成小块一次插入到空闲链表

        if (nobjs - 1 == i)
        {
            current_obj -> free_list_link = 0;
            break;
        }
        else
        {
            current_obj -> free_list_link = next_obj;
        }
    }
    return(result);
}


static char *chunk_alloc(size_t size, int *nobjs)
{
    char * result;
    size_t total_bytes = size * *nobjs;//需要申请空间的大小
    size_t bytes_left = end_free - start_free;//计算内存池剩余空间

    //如果内存池剩余空间完全满足需求量
    if (bytes_left >= total_bytes)
    {
        result = start_free;
        start_free += total_bytes;
        return(result);
    }
        //内存池剩余空间不满足需求量，但是至少能够提供一个以上数据块
    else if (bytes_left >= size)
    {
        *nobjs = bytes_left / size;
        total_bytes = size * *nobjs;
        result = start_free;
        start_free += total_bytes;
        return(result);
    }
        //剩余空间连一个数据块（大小为size）也无法提供
    else
    {
        size_t bytes_to_get = 2 * total_bytes + ROUND_UP(heap_size >> 4);

        //内存池的剩余空间分给合适的空闲链表
        if (bytes_left > 0)
        {
             obj_t *  volatile * my_free_list = &free_list [FREELIST_INDEX(bytes_left)];

            ((obj_t *)start_free) -> free_list_link = *my_free_list;
            *my_free_list = (obj_t *)start_free;
        }
        start_free = (char *)malloc(bytes_to_get);//配置heap空间，用来补充内存池
        if (0 == start_free)
        {
            int i;
            obj_t * volatile * my_free_list, *p;

            //从空闲链表中找出一个比较大的空闲数据块还给内存池（之后会将这个大的空闲数据块切成多个小的空闲数据块再次加入到空闲链表）
            for (i = size; i <= __MAX_BYTES; i += __ALIGN)
            {
                my_free_list = &free_list[FREELIST_INDEX(i)];
                p = *my_free_list;
                if (0 != p)
                {
                    *my_free_list = p -> free_list_link;
                    start_free = (char *)p;
                    end_free = start_free + i;
                    return(chunk_alloc(size, nobjs));//递归调用自己，为了修正nobjs
                }
            }
            end_free = 0;
            start_free = (char *)malloc(bytes_to_get);//如果连这个大的数据块都找不出来则调用第一级配置器
        }
        //如果分配成功
        heap_size += bytes_to_get;//内存池大小增加
        end_free = start_free + bytes_to_get;//修改内存池可用空间的结束位置
        return(chunk_alloc(size, nobjs));//递归调用自己，为了修正nobjs
    }
}


#define malloc_size 100
#define usr_malloc(x)  allocate(x)
#define usr_free(x)   deallocate(x,malloc_size)
int main()
{
	memory_init();
	uint32_t count;
	clock_t start;
	clock_t end;
	char *buf;
	while(1)
	{
		start = clock();
		for(count = 0;count < 1000000;count++)
		{
			
			buf = usr_malloc(malloc_size);
			if(buf == NULL)
			{
				exit(0);
			}
			usr_free(buf);

		}
		end = clock();
		printf("total time = %ld",(end-start));
	}
}