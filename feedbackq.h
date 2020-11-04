#include "master.h"

#define FEEDBACK_LEVELS 4

struct feedbackq {
	int queue[MAX_USERS];	/* value is ctrl_block->id */
	int count;
	unsigned int quant;
};

void feedbackq_init(struct feedbackq  fq[FEEDBACK_LEVELS]);
int feedbackq_ready(struct feedbackq  fq[FEEDBACK_LEVELS], const struct process * procs);

int feedbackq_enq(struct feedbackq  * fq, const int pi);
int feedbackq_deq(struct feedbackq  * fq, const int pos);
int feedbackq_top(struct feedbackq  * fq);

unsigned int feedbackq_quant(struct feedbackq  * fq);
