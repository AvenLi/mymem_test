#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include "malloc.h"	


#define MEM_BLOCK_SIZE			 ((uint32_t)64)
#define MEM_MAX_SIZE			 ((uint32_t)8*1024*1024ul)
#define MEM_ALLOC_TABLE_SIZE	 ((uint32_t)(MEM_MAX_SIZE/MEM_BLOCK_SIZE))
#define MALLOC_OFFSET(X)         ((uint32_t)(X)<<6u)
#define FREE_OFFSET(X)           ((X)>>6u)
#define __ALIGN  8u//小块区域的上界
#define __MAX_BYTES 128u//小块区域的下降
#define __NFREELISTS  (__MAX_BYTES/__ALIGN) // _MAX_BYTES/_ALIGN，有多少个区域

#define  FREELIST_INDEX(bytes)  (((bytes) + __ALIGN - 1)/__ALIGN - 1)
#define  ROUND_UP(bytes)        (((bytes) + __ALIGN - 1) & (~(__ALIGN - 1)))//等价于(bytes + 7) / 8



union obj
{
    union obj * free_list_link;
    char client_data[1];
};

typedef union obj  obj_t;
static  obj_t*   volatile   free_list[__NFREELISTS];


static  char              s_MemPool[MEM_MAX_SIZE] __attribute__ ((aligned (__ALIGN)));
static  uint32_t          s_MemMaps[MEM_ALLOC_TABLE_SIZE];
static  uint32_t          s_MemRemain;
static char *start_free;  //内存池可用空间的起始位置，初始化为0
static char *end_free;    //内存池可用空间的结束位置,初始化为0
static volatile  bool     mem_busy = false;
static volatile  uint32_t heap_size;
static char *chunk_alloc(uint32_t size, int *nobjs);
void* refill(uint32_t n) ;


void memory_init(void)
{
	memset(s_MemPool,0,MEM_MAX_SIZE);
	memset(s_MemMaps,0,MEM_ALLOC_TABLE_SIZE*4);
    mem_busy = false;
    start_free = 0;
    end_free = 0;
    start_free =0;
    heap_size = 0;
}
static void *mymalloc (uint32_t size)
{
    int32_t  offset=0;
    uint32_t nmemb;
	uint32_t cmemb=0;
    if(size == 0)
	{
		return NULL;
    }
    nmemb = size;
    nmemb >>=6; //get block
    if(size&(MEM_BLOCK_SIZE-1))
    {
        nmemb++;
    }
    for(offset = MEM_ALLOC_TABLE_SIZE-1;offset>= 0;offset--)
    {
		if(s_MemMaps[offset] == 0)
        {
            cmemb ++;
        }
		else
        {
            cmemb = 0;
        }
        if(cmemb < nmemb)
            continue;
        for(uint16_t  i=0;i<nmemb;i++)
        {
            s_MemMaps[offset+i] = nmemb;
        }
        return (void*)(s_MemPool+MALLOC_OFFSET(offset));
    }
    return NULL;
}

static void  myfree(void *pointer)
{
	uint32_t offset;
	uint32_t block_num;
	if(pointer == NULL)
	{
		return;
	}
	offset=(uint32_t)((char*)pointer - &s_MemPool[0]);
    if(offset > MEM_MAX_SIZE)
    {
		return;
    }
    offset = FREE_OFFSET(offset);
    block_num  = s_MemMaps[offset];
    for(int i =0; i< block_num; i++)
    {
        s_MemMaps[offset+i]=0;
    }
	pointer = NULL;
}



 void * allocate(uint32_t bytes)
{
    obj_t* volatile *my_free_list;
    obj_t *  result;
    int    count;
    if (bytes <  __MAX_BYTES)
    {
        while( mem_busy );
        mem_busy = true;
        my_free_list = &free_list[FREELIST_INDEX(bytes)];
        result = *my_free_list;
        if(result != NULL)
        {
            *my_free_list = result->free_list_link;
        }
        else
        {
            int nobjs = 10;
            char *chunk = mymalloc(ROUND_UP(bytes)*10);
            if(chunk == NULL)  //has no memory anymore
            {
                for (count = bytes; count <= __MAX_BYTES; count += __ALIGN)
                {
                    my_free_list = &free_list[FREELIST_INDEX(count)];
                    result = *my_free_list;
                    if (result != NULL)
                    {
                        *my_free_list = result -> free_list_link;
                    }
                }
                return result;
            }
            heap_size += ROUND_UP(bytes)*10;
            obj_t * current_obj, * next_obj;
            my_free_list = &free_list[FREELIST_INDEX(bytes)];
            result = (obj_t *)chunk;
            *my_free_list = next_obj = (obj_t *)(chunk + bytes);
            for (count = 1; ; count++)
            {
                current_obj = next_obj;
                next_obj = (obj_t *)((char *)next_obj + bytes);
                if (nobjs - 1 == count)
                {
                    current_obj -> free_list_link = 0;
                    break;
                }
                else
                {
                    current_obj -> free_list_link = next_obj;
                }
            }
        }
        mem_busy = false;
    }
    else
    {
        result =  mymalloc(bytes);
    }
    return result->client_data;
}



 void deallocate(void *p, uint32_t n)
{
    obj_t   *q = (obj_t *)p;
    obj_t * volatile * my_free_list;
    while(mem_busy);
    mem_busy = true;
    if (n < (uint32_t) __MAX_BYTES)//如果空间大于128字节，采用普通的方法析构
    {
        my_free_list = &free_list[FREELIST_INDEX(n)];//否则将空间回收到相应空闲链表（由释放块的大小决定）中
        q->free_list_link = (obj_t *) my_free_list;
        *my_free_list = q;
    }
    else{
        myfree(p);
    }
   mem_busy = false;
}


