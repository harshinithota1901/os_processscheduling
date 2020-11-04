#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>

#include "master.h"
#include "blockedq.h"
#include "feedbackq.h"

//maximum time to run
#define MAX_RUNTIME 3
//maximum children to create
#define MAX_CHILDREN 100

//Our program options
static unsigned int arg_c = 5;
static char * arg_l = NULL;
static unsigned int arg_t = MAX_RUNTIME;

static pid_t childpids[MAX_CHILDREN];  //array for user pids
static unsigned int C = 0;
static int shmid = -1, msgid = -1;    //shared memory and msg queue ids
static unsigned int interrupted = 0;

static FILE * output = NULL;
static struct shared * shmp = NULL; //pointer to shared memory

static struct feedbackq fq[FEEDBACK_LEVELS];  //multi-level feedback queue
static struct blockedq bq;        //blocked queue

static unsigned int pcb_bitmap = 0;

enum stat_times {IDLE_TIME, TURN_TIME, WAIT_TIME, SLEEP_TIME};
static struct vclock vclk_stat[4];

//Called when we receive a signal
static void sign_handler(const int sig)
{
  interrupted = 1;
	fprintf(output, "[%u:%u] Signal %i received\n", shmp->vclk.sec, shmp->vclk.ns, sig);
}

//return 0 or 1 bot bit n from pcb bitmap
static int bit_status(const int n){
  return ((pcb_bitmap & (1 << n)) >> n);
}

//find first available pcb
static int unused_pcb(){
	int i;
  for(i=0; i < MAX_USERS; i++){
  	if(bit_status(i) == 0){
			pcb_bitmap ^= (1 << i);	//raise the bit
      return i;
    }
  }
  return -1;
}

//mark a pcb as unused
void pcb_release(struct process * procs, const unsigned int i){

  pcb_bitmap ^= (1 << i); //switch bit
  bzero(&shmp->procs[i], sizeof(struct process));
}


struct process * pcb_get(){
	const int i = unused_pcb();
	if(i == -1){
		return NULL;
	}

  shmp->procs[i].id	= C;
  shmp->procs[i].state = READY;
	return &shmp->procs[i];
}

