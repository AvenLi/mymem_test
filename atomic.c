#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>

#define PRODATA_FIFO_MAX  1024u

#define ATOMIC_ADD    __sync_fetch_and_add
#define ATOMIC_SUB    __sync_fetch_and_sub

static struct
{
    uint16_t   pcmd[PRODATA_FIFO_MAX];
    uint16_t  front;
    uint16_t  rear;
    uint16_t  count;
}s_ProcmdFifo = {{0},ATOMIC_VAR_INIT(0),ATOMIC_VAR_INIT(0),ATOMIC_VAR_INIT(0)};


static inline bool insert_command(uint16_t cdm)
{
    uint16_t  temp;
    if(atomic_load(&s_ProcmdFifo.count)== PRODATA_FIFO_MAX)
    {
        return false;
    }
    s_ProcmdFifo.pcmd[s_ProcmdFifo.rear] = cdm;
    s_ProcmdFifo.rear = (s_ProcmdFifo.rear + 1)&(PRODATA_FIFO_MAX - 1);
    ATOMIC_ADD(&s_ProcmdFifo.count,1);
    return true;
}

static uint16_t read_command(void)
{
    uint16_t  sql_cmd;
    if(atomic_load(&s_ProcmdFifo.count) == 0)
    {
        return 0;
    }
    sql_cmd = s_ProcmdFifo.pcmd[s_ProcmdFifo.front];
    s_ProcmdFifo.front = (s_ProcmdFifo.front + 1)&(PRODATA_FIFO_MAX - 1);
    ATOMIC_SUB(&s_ProcmdFifo.count,1);
    return  sql_cmd;
}




void *test(void *pdata)
{
    uint16_t  res;
    while(1)
    {
        res = read_command();
        if(res > 0)
        {
            printf("read count = %d\r\n",res);
        }
    }
}

int main()
{
    pthread_t  id;
    pthread_create(&id,NULL,test,NULL);
    uint16_t  cnt = 1;
    while(1)
    {
        if(insert_command(cnt++))
        {
           // printf("save count = %d\r\n",cnt);
            usleep(10);
        }
    }
}

