#ifndef MASTER_H
#define MASTER_H

#include <unistd.h>

#define MAX_USERS 18

struct vclock {
  unsigned int sec;
	unsigned int ns;
};

//helper functions for virtual clock
#define VCLOCK_COPY(x,y) x.sec = y.sec; x.ns = y.ns;
#define VCLOCK_AVE(x,count) x.sec /= count; x.ns /= count;

enum status_type { READY=1, IOBLK, TERMINATE, DECISON_COUNT};
enum vclock_type { TOTAL_CPU=0, TOTAL_SYSTEM, BURST_TIME, FORK_TIME, BLOCKED_TIME, READY_TIME, VCLOCK_COUNT};

// entry in the process control table
struct process {
	int	pid;
	int id;
	enum status_type state;

	struct vclock	vclk[VCLOCK_COUNT];
};

//The variables shared between master and palin processes
struct shared {
	struct vclock vclk;
	struct process procs[MAX_USERS];
};

// quantum 10 ms ( in ns )
#define QUANTUM_NS 10000000

//shared memory constants
#define FTOK_Q_PATH "/tmp"
#define FTOK_SHM_PATH "/tmp"
#define FTOK_Q_KEY 6776
#define FTOK_SHM_KEY 7667

struct msgbuf {
	long mtype;
	pid_t from;

	int msg;
	int quant_s;
	int quant_ns;
};

#define MSG_SIZE sizeof(pid_t) + (3*sizeof(int))

#endif
