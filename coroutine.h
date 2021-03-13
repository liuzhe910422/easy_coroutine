#ifndef __easy_COROUTINE_H__
#define __easy_COROUTINE_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/tcp.h>

#include <sys/epoll.h>
#include <sys/poll.h>

#include <errno.h>

#include "queue.h"
#include "tree.h"

#define easy_CO_MAX_EVENTS		(1024*1024)
#define easy_CO_MAX_STACKSIZE	(16*1024) // {http: 16*1024, tcp: 4*1024}

#define BIT(x)	 				(1 << (x))
#define CLEARBIT(x) 			~(1 << (x))

#define CANCEL_FD_WAIT_UINT64	1

typedef void (*proc_coroutine)(void *);


typedef enum {
	easy_COROUTINE_STATUS_WAIT_READ,
	easy_COROUTINE_STATUS_WAIT_WRITE,
	easy_COROUTINE_STATUS_NEW,
	easy_COROUTINE_STATUS_READY,
	easy_COROUTINE_STATUS_EXITED,
	easy_COROUTINE_STATUS_BUSY,
	easy_COROUTINE_STATUS_SLEEPING,
	easy_COROUTINE_STATUS_EXPIRED,
	easy_COROUTINE_STATUS_FDEOF,
	easy_COROUTINE_STATUS_DETACH,
	easy_COROUTINE_STATUS_CANCELLED,
	easy_COROUTINE_STATUS_PENDING_RUNCOMPUTE,
	easy_COROUTINE_STATUS_RUNCOMPUTE,
	easy_COROUTINE_STATUS_WAIT_IO_READ,
	easy_COROUTINE_STATUS_WAIT_IO_WRITE,
	easy_COROUTINE_STATUS_WAIT_MULTI
} easy_coroutine_status;

typedef enum {
	easy_COROUTINE_COMPUTE_BUSY,
	easy_COROUTINE_COMPUTE_FREE
} easy_coroutine_compute_status;

typedef enum {
	easy_COROUTINE_EV_READ,
	easy_COROUTINE_EV_WRITE
} easy_coroutine_event;


LIST_HEAD(_easy_coroutine_link, _easy_coroutine);
TAILQ_HEAD(_easy_coroutine_queue, _easy_coroutine);

RB_HEAD(_easy_coroutine_rbtree_sleep, _easy_coroutine);
RB_HEAD(_easy_coroutine_rbtree_wait, _easy_coroutine);



typedef struct _easy_coroutine_link easy_coroutine_link;
typedef struct _easy_coroutine_queue easy_coroutine_queue;

typedef struct _easy_coroutine_rbtree_sleep easy_coroutine_rbtree_sleep;
typedef struct _easy_coroutine_rbtree_wait easy_coroutine_rbtree_wait;


typedef struct _easy_cpu_ctx {
	void *esp; //
	void *ebp;
	void *eip;
	void *edi;
	void *esi;
	void *ebx;
	void *r1;
	void *r2;
	void *r3;
	void *r4;
	void *r5;
} easy_cpu_ctx;

///
typedef struct _easy_schedule {
	uint64_t birth;
	easy_cpu_ctx ctx;
	void *stack;
	size_t stack_size;
	int spawned_coroutines;
	uint64_t default_timeout;
	struct _easy_coroutine *curr_thread;
	int page_size;

	int poller_fd;
	int eventfd;
	struct epoll_event eventlist[easy_CO_MAX_EVENTS];
	int nevents;

	int num_new_events;
	pthread_mutex_t defer_mutex;

	easy_coroutine_queue ready;
	easy_coroutine_queue defer;

	easy_coroutine_link busy;
	
	easy_coroutine_rbtree_sleep sleeping;
	easy_coroutine_rbtree_wait waiting;

	//private 

} easy_schedule;

typedef struct _easy_coroutine {

	//private
	
	easy_cpu_ctx ctx; //
	proc_coroutine func; 
	void *arg;
	void *data;
	size_t stack_size;
	size_t last_stack_size;
	
	easy_coroutine_status status;
	easy_schedule *sched;

	uint64_t birth;
	uint64_t id;
#if CANCEL_FD_WAIT_UINT64
	int fd;
	unsigned short events;  //POLL_EVENT
#else
	int64_t fd_wait;
#endif
	char funcname[64];
	struct _easy_coroutine *co_join;

	void **co_exit_ptr;
	void *stack;
	void *ebp;
	uint32_t ops;
	uint64_t sleep_usecs;

	RB_ENTRY(_easy_coroutine) sleep_node;
	RB_ENTRY(_easy_coroutine) wait_node;

	LIST_ENTRY(_easy_coroutine) busy_next; //

	TAILQ_ENTRY(_easy_coroutine) ready_next;
	TAILQ_ENTRY(_easy_coroutine) defer_next;
	TAILQ_ENTRY(_easy_coroutine) cond_next;

	TAILQ_ENTRY(_easy_coroutine) io_next;
	TAILQ_ENTRY(_easy_coroutine) compute_next;

	struct {
		void *buf;
		size_t nbytes;
		int fd;
		int ret;
		int err;
	} io;

	struct _easy_coroutine_compute_sched *compute_sched;
	int ready_fds;
	struct pollfd *pfds;
	nfds_t nfds;
} easy_coroutine;


typedef struct _easy_coroutine_compute_sched {
	easy_cpu_ctx ctx;
	easy_coroutine_queue coroutines;

	easy_coroutine *curr_coroutine;

	pthread_mutex_t run_mutex;
	pthread_cond_t run_cond;

	pthread_mutex_t co_mutex;
	LIST_ENTRY(_easy_coroutine_compute_sched) compute_next;
	
	easy_coroutine_compute_status compute_status;
} easy_coroutine_compute_sched;

extern pthread_key_t global_sched_key;
static inline easy_schedule *easy_coroutine_get_sched(void) {
	return pthread_getspecific(global_sched_key);
}

static inline uint64_t easy_coroutine_diff_usecs(uint64_t t1, uint64_t t2) {
	return t2-t1;
}

static inline uint64_t easy_coroutine_usec_now(void) {
	struct timeval t1 = {0, 0};
	gettimeofday(&t1, NULL);

	return t1.tv_sec * 1000000 + t1.tv_usec;
}



int easy_epoller_create(void);


void easy_schedule_cancel_event(easy_coroutine *co);
void easy_schedule_sched_event(easy_coroutine *co, int fd, easy_coroutine_event e, uint64_t timeout);

void easy_schedule_desched_sleepdown(easy_coroutine *co);
void easy_schedule_sched_sleepdown(easy_coroutine *co, uint64_t msecs);

easy_coroutine* easy_schedule_desched_wait(int fd);
void easy_schedule_sched_wait(easy_coroutine *co, int fd, unsigned short events, uint64_t timeout);

int easy_epoller_ev_register_trigger(void);
int easy_epoller_wait(struct timespec t);
int easy_coroutine_resume(easy_coroutine *co);
void easy_coroutine_free(easy_coroutine *co);
int easy_coroutine_create(easy_coroutine **new_co, proc_coroutine func, void *arg);
void easy_coroutine_yield(easy_coroutine *co);

void easy_coroutine_sleep(uint64_t msecs);


int easy_socket(int domain, int type, int protocol);
int easy_accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t easy_recv(int fd, void *buf, size_t len, int flags);
ssize_t easy_send(int fd, const void *buf, size_t len, int flags);
int easy_close(int fd);
int easy_poll(struct pollfd *fds, nfds_t nfds, int timeout);


ssize_t easy_sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t easy_recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
void easy_schedule_run(void);








#endif


