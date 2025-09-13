#include "userprog/tss.h"
#include <debug.h>
#include <stddef.h>
#include "userprog/gdt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* The Task-State Segment (TSS).
 *
 *  Instances of the TSS, an x86-64 specific structure, are used to
 *  define "tasks", a form of support for multitasking built right
 *  into the processor.  However, for various reasons including
 *  portability, speed, and flexibility, most x86-64 OSes almost
 *  completely ignore the TSS.  We are no exception.
 *
 *  Unfortunately, there is one thing that can only be done using
 *  a TSS: stack switching for interrupts that occur in user mode.
 *  When an interrupt occurs in user mode (ring 3), the processor
 *  consults the rsp0 members of the current TSS to determine the
 *  stack to use for handling the interrupt.  Thus, we must create
 *  a TSS and initialize at least these fields, and this is
 *  precisely what this file does.
 *
 *  When an interrupt is handled by an interrupt or trap gate
 *  (which applies to all interrupts we handle), an x86-64 processor
 *  works like this:
 *
 *    - If the code interrupted by the interrupt is in the same
 *      ring as the interrupt handler, then no stack switch takes
 *      place.  This is the case for interrupts that happen when
 *      we're running in the kernel.  The contents of the TSS are
 *      irrelevant for this case.
 *
 *    - If the interrupted code is in a different ring from the
 *      handler, then the processor switches to the stack
 *      specified in the TSS for the new ring.  This is the case
 *      for interrupts that happen when we're in user space.  It's
 *      important that we switch to a stack that's not already in
 *      use, to avoid corruption.  Because we're running in user
 *      space, we know that the current process's kernel stack is
 *      not in use, so we can always use that.  Thus, when the
 *      scheduler switches threads, it also changes the TSS's
 *      stack pointer to point to the new thread's kernel stack.
 *      (The call is in schedule in thread.c.) */
/* 태스크 상태 세그먼트(TSS).
 *
 *  x86-64 전용 구조체인 TSS의 인스턴스는 "태스크"를 정의하는 데 사용되며,
 *  이는 프로세서에 내장된 멀티태스킹 지원 방식이다. 그러나 이식성, 속도, 유연성과 같은 이유로
 *  대부분의 x86-64 OS는 TSS를 거의 완전히 무시한다. 우리도 예외가 아니다.
 *
 *  하지만 TSS로만 할 수 있는 것이 하나 있는데, 바로 사용자 모드에서 발생하는
 *  인터럽트 시 스택 전환(stack switching)이다.
 *  사용자 모드(ring 3)에서 인터럽트가 발생하면 프로세서는 현재 TSS의 rsp0 값을 참조하여
 *  인터럽트 처리에 사용할 스택을 결정한다. 따라서 우리는 TSS를 만들고 최소한 이 필드를
 *  초기화해야 하며, 이 파일은 바로 그것을 수행한다.
 *
 *  인터럽트 게이트나 트랩 게이트로 인터럽트가 처리될 때(x86-64에서는 우리가 처리하는 모든 인터럽트에 해당),
 *  프로세서는 다음과 같이 동작한다:
 *
 *    - 인터럽트된 코드와 인터럽트 핸들러가 같은 권한 링에 있으면 스택 전환은 발생하지 않는다.
 *      이는 커널 모드에서 실행 중 인터럽트가 발생한 경우이다.
 *      이 경우 TSS의 내용은 무관하다.
 *
 *    - 인터럽트된 코드와 핸들러가 다른 권한 링에 있으면,
 *      프로세서는 새 링에 대해 TSS에 지정된 스택으로 전환한다.
 *      이는 사용자 공간에서 인터럽트가 발생한 경우이다.
 *      이미 사용 중인 스택을 다시 쓰면 안 되므로 반드시 안전한 스택으로 전환해야 한다.
 *      사용자 공간 실행 중에는 현재 프로세스의 커널 스택이 사용 중이 아님을 보장할 수 있으므로,
 *      항상 그것을 사용할 수 있다.
 *      따라서 스케줄러가 스레드를 전환할 때마다 TSS의 스택 포인터를 새 스레드의 커널 스택을 가리키도록 갱신한다.
 *      (thread.c의 schedule 함수에서 호출된다.) */

/* Kernel TSS. */
/* 커널 TSS */
struct task_state *tss;

/* Initializes the kernel TSS. */
/* 커널 TSS를 초기화한다. */
void
tss_init (void) {
	/* Our TSS is never used in a call gate or task gate, so only a
	 * few fields of it are ever referenced, and those are the only
	 * ones we initialize. */
	/* 우리의 TSS는 호출 게이트나 태스크 게이트에서 사용되지 않는다.
	 * 따라서 실제로 참조되는 몇몇 필드만 존재하며, 우리는 그 부분만 초기화한다. */
	tss = palloc_get_page (PAL_ASSERT | PAL_ZERO);
	tss_update (thread_current ());
}

/* Returns the kernel TSS. */
/* 커널 TSS를 반환한다. */
struct task_state *
tss_get (void) {
	ASSERT (tss != NULL);
	return tss;
}

/* Sets the ring 0 stack pointer in the TSS to point to the end
 * of the thread stack. */
/* TSS의 ring 0 스택 포인터를 스레드 스택의 끝을 가리키도록 설정한다. */
void
tss_update (struct thread *next) {
	ASSERT (tss != NULL);
	tss->rsp0 = (uint64_t) next + PGSIZE;
}