//Create a child process
static pid_t master_fork(const char *prog)
{

  struct process *pcb = pcb_get();
  if(pcb == NULL){
    fprintf(output, "Warning: No pcb available\n");
    return 0; //no free processes
  }

	const pid_t pid = fork();  //create process
	if(pid < 0){
		perror("fork");
		return -1;

	}else if(pid == 0){
    //run the specified program
		execl(prog, prog, NULL);
		perror("execl");
		exit(1);

	}else{
    pcb->pid = pid;
    VCLOCK_COPY(pcb->vclk[READY_TIME], shmp->vclk);
    VCLOCK_COPY(pcb->vclk[FORK_TIME],  shmp->vclk);

    const int pcb_index = pcb - shmp->procs; //process index
    const int rv = feedbackq_enq(&fq[0], pcb_index);
    if(rv < 0){
      fprintf(stderr, "[%i: %i] Error: Queueing process with PID %d failed\n", shmp->vclk.sec, shmp->vclk.ns, pcb->pid);
    }else{
      fprintf(output,"[%u:%u] Master: Generating process with PID %u and putting it in queue 0\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id);
    }

    //save child pid
		childpids[C++] = pid;
	}
	return pid;
}

//Wait for all processes to exit
static void master_waitall()
{
  int i;
  for(i=0; i < C; ++i){ //for each process
    if(childpids[i] == 0){  //if pid is zero, process doesn't exist
      continue;
    }

    int status;
    if(waitpid(childpids[i], &status, WNOHANG) > 0){

      if (WIFEXITED(status)) {  //if process exited

        fprintf(output,"Master: Child %u terminated with %i at %u:%u\n",
          childpids[i], WEXITSTATUS(status), shmp->vclk.sec, shmp->vclk.ns);

      }else if(WIFSIGNALED(status)){  //if process was signalled
        fprintf(output,"Master: Child %u killed with signal %d at system time at %u:%u\n",
          childpids[i], WTERMSIG(status), shmp->vclk.sec, shmp->vclk.ns);
      }
      childpids[i] = 0;
    }
  }
}

static void output_result(){

  VCLOCK_AVE(vclk_stat[TURN_TIME],  C);
  VCLOCK_AVE(vclk_stat[WAIT_TIME],  C);
  VCLOCK_AVE(vclk_stat[SLEEP_TIME], C);

  fprintf(output,"Quantum: %d\n", QUANTUM_NS);
  fprintf(output,"Runtime: %u:%u\n", shmp->vclk.sec, shmp->vclk.ns);
  fprintf(output,"Average Turnaround Time: %u:%u\n",  vclk_stat[TURN_TIME].sec,   vclk_stat[TURN_TIME].ns);
  fprintf(output,"Average Wait Time. : %u:%u\n",      vclk_stat[WAIT_TIME].sec,   vclk_stat[WAIT_TIME].ns);
  fprintf(output,"Average Blocked Time: %u:%u\n",     vclk_stat[SLEEP_TIME].sec,  vclk_stat[SLEEP_TIME].ns);
  fprintf(output,"Idle Time: %u:%u\n",        vclk_stat[IDLE_TIME].sec,   vclk_stat[IDLE_TIME].ns);

  //TODO: ave cpu util
}

//Called at end to cleanup all resources and exit
static void master_exit(const int ret)
{
  //tell all users to terminate
  int i;
  for(i=0; i < C; i++){
    if(childpids[i] <= 0){
      continue;
    }
  	kill(childpids[i], SIGTERM);
  }
  master_waitall();

  output_result();

  if(shmp){
    shmdt(shmp);
    shmctl(shmid, IPC_RMID, NULL);
  }

  if(msgid > 0){
    msgctl(msgid, IPC_RMID, NULL);
  }

  fclose(output);
	exit(ret);
}

static void vclock_increment(struct vclock * x, struct vclock * inc){
  x->sec += inc->sec;
  x->ns += inc->ns;
	if(x->ns > 1000000000){
		x->sec++;
		x->ns = 0;
	}
}

static void vclock_substract(struct vclock * x, struct vclock * y, struct vclock * z){
  z->sec = x->sec - y->sec;

  if(y->ns > x->ns){
    z->ns = y->ns - x->ns;
    z->sec--;
  }else{
    z->ns = x->ns - y->ns;
  }
}

//Move time forward
static int update_timer(struct shared *shmp, struct vclock * fork_vclock)
{
  static const int maxTimeBetweenNewProcsSecs = 1;
  static const int maxTimeBetweenNewProcsNS = 500000;

  struct vclock inc = {0, 100};

  vclock_increment(&shmp->vclk, &inc);
  usleep(10);
  //fprintf(output, "[%u:%u] Master: Incremented system time with 100 ns\n", shmp->vclk.sec, shmp->vclk.ns);

  //if its time to fork
  if(  (shmp->vclk.sec  > fork_vclock->sec) ||
      ((shmp->vclk.sec == fork_vclock->sec) && (shmp->vclk.ns > fork_vclock->ns))){

    *fork_vclock = shmp->vclk;
    inc.sec = (rand() % maxTimeBetweenNewProcsSecs);
    inc.ns  = (rand() % maxTimeBetweenNewProcsNS);
    vclock_increment(fork_vclock, &inc);

    return 1;
  }

  return 0;
}

//Process program options
static int update_options(const int argc, char * const argv[])
{

  int opt;
	while((opt=getopt(argc, argv, "hc:l:t:")) != -1){
		switch(opt){
			case 'h':
				fprintf(output,"Usage: master [-h]\n");
        fprintf(output,"Usage: master [-n x] [-s x] [-t time] infile\n");
				fprintf(output," -h Describe program options\n");
				fprintf(output," -c x Total of child processes (Default is 5)\n");
        fprintf(output," -l filename Log filename (Default is log.txt)\n");
        fprintf(output," -t x Maximum runtime (Default is 20)\n");
				return 1;

      case 'c':
        arg_c	= atoi(optarg); //convert value -n from string to int
        break;

      case 't':
        arg_t	= atoi(optarg);
        break;

      case 'l':
				arg_l = strdup(optarg);
				break;

			default:
				fprintf(output, "Error: Invalid option '%c'\n", opt);
				return -1;
		}
	}

	if(arg_l == NULL){
		arg_l = strdup("log.txt");
	}
  return 0;
}

//Initialize the shared memory
static int shared_initialize()
{
  key_t key = ftok(FTOK_SHM_PATH, FTOK_SHM_KEY);  //get a key for the shared memory
	if(key == -1){
		perror("ftok");
		return -1;
	}

  const long shared_size = sizeof(struct shared);

	shmid = shmget(key, shared_size, IPC_CREAT | IPC_EXCL | S_IRWXU);
	if(shmid == -1){
		perror("shmget");
		return -1;
	}

  shmp = (struct shared*) shmat(shmid, NULL, 0); //attach it
  if(shmp == NULL){
		perror("shmat");
		return -1;
	}

	key = ftok(FTOK_Q_PATH, FTOK_Q_KEY);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	msgid = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
	if(msgid == -1){
		perror("msgget");
		return -1;
	}
  return 0;
}

//Initialize the master process
static int master_initialize()
{

  if(shared_initialize() < 0){
    return -1;
  }

  //zero pids
  bzero(childpids, sizeof(pid_t)*MAX_CHILDREN);

  //zero the shared clock
  shmp->vclk.sec	= 0;
	shmp->vclk.ns	= 0;

  //zero the processes
  bzero(shmp, sizeof(struct shared));

  //initialize queues
  blockedq_init(&bq);
  feedbackq_init(fq);

  return 0;
}

//Send a message to user process. Buffer must be filled!
static int send_msg(struct msgbuf *m)
{
	m->from = getpid();	//mark who is sending the message
	if(msgsnd(msgid, m, MSG_SIZE, 0) == -1){
		perror("msgsnd");
		return -1;
	}
  return 0;
}

static int get_msg(struct msgbuf *m)
{
	if(msgrcv(msgid, (void*)m, MSG_SIZE, getpid(), 0) == -1){
		perror("msgrcv");
		return -1;
	}
	return 0;
}

static int update_pcb_state(struct process * pcb, const int q){

  switch(pcb->state){
    case READY:
      fprintf(output,"[%u:%u] Master: Receiving that process with PID %u ran for %u nanoseconds\n",
        shmp->vclk.sec, shmp->vclk.ns, pcb->id, pcb->vclk[BURST_TIME].ns);

      vclock_increment(&pcb->vclk[TOTAL_CPU], &pcb->vclk[BURST_TIME]);
      //update shared clock with burst time
      vclock_increment(&shmp->vclk,          &pcb->vclk[BURST_TIME]);
      break;

    case IOBLK:
      fprintf(output,"[%u:%u] Master: Process with PID %u has blocked on IO to %u:%u\n",
          shmp->vclk.sec, shmp->vclk.ns, pcb->id, pcb->vclk[BURST_TIME].sec, pcb->vclk[BURST_TIME].ns);
      /* add burst and current timer to make blocked timestamp */
  		vclock_increment(&pcb->vclk[BLOCKED_TIME], &pcb->vclk[BURST_TIME]);
  		vclock_increment(&pcb->vclk[BLOCKED_TIME], &shmp->vclk);
      break;

    case TERMINATE:

      vclock_increment(&pcb->vclk[TOTAL_CPU], &pcb->vclk[BURST_TIME]);
      vclock_substract(&shmp->vclk, &pcb->vclk[FORK_TIME], &pcb->vclk[TOTAL_SYSTEM]);
      fprintf(output,"[%u:%u] Master: Process with PID %u terminated\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id);
      break;

    default:
      fprintf(output,"[%u:%u] Master: Process with PID %d has invalid state\n", shmp->vclk.sec, shmp->vclk.ns, pcb->pid);
      return -1;
      break;
  }
  return 0;
}

static void update_queue(struct process * pcb, int q){

  const int pcb_index = feedbackq_deq(&fq[q], 0);
  struct vclock res;

  switch(pcb->state){
    case TERMINATE:

      //vclk_stat[TURN_TIME] time = system time / num processes
      vclock_increment(&vclk_stat[TURN_TIME], &pcb->vclk[TOTAL_SYSTEM]);

      /* wait time = total_system time - total cpu time */
      vclock_substract(&pcb->vclk[TOTAL_SYSTEM], &pcb->vclk[TOTAL_CPU], &res);
      vclock_increment(&vclk_stat[WAIT_TIME], &res);

      fprintf(output,"[%u:%u] Master: Process with PID %u terminated, removed from queue %d\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id, q);
      pcb_release(shmp->procs, pcb_index);
      break;

    case IOBLK:
      fprintf(output,"[%u:%u] Master: Putting process with PID %u into blocked queue\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id);
      blockedq_enq(&bq, pcb_index);
      break;

    default:
      //check if process was preepted
      if(pcb->vclk[BURST_TIME].ns == feedbackq_quant(&fq[q])){
        //if we can move process to next level queue
        if(q < (FEEDBACK_LEVELS - 1)){
          q++;
        }
      }else{
        fprintf(output,"[%u:%u] Master: not using its entire time quantum\n", shmp->vclk.sec, shmp->vclk.ns);
      }
      VCLOCK_COPY(pcb->vclk[READY_TIME], shmp->vclk);

      fprintf(output,"[%u:%u] Master: Process with PID %u moved to queue %d\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id, q);
      feedbackq_enq(&fq[q], pcb_index);
      break;
  }
}

static int dispatch_fq(const int q){

  const int pcb_index = feedbackq_top(&fq[q]);
  struct process * pcb = &shmp->procs[pcb_index];

  fprintf(output,"[%u:%u] Master: Dispatching process with PID %u from queue %i\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id, q);

  struct msgbuf mb;
  mb.mtype = pcb->pid;
  mb.quant_ns = feedbackq_quant(&fq[q]);

  //tell process he can run and get his decision
  if( (send_msg(&mb) == -1) || (get_msg(&mb) == -1) ){
    return -1;
  }

  //set burst time - for execution or io
  pcb->vclk[BURST_TIME].sec = mb.quant_s;
  pcb->vclk[BURST_TIME].ns = mb.quant_ns;

  pcb->state = mb.msg;

  update_pcb_state(pcb, q);
  update_queue(pcb, q);

  //calculate dispatch time
  struct vclock temp;
  temp.sec = 0;
  temp.ns = rand() % 100;
  fprintf(output,"[%u:%u] Master: total time this dispatching was %d nanoseconds\n", shmp->vclk.sec, shmp->vclk.ns, temp.ns);
  vclock_increment(&shmp->vclk, &temp);

  return 0;
}

static int dispatch_bq(){

  const int pcb_index = blockedq_ready(&bq, &shmp->vclk, shmp->procs);
  if(pcb_index == -1){
    return -1;
  }
  struct process * pcb = &shmp->procs[pcb_index];

  //burst time of pcb has time process was blocked
  vclock_increment(&vclk_stat[SLEEP_TIME], &pcb->vclk[BURST_TIME]);

  //change process pcb to ready, and reset timers
  pcb->state = READY;
  pcb->vclk[BLOCKED_TIME].sec = pcb->vclk[BLOCKED_TIME].ns = 0;
  pcb->vclk[BURST_TIME].sec = pcb->vclk[BURST_TIME].ns = 0;
  pcb->vclk[READY_TIME] = shmp->vclk;

  //add to first queue after unblock
  const int rv = feedbackq_enq(&fq[0], pcb_index);
  if(rv < 0){
    fprintf(stderr, "[%i: %i] Error: Queueing process with PID %d failed\n", shmp->vclk.sec, shmp->vclk.ns, pcb->pid);
  }else{
    fprintf(output,"[%u:%u] Master: Unblocked process with PID %d to queue 0\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id);
  }

  return rv;
}

int main(const int argc, char * const argv[])
{

  if(update_options(argc, argv) < 0){
    master_exit(1);
  }

  output = fopen(arg_l, "w");
  if(output == NULL){
    perror("fopen");
    return 1;
  }

  //signal(SIGCHLD, master_waitall);
  signal(SIGTERM, sign_handler);
  signal(SIGALRM, sign_handler);
  //alarm(arg_t);

  if(master_initialize() < 0){
    master_exit(1);
  }


  int idling = 0;
  struct vclock idle_vclock = {0,0}, temp;
  struct vclock fork_vclock = {0,0};


  //run until interrupted
  while(!interrupted){

    if(update_timer(shmp, &fork_vclock) > 0){
      if(C < MAX_CHILDREN){
        const pid_t pid = master_fork("./user");
        fprintf(output,"[%u:%u] Master: Creating new child pid %i\n", shmp->vclk.sec, shmp->vclk.ns, pid);
      }else{  //we have generated all of the children
        interrupted = 1;  //stop master loop
      }
    }

    dispatch_bq();

    //get a queue with ready process
    const int q_index = feedbackq_ready(fq, shmp->procs);
    if(q_index >= 0){

      //if we are in idle mode
      if(idling){

        //how much time we were idle
        vclock_substract(&shmp->vclk, &idle_vclock, &temp);
        fprintf(output,"[%u:%u] Master: End of idle mode of %u:%u.\n",
          shmp->vclk.sec, shmp->vclk.ns, temp.sec, temp.ns);
        vclock_increment(&vclk_stat[IDLE_TIME], &temp);

        idle_vclock.sec  = 0;
        idle_vclock.ns = 0;
        idling = 0;
      }

      if(dispatch_fq(q_index) < 0){
        fprintf(stderr, "Error: Dispatch failed.\n");
        break;
      }

    //no ready process
    }else{

      //set CPU mode to idling
      if(idling == 0){
        fprintf(output,"[%u:%u] Master: No process ready to dispatch.\n", shmp->vclk.sec, shmp->vclk.ns);
        VCLOCK_COPY(idle_vclock, shmp->vclk);
        idling = 1;
      }

      //if we have processes blocked on IO
      if(blockedq_size(&bq) > 0){

        struct process * pcb = &shmp->procs[blockedq_top(&bq)];
        fprintf(output,"[%u:%u] Master: No process ready. Setting time to first unblock at %u:%u.\n",
          shmp->vclk.sec, shmp->vclk.ns, pcb->vclk[BLOCKED_TIME].sec, pcb->vclk[BLOCKED_TIME].ns);

        VCLOCK_COPY(shmp->vclk, pcb->vclk[BLOCKED_TIME]);
      }else{
        //jump to next fork time
        fprintf(output,"[%u:%u] Master: No process ready. Setting time to next fork at %u:%u.\n",
          shmp->vclk.sec, shmp->vclk.ns, fork_vclock.sec, fork_vclock.ns);
        shmp->vclk = fork_vclock;
      }
    }
	}

  fprintf(output,"[%u:%u] Master exit\n", shmp->vclk.sec, shmp->vclk.ns);
	master_exit(0);

	return 0;
}
