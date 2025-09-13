#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* Number of page faults processed. */
/* 처리된 페이지 폴트 횟수 */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
/* 사용자 프로그램이 일으킬 수 있는 인터럽트의 핸들러를 등록한다.

   실제 유닉스 계열 OS에서는 대부분의 이러한 인터럽트가
   시그널 형태로 사용자 프로세스에 전달되지만(POSIX [SV-386] 3-24, 3-25 참조),
   여기서는 시그널을 구현하지 않으므로 단순히 해당 사용자 프로세스를 종료한다.

   페이지 폴트는 예외다. 현재는 다른 예외와 동일하게 취급되지만,
   가상 메모리를 구현하기 위해서는 나중에 수정이 필요하다.

   각각의 예외에 대한 설명은 [IA32-v3a] 5.15 "Exception and Interrupt Reference"를 참고. */
void
exception_init (void) {
	/* These exceptions can be raised explicitly by a user program,
	   e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
	   we set DPL==3, meaning that user programs are allowed to
	   invoke them via these instructions. */
	/* 이러한 예외들은 사용자 프로그램에 의해 명시적으로 발생할 수 있다.
	   예: INT, INT3, INTO, BOUND 명령어를 통해 발생.
	   따라서 DPL==3으로 설정하여, 사용자 프로그램이 해당 명령어로 호출 가능하도록 한다. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* These exceptions have DPL==0, preventing user processes from
	   invoking them via the INT instruction.  They can still be
	   caused indirectly, e.g. #DE can be caused by dividing by
	   0.  */
	/* 이러한 예외들은 DPL==0으로 설정되어 사용자 프로세스가
	   INT 명령으로 직접 호출할 수는 없다.
	   그러나 간접적으로는 발생할 수 있다. (예: 0으로 나누면 #DE 발생) */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* Most exceptions can be handled with interrupts turned on.
	   We need to disable interrupts for page faults because the
	   fault address is stored in CR2 and needs to be preserved. */
	/* 대부분의 예외는 인터럽트를 켠 상태에서 처리할 수 있다.
	   하지만 페이지 폴트는 CR2 레지스터에 폴트 주소가 저장되므로,
	   이를 보존하기 위해 인터럽트를 꺼야 한다. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
/* 예외 통계를 출력한다. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
/* 사용자 프로세스에 의해 발생한 (가능성이 높은) 예외를 처리하는 핸들러 */
static void
kill (struct intr_frame *f) {
	/* This interrupt is one (probably) caused by a user process.
	   For example, the process might have tried to access unmapped
	   virtual memory (a page fault).  For now, we simply kill the
	   user process.  Later, we'll want to handle page faults in
	   the kernel.  Real Unix-like operating systems pass most
	   exceptions back to the process via signals, but we don't
	   implement them. */
	/* 이 인터럽트는 (아마도) 사용자 프로세스에 의해 발생했다.
	   예: 프로세스가 매핑되지 않은 가상 메모리에 접근 → 페이지 폴트.
	   현재는 단순히 해당 사용자 프로세스를 종료한다.
	   나중에는 커널에서 페이지 폴트를 처리해야 한다.
	   실제 유닉스 계열 OS에서는 대부분의 예외가 시그널로 프로세스에 전달되지만,
	   여기서는 구현하지 않는다. */

	/* The interrupt frame's code segment value tells us where the
	   exception originated. */
	/* 인터럽트 프레임의 코드 세그먼트 값은 예외가 어디서 발생했는지 알려준다. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* User's code segment, so it's a user exception, as we
			   expected.  Kill the user process.  */
			/* 사용자 코드 세그먼트 → 사용자 예외 (예상된 경우).
			   사용자 프로세스를 종료한다. */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
			/* Kernel's code segment, which indicates a kernel bug.
			   Kernel code shouldn't throw exceptions.  (Page faults
			   may cause kernel exceptions--but they shouldn't arrive
			   here.)  Panic the kernel to make the point.  */
			/* 커널 코드 세그먼트 → 커널 버그를 의미.
			   커널 코드는 예외를 던지면 안 된다.
			   (페이지 폴트는 커널 예외를 유발할 수 있지만 여기까지 오면 안 된다.)
			   따라서 커널 패닉 발생. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* Some other code segment?  Shouldn't happen.  Panic the
			   kernel. */
			/* 다른 코드 세그먼트? 발생해서는 안 된다.
			   커널 패닉 발생. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
/* 페이지 폴트 핸들러. 가상 메모리 구현을 위해 이 뼈대 코드를 채워 넣어야 한다.
   프로젝트 2의 일부 해법도 이 코드를 수정할 필요가 있다.

   진입 시, 폴트를 일으킨 주소는 CR2(Control Register 2)에 있고,
   폴트 관련 정보는 exception.h에 정의된 PF_* 매크로 형식으로
   F의 error_code 멤버에 들어 있다.
   아래 예제 코드는 이 정보를 파싱하는 방법을 보여준다.
   자세한 내용은 [IA32-v3a] 5.15 "Exception and Interrupt Reference" 중
   "Interrupt 14--Page Fault Exception (#PF)"를 참고하라. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* True: not-present page, false: writing r/o page. */
	/* true: 존재하지 않는 페이지, false: 읽기 전용 페이지에 쓰기 */
	bool write;        /* True: access was write, false: access was read. */
	/* true: 쓰기 접근, false: 읽기 접근 */
	bool user;         /* True: access by user, false: access by kernel. */
	/* true: 사용자 접근, false: 커널 접근 */
	void *fault_addr;  /* Fault address. */
	/* 폴트가 발생한 주소 */

	/* Obtain faulting address, the virtual address that was
	   accessed to cause the fault.  It may point to code or to
	   data.  It is not necessarily the address of the instruction
	   that caused the fault (that's f->rip). */
	/* 폴트를 일으킨 주소를 얻는다.
	   코드 또는 데이터를 가리킬 수 있다.
	   반드시 폴트를 일으킨 명령어의 주소(f->rip)와 같지는 않다. */
	fault_addr = (void *) rcr2();

	/* Turn interrupts back on (they were only off so that we could
	   be assured of reading CR2 before it changed). */
	/* 인터럽트를 다시 켠다. (CR2를 읽는 동안 값이 변하지 않도록 잠시 껐던 것임) */
	intr_enable ();


	/* Determine cause. */
	/* 원인 판별 */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* For project 3 and later. */
	/* 프로젝트 3 이후를 위한 처리 */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* Count page faults. */
	/* 페이지 폴트 횟수 증가 */
	page_fault_cnt++;

	/* If the fault is true fault, show info and exit. */
	/* 실제 폴트라면 정보 출력 후 종료 */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}