void* refill(uint32_t n)
{
    int nobjs = 10;
    char * chunk = chunk_alloc(n, &nobjs);//从内存池里取出nobjs个大小为n的数据块,返回值nobjs为真实申请到的数据块个数，注意这里nobjs个大小为n的数据块所在的空间是连续的
    if(chunk == NULL)
    {
        return NULL;
    }
    obj_t *  volatile * my_free_list;
    obj_t * result;
    obj_t * current_obj, * next_obj;
    int i;
    if (1 == nobjs)
    {
        return(chunk);
    }
    my_free_list = &free_list[FREELIST_INDEX(n)];
    result = (obj_t *)chunk;
    *my_free_list = next_obj = (obj_t *)(chunk + n);
    for (i = 1; ; i++)
    {
        current_obj = next_obj;
        next_obj = (obj_t *)((char *)next_obj + n);
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


static char *chunk_alloc(uint32_t size, int *nobjs)
{
    char * result;
    uint32_t total_bytes = size * *nobjs;
    uint32_t bytes_left = (uint32_t )(end_free - start_free);
    if (bytes_left >= total_bytes)
    {
        result = start_free;
        start_free += total_bytes;
        return(result);
    }
    else if (bytes_left >= size)
    {
        *nobjs = (int)(bytes_left / size);
        total_bytes = size * *nobjs;
        result = start_free;
        start_free += total_bytes;
        return(result);
    }
    else
    {
        size_t bytes_to_get = 2 * total_bytes + ROUND_UP(heap_size >> 4);
        if (bytes_left > 0)
        {
             obj_t *  volatile * my_free_list = &free_list [FREELIST_INDEX(bytes_left)];
            ((obj_t *)start_free) -> free_list_link = *my_free_list;
            *my_free_list = (obj_t *)start_free;
        }
        start_free = (char *)mymalloc(bytes_to_get);
        if (NULL == start_free)
        {
            int i;
            obj_t * volatile * my_free_list, *p;
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
            return NULL;//如果连这个大的数据块都找不出来则调用第一级配置器
        }
        heap_size += bytes_to_get;//内存池大小增加
        end_free = start_free + bytes_to_get;//修改内存池可用空间的结束位置
        return(chunk_alloc(size, nobjs));//递归调用自己，为了修正nobjs
    }
}


#define malloc_size     (rand()%100+10)
#define usr_malloc(x)   allocate(x)
#define usr_free(x,y)   deallocate(x,y)


int main()
{
	memory_init();
	uint32_t count;
	clock_t start;
	clock_t end;
	char *buf;
	int size ;
	while(1)
	{
		start = clock();
		for(count = 0;count < 10000000;count++)
		{
            size = malloc_size;
			buf = usr_malloc(size);
			if(buf == NULL)
			{
			    printf("malloc count = %ld\r\n",count);
                return  0;
			}
            memset(buf,'A',size);
            usr_free(buf,size);
		}
		end = clock();
		printf("size = %d,total time = %ld\r\n",size,(end-start));
	}
}
