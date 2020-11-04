#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include "master.h"

static int shmid = -1, msgid = -1;  //semaphore identifier
static struct shared * shmp = NULL;

//Initialize the shared memory pointer
static int shared_initialize()
{
	key_t key = ftok(FTOK_SHM_PATH, FTOK_SHM_KEY);  //get a key for the shared memory
	if(key == -1){
		perror("ftok");
		return -1;
	}

	shmid = shmget(key, 0, 0);
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

	msgid = msgget(key, 0);
	if(msgid == -1){
		perror("msgget");
		return EXIT_FAILURE;
	}

	return 0;
}

static int send_msg(const int msgid, struct msgbuf *m)
{
	m->mtype = getppid();	//send to parent
	m->from = getpid();	//mark who is sending the message
	if(msgsnd(msgid, m, MSG_SIZE, 0) == -1){
		perror("msgsnd");
		return -1;
	}
	return 0;
}

static int get_msg(const int msgid, struct msgbuf *m){
	if(msgrcv(msgid, (void*)m, MSG_SIZE, getpid(), 0) == -1){
		perror("msgrcv");
		return -1;
	}
	return 0;
}

static int decide_action()
{
	//10 % chance to terminate
	static const int term_chance = 10;

	const int term = rand() % 100;
	const int action = (term < term_chance) ? 3 : rand() % 3;

	return action;
}

static void msg_use_quantum(struct msgbuf *msg, const int q){
	msg->msg = READY;
	msg->quant_s = 0;
	msg->quant_ns = q;
}

static void msg_block_io(struct msgbuf *msg){

	static const int r = 3;
	static const int s = 1000;

	msg->msg = IOBLK;
	msg->quant_s	 = rand() % r;
	msg->quant_ns = rand() % s;
}

static void msg_use_quantum_preempt(struct msgbuf *msg, const int q){
	static const float preempt_min = 1.0f;
	static const int preempt_max = 99;

	msg->msg = READY;
	msg->quant_s = 0;
	msg->quant_ns = (int)((float) q / (100.0f / (preempt_min + (rand() % preempt_max))));
}

static void msg_terminate(struct msgbuf *msg){
	msg->msg = TERMINATE;
	msg->quant_s = 0;
	msg->quant_ns = 0;
}

int main(const int argc, char * const argv[]){

	struct msgbuf msg;

	if(shared_initialize() < 0){
		return EXIT_FAILURE;
	}

	//initialize the rand() function
	srand(getpid());

	int terminate_me = 0;
	while(terminate_me == 0){


		if(get_msg(msgid, &msg) == -1){
			break;
		}
		//printf("SLICE=%d\n", msg.quant_ns);
		//fflush(stdout);

		switch(decide_action()){
			case 0:	msg_use_quantum(&msg, msg.quant_ns);				break;
			case 1: msg_use_quantum_preempt(&msg,msg.quant_ns);	break;
			case 2:	msg_block_io(&msg);													break;
			case 3:	default:
				msg_terminate(&msg);
				terminate_me = 1;
				break;
		}

		//send request to enter critical section to master
		if(send_msg(msgid, &msg) == EXIT_FAILURE){	//lock shared oss clock
			break;
		}
	}

	shmdt(shmp);
	return EXIT_SUCCESS;
}
