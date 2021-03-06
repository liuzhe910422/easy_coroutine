#include "coroutine.h"

pthread_key_t global_sched_key;
static pthread_once_t sched_key_once = PTHREAD_ONCE_INIT;

//new_ctx --> rdi , 
//cur_ctx --> rsi

int _switch(easy_cpu_ctx *new_ctx, easy_cpu_ctx *cur_ctx);

#ifdef __i386__
__asm__ (
"    .text                                  \n"
"    .p2align 2,,3                          \n"
".globl _switch                             \n"
"_switch:                                   \n"
"__switch:                                  \n"
"movl 8(%esp), %edx      # fs->%edx         \n"
"movl %esp, 0(%edx)      # save esp         \n"
"movl %ebp, 4(%edx)      # save ebp         \n"
"movl (%esp), %eax       # save eip         \n"
"movl %eax, 8(%edx)                         \n"
"movl %ebx, 12(%edx)     # save ebx,esi,edi \n"
"movl %esi, 16(%edx)                        \n"
"movl %edi, 20(%edx)                        \n"
"movl 4(%esp), %edx      # ts->%edx         \n"
"movl 20(%edx), %edi     # restore ebx,esi,edi      \n"
"movl 16(%edx), %esi                                \n"
"movl 12(%edx), %ebx                                \n"
"movl 0(%edx), %esp      # restore esp              \n"
"movl 4(%edx), %ebp      # restore ebp              \n"
"movl 8(%edx), %eax      # restore eip              \n"
"movl %eax, (%esp)                                  \n"
"ret                                                \n"
);
#elif defined(__x86_64__)

__asm__ (
"    .text                                  \n"
"       .p2align 4,,15                                   \n"
".globl _switch                                          \n"
".globl __switch                                         \n"
"_switch:                                                \n"
"__switch:                                               \n"
"       movq %rsp, 0(%rsi)      # save stack_pointer     \n"
"       movq %rbp, 8(%rsi)      # save frame_pointer     \n"
"       movq (%rsp), %rax       # save insn_pointer      \n"
"       movq %rax, 16(%rsi)                              \n"
"       movq %rbx, 24(%rsi)     # save rbx,r12-r15       \n"
"       movq %r12, 32(%rsi)                              \n"
"       movq %r13, 40(%rsi)                              \n"
"       movq %r14, 48(%rsi)                              \n"
"       movq %r15, 56(%rsi)                              \n"
"       movq 56(%rdi), %r15                              \n"
"       movq 48(%rdi), %r14                              \n"
"       movq 40(%rdi), %r13     # restore rbx,r12-r15    \n"
"       movq 32(%rdi), %r12                              \n"
"       movq 24(%rdi), %rbx                              \n"
"       movq 8(%rdi), %rbp      # restore frame_pointer  \n"
"       movq 0(%rdi), %rsp      # restore stack_pointer  \n"
"       movq 16(%rdi), %rax     # restore insn_pointer   \n"
"       movq %rax, (%rsp)                                \n"
"       ret                                              \n"
);
#endif


static void _exec(void *lt) {
#if defined(__lvm__) && defined(__x86_64__)
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r" (lt));
#endif

	easy_coroutine *co = (easy_coroutine*)lt;
	co->func(co->arg); //
	co->status |= (BIT(easy_COROUTINE_STATUS_EXITED) | BIT(easy_COROUTINE_STATUS_FDEOF) 
		| BIT(easy_COROUTINE_STATUS_DETACH));
	
#if 1
	easy_coroutine_yield(co);
#else
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

extern int easy_schedule_create(int stack_size);



void easy_coroutine_free(easy_coroutine *co) {
	if (co == NULL) return ;
	
	co->sched->spawned_coroutines --;
#if 1
	if (co->stack) {
		free(co->stack);
		co->stack = NULL;
	}
#endif
	if (co) {
		free(co);
	}

}

static void easy_coroutine_init(easy_coroutine *co) {

	void **stack = (void **)(co->stack + co->stack_size);

	stack[-3] = NULL;
	stack[-2] = (void *)co;

	co->ctx.esp = (void*)stack - (4 * sizeof(void*));
	co->ctx.ebp = (void*)stack - (3 * sizeof(void*));
	co->ctx.eip = (void*)_exec;
	co->status = BIT(easy_COROUTINE_STATUS_READY);
	
}

void easy_coroutine_yield(easy_coroutine *co) {
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
}

static inline void easy_coroutine_madvise(easy_coroutine *co) {

	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp;
	assert(current_stack <= co->stack_size);

	if (current_stack < co->last_stack_size &&
		co->last_stack_size > co->sched->page_size) {
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1));
		assert(madvise(co->stack, co->stack_size-tmp, MADV_DONTNEED) == 0);
	}
	co->last_stack_size = current_stack;
}

