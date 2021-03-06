#include "coroutine.h"



#define FD_KEY(f,e) (((int64_t)(f) << (sizeof(int32_t) * 8)) | e)
#define FD_EVENT(f) ((int32_t)(f))
#define FD_ONLY(f) ((f) >> ((sizeof(int32_t) * 8)))


static inline int easy_coroutine_sleep_cmp(easy_coroutine *co1, easy_coroutine *co2) {
	if (co1->sleep_usecs < co2->sleep_usecs) {
		return -1;
	}
	if (co1->sleep_usecs == co2->sleep_usecs) {
		return 0;
	}
	return 1;
}

static inline int easy_coroutine_wait_cmp(easy_coroutine *co1, easy_coroutine *co2) {
#if CANCEL_FD_WAIT_UINT64
	if (co1->fd < co2->fd) return -1;
	else if (co1->fd == co2->fd) return 0;
	else return 1;
#else
	if (co1->fd_wait < co2->fd_wait) {
		return -1;
	}
	if (co1->fd_wait == co2->fd_wait) {
		return 0;
	}
#endif
	return 1;
}


RB_GENERATE(_easy_coroutine_rbtree_sleep, _easy_coroutine, sleep_node, easy_coroutine_sleep_cmp);
RB_GENERATE(_easy_coroutine_rbtree_wait, _easy_coroutine, wait_node, easy_coroutine_wait_cmp);



