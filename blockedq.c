#include <string.h>
#include "blockedq.h"

void blockedq_init(struct blockedq * bq){
  memset(bq->queue, -1, sizeof(int)*MAX_USERS);
  bq->count = 0;
}

int blockedq_enq(struct blockedq * bq, const int p){
  if(bq->count < MAX_USERS){
    bq->queue[bq->count++] = p;
    return bq->count - 1;
  }else{
    return -1;
  }
}


static void blockedq_shift(struct blockedq * bq, const int pos){
  int i;
  for(i=pos; i < bq->count; i++){
    bq->queue[i] = bq->queue[i+1];
  }
  bq->queue[i] = -1;
}

int blockedq_deq(struct blockedq * bq, const int pos){
  const unsigned int pi = bq->queue[pos];
  bq->count--;
  blockedq_shift(bq, pos);
  return pi;
}

// find user we can unblocked
int blockedq_ready(struct blockedq * bq, const struct vclock * clock, const struct process * procs){
  int i;
  for(i=0; i < bq->count; i++){
    const int pi = bq->queue[i];
    if( (clock->sec > procs[pi].vclk[BLOCKED_TIME].sec) ||
       ((clock->sec == procs[pi].vclk[BLOCKED_TIME].sec) && (clock->ns > procs[pi].vclk[BLOCKED_TIME].ns))
      ){	//if our event time is reached
      return blockedq_deq(bq, i);
    }
  }
  return -1;
}

int blockedq_top(struct blockedq * bq){
  return bq->queue[0];
}

int blockedq_size(struct blockedq * bq){
  return bq->count;
}
