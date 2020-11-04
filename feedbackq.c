#include <string.h>
#include "feedbackq.h"

static void feedbackq_zero(struct feedbackq  * fq, const int q){
  memset(fq->queue, -1, sizeof(int)*MAX_USERS);

  fq->count = 0;
  fq->quant = q;
}

void feedbackq_init(struct feedbackq  fq[FEEDBACK_LEVELS]){
  int i, q = QUANTUM_NS;
  for(i=1; i < FEEDBACK_LEVELS; i++){
    feedbackq_zero(&fq[i], q);
    q *= 2; //next q gets half the quantum
  }
}


int feedbackq_ready(struct feedbackq  fq[FEEDBACK_LEVELS], const struct process * procs){

  int i;
  for(i=0; i < FEEDBACK_LEVELS; i++){

    const int pi = fq[i].queue[0];
    if(procs[pi].state == READY){    /* if process is ready */
      return i;
    }
  }
  return -1;
}


int feedbackq_enq(struct feedbackq  * fq, const int pi){
  if(fq->count < MAX_USERS){
    fq->queue[fq->count++] = pi;
    return fq->count - 1;
  }else{
    return -1;
  }
}

//Pop item at pos, from queue
int feedbackq_deq(struct feedbackq  * fq, const int pos){

  const unsigned int pi = fq->queue[pos];
  fq->count--;

  //shift queue left
  int i;
  for(i=pos; i < fq->count; i++){
    fq->queue[i] = fq->queue[i+1];
  }
  fq->queue[i] = -1;

  return pi;
}

int feedbackq_top(struct feedbackq  * fq){
  return fq->queue[0];
}

unsigned int feedbackq_quant(struct feedbackq  * fq){
  return fq->quant;
}