int easy_coroutine_resume(easy_coroutine *co) {
	
	if (co->status & BIT(easy_COROUTINE_STATUS_NEW)) {
		easy_coroutine_init(co);
	}

	easy_schedule *sched = easy_coroutine_get_sched();
	sched->curr_thread = co;
	_switch(&co->ctx, &co->sched->ctx);
	sched->curr_thread = NULL;

	easy_coroutine_madvise(co);
#if 1
	if (co->status & BIT(easy_COROUTINE_STATUS_EXITED)) {
		if (co->status & BIT(easy_COROUTINE_STATUS_DETACH)) {
			printf("easy_coroutine_resume --> \n");
			easy_coroutine_free(co);
		}
		return -1;
	} 
#endif
	return 0;
}


void easy_coroutine_renice(easy_coroutine *co) {
	co->ops ++;
#if 1
	if (co->ops < 5) return ;
#endif
	printf("easy_coroutine_renice\n");
	TAILQ_INSERT_TAIL(&easy_coroutine_get_sched()->ready, co, ready_next);
	printf("easy_coroutine_renice 111\n");
	easy_coroutine_yield(co);
}


void easy_coroutine_sleep(uint64_t msecs) {
	easy_coroutine *co = easy_coroutine_get_sched()->curr_thread;

	if (msecs == 0) {
		TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);
		easy_coroutine_yield(co);
	} else {
		easy_schedule_sched_sleepdown(co, msecs);
	}
}

void easy_coroutine_detach(void) {
	easy_coroutine *co = easy_coroutine_get_sched()->curr_thread;
	co->status |= BIT(easy_COROUTINE_STATUS_DETACH);
}

static void easy_coroutine_sched_key_destructor(void *data) {
	free(data);
}

static void easy_coroutine_sched_key_creator(void) {
	assert(pthread_key_create(&global_sched_key, easy_coroutine_sched_key_destructor) == 0);
	assert(pthread_setspecific(global_sched_key, NULL) == 0);
	
	return ;
}


// coroutine --> 
// create 
//
int easy_coroutine_create(easy_coroutine **new_co, proc_coroutine func, void *arg) {

	assert(pthread_once(&sched_key_once, easy_coroutine_sched_key_creator) == 0);
	easy_schedule *sched = easy_coroutine_get_sched();

	if (sched == NULL) {
		easy_schedule_create(0);
		
		sched = easy_coroutine_get_sched();
		if (sched == NULL) {
			printf("Failed to create scheduler\n");
			return -1;
		}
	}

	easy_coroutine *co = calloc(1, sizeof(easy_coroutine));
	if (co == NULL) {
		printf("Failed to allocate memory for new coroutine\n");
		return -2;
	}

	int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size);
	if (ret) {
		printf("Failed to allocate stack for new coroutine\n");
		free(co);
		return -3;
	}

	co->sched = sched;
	co->stack_size = sched->stack_size;
	co->status = BIT(easy_COROUTINE_STATUS_NEW); //
	co->id = sched->spawned_coroutines ++;
	co->func = func;
#if CANCEL_FD_WAIT_UINT64
	co->fd = -1;
	co->events = 0;
#else
	co->fd_wait = -1;
#endif
	co->arg = arg;
	co->birth = easy_coroutine_usec_now();
	*new_co = co;

	TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);

	return 0;
}