void easy_schedule_sched_sleepdown(easy_coroutine *co, uint64_t msecs) {
	uint64_t usecs = msecs * 1000u;

	easy_coroutine *co_tmp = RB_FIND(_easy_coroutine_rbtree_sleep, &co->sched->sleeping, co);
	if (co_tmp != NULL) {
		RB_REMOVE(_easy_coroutine_rbtree_sleep, &co->sched->sleeping, co_tmp);
	}

	co->sleep_usecs = easy_coroutine_diff_usecs(co->sched->birth, easy_coroutine_usec_now()) + usecs;

	while (msecs) {
		co_tmp = RB_INSERT(_easy_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		if (co_tmp) {
			printf("1111 sleep_usecs %"PRIu64"\n", co->sleep_usecs);
			co->sleep_usecs ++;
			continue;
		}
		co->status |= BIT(easy_COROUTINE_STATUS_SLEEPING);
		break;
	}

	//yield
}

void easy_schedule_desched_sleepdown(easy_coroutine *co) {
	if (co->status & BIT(easy_COROUTINE_STATUS_SLEEPING)) {
		RB_REMOVE(_easy_coroutine_rbtree_sleep, &co->sched->sleeping, co);

		co->status &= CLEARBIT(easy_COROUTINE_STATUS_SLEEPING);
		co->status |= BIT(easy_COROUTINE_STATUS_READY);
		co->status &= CLEARBIT(easy_COROUTINE_STATUS_EXPIRED);
	}
}

easy_coroutine *easy_schedule_search_wait(int fd) {
	easy_coroutine find_it = {0};
	find_it.fd = fd;

	easy_schedule *sched = easy_coroutine_get_sched();
	
	easy_coroutine *co = RB_FIND(_easy_coroutine_rbtree_wait, &sched->waiting, &find_it);
	co->status = 0;

	return co;
}

easy_coroutine* easy_schedule_desched_wait(int fd) {
	
	easy_coroutine find_it = {0};
	find_it.fd = fd;

	easy_schedule *sched = easy_coroutine_get_sched();
	
	easy_coroutine *co = RB_FIND(_easy_coroutine_rbtree_wait, &sched->waiting, &find_it);
	if (co != NULL) {
		RB_REMOVE(_easy_coroutine_rbtree_wait, &co->sched->waiting, co);
	}
	co->status = 0;
	easy_schedule_desched_sleepdown(co);
	
	return co;
}

void easy_schedule_sched_wait(easy_coroutine *co, int fd, unsigned short events, uint64_t timeout) {
	
	if (co->status & BIT(easy_COROUTINE_STATUS_WAIT_READ) ||
		co->status & BIT(easy_COROUTINE_STATUS_WAIT_WRITE)) {
		printf("Unexpected event. lt id %"PRIu64" fd %"PRId32" already in %"PRId32" state\n",
            co->id, co->fd, co->status);
		assert(0);
	}

	if (events & POLLIN) {
		co->status |= easy_COROUTINE_STATUS_WAIT_READ;
	} else if (events & POLLOUT) {
		co->status |= easy_COROUTINE_STATUS_WAIT_WRITE;
	} else {
		printf("events : %d\n", events);
		assert(0);
	}

	co->fd = fd;
	co->events = events;
	easy_coroutine *co_tmp = RB_INSERT(_easy_coroutine_rbtree_wait, &co->sched->waiting, co);

	assert(co_tmp == NULL);

	//printf("timeout --> %"PRIu64"\n", timeout);
	if (timeout == 1) return ; //Error

	easy_schedule_sched_sleepdown(co, timeout);
	
}

void easy_schedule_cancel_wait(easy_coroutine *co) {
	RB_REMOVE(_easy_coroutine_rbtree_wait, &co->sched->waiting, co);
}

void easy_schedule_free(easy_schedule *sched) {
	if (sched->poller_fd > 0) {
		close(sched->poller_fd);
	}
	if (sched->eventfd > 0) {
		close(sched->eventfd);
	}
	
	free(sched);

	assert(pthread_setspecific(global_sched_key, NULL) == 0);
}

int easy_schedule_create(int stack_size) {

	int sched_stack_size = stack_size ? stack_size : easy_CO_MAX_STACKSIZE;

	easy_schedule *sched = (easy_schedule*)calloc(1, sizeof(easy_schedule));
	if (sched == NULL) {
		printf("Failed to initialize scheduler\n");
		return -1;
	}

	assert(pthread_setspecific(global_sched_key, sched) == 0);

	sched->poller_fd = easy_epoller_create();
	if (sched->poller_fd == -1) {
		printf("Failed to initialize epoller\n");
		easy_schedule_free(sched);
		return -2;
	}

	easy_epoller_ev_register_trigger();

	sched->stack_size = sched_stack_size;
	sched->page_size = getpagesize();

	sched->spawned_coroutines = 0;
	sched->default_timeout = 3000000u;

	RB_INIT(&sched->sleeping);
	RB_INIT(&sched->waiting);

	sched->birth = easy_coroutine_usec_now();

	TAILQ_INIT(&sched->ready);
	//TAILQ_INIT(&sched->defer);
	LIST_INIT(&sched->busy);

	bzero(&sched->ctx, sizeof(easy_cpu_ctx));
}


static easy_coroutine *easy_schedule_expired(easy_schedule *sched) {
	
	uint64_t t_diff_usecs = easy_coroutine_diff_usecs(sched->birth, easy_coroutine_usec_now());
	easy_coroutine *co = RB_MIN(_easy_coroutine_rbtree_sleep, &sched->sleeping);
	if (co == NULL) return NULL;
	
	if (co->sleep_usecs <= t_diff_usecs) {
		RB_REMOVE(_easy_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		return co;
	}
	return NULL;
}

static inline int easy_schedule_isdone(easy_schedule *sched) {
	return (RB_EMPTY(&sched->waiting) && 
		LIST_EMPTY(&sched->busy) &&
		RB_EMPTY(&sched->sleeping) &&
		TAILQ_EMPTY(&sched->ready));
}

static uint64_t easy_schedule_min_timeout(easy_schedule *sched) {
	uint64_t t_diff_usecs = easy_coroutine_diff_usecs(sched->birth, easy_coroutine_usec_now());
	uint64_t min = sched->default_timeout;

	easy_coroutine *co = RB_MIN(_easy_coroutine_rbtree_sleep, &sched->sleeping);
	if (!co) return min;

	min = co->sleep_usecs;
	if (min > t_diff_usecs) {
		return min - t_diff_usecs;
	}

	return 0;
} 

static int easy_schedule_epoll(easy_schedule *sched) {

	sched->num_new_events = 0;

	struct timespec t = {0, 0};
	uint64_t usecs = easy_schedule_min_timeout(sched);
	if (usecs && TAILQ_EMPTY(&sched->ready)) {
		t.tv_sec = usecs / 1000000u;
		if (t.tv_sec != 0) {
			t.tv_nsec = (usecs % 1000u) * 1000u;
		} else {
			t.tv_nsec = usecs * 1000u;
		}
	} else {
		return 0;
	}

	int nready = 0;
	while (1) {
		nready = easy_epoller_wait(t);
		if (nready == -1) {
			if (errno == EINTR) continue;
			else assert(0);
		}
		break;
	}

	sched->nevents = 0;
	sched->num_new_events = nready;

	return 0;
}

void easy_schedule_run(void) {

	easy_schedule *sched = easy_coroutine_get_sched();
	if (sched == NULL) return ;

	while (!easy_schedule_isdone(sched)) {
		
		// 1. expired --> sleep rbtree
		easy_coroutine *expired = NULL; //
		while ((expired = easy_schedule_expired(sched)) != NULL) {
			easy_coroutine_resume(expired);
		}
		// 2. ready queue //
		easy_coroutine *last_co_ready = TAILQ_LAST(&sched->ready, _easy_coroutine_queue);
		while (!TAILQ_EMPTY(&sched->ready)) {
			easy_coroutine *co = TAILQ_FIRST(&sched->ready);
			TAILQ_REMOVE(&co->sched->ready, co, ready_next);

			if (co->status & BIT(easy_COROUTINE_STATUS_FDEOF)) {
				easy_coroutine_free(co);
				break;
			}

			easy_coroutine_resume(co);
			if (co == last_co_ready) break;
		}

		// 3. wait rbtree
		easy_schedule_epoll(sched);
		while (sched->num_new_events) {
			int idx = --sched->num_new_events;
			struct epoll_event *ev = sched->eventlist+idx;
			
			int fd = ev->data.fd;
			int is_eof = ev->events & EPOLLHUP;
			if (is_eof) errno = ECONNRESET;

			easy_coroutine *co = easy_schedule_search_wait(fd);
			if (co != NULL) {
				if (is_eof) {
					co->status |= BIT(easy_COROUTINE_STATUS_FDEOF);
				}
				easy_coroutine_resume(co);
			}

			is_eof = 0;
		}
	}

	easy_schedule_free(sched);
	
	return ;
}

