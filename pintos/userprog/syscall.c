#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "filesys/filesys.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */
/* 시스템 콜.
 *
 * 과거에는 시스템 콜이 인터럽트 핸들러에 의해 처리되었다
 * (예: 리눅스의 int 0x80). 하지만 x86-64에서는 제조사가
 * 시스템 콜 요청을 위한 더 효율적인 경로인 `syscall` 명령을 제공한다.
 *
 * `syscall` 명령은 모델별 레지스터(MSR)의 값을 읽어 동작한다.
 * 자세한 내용은 매뉴얼을 참고하라. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
/* 세그먼트 셀렉터 MSR */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
/* 롱 모드 SYSCALL 진입 지점 주소(MSR) */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
/* EFLAGS 마스크(MSR) */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	/* syscall_entry가 유저 스택을 커널 모드 스택으로 교체하기 전까지는
	 * 인터럽트 서비스 루틴이 어떤 인터럽트도 처리하면 안 된다.
	 * 따라서 EFLAGS의 해당 비트들을 마스킹한다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
/* 메인 시스템 콜 인터페이스 */
void
syscall_handler (struct intr_frame *f UNUSED) {
	int sys_number = f->R.rax;							// f->R.rax 는 시스템 콜 번호가 들어있는 레지스터 값

	switch (sys_number)
	{
	case SYS_HALT:
		halt();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_FORK:
		//fork(f->R.rdi);
		break;
	case SYS_EXEC:
		break;
	case SYS_WAIT:
		f->R.rax = process_wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);			// f는 인터럽트가 발생했을 때 CPU 레지스터 상태를 저장한 구조체, R은 범용 레지스터 집합
														// rdi = 파일 이름 문자열, rsi = 초기 크기 값
		break;	
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		break;
	case SYS_FILESIZE:
		break;
	case SYS_READ:
		break;
	case SYS_WRITE:
		break;
	case SYS_SEEK:
		break;
	case SYS_TELL:
		break;
	case SYS_CLOSE:
		break;
	default:
		exit(-1);
	}

	printf ("system call!\n");
	thread_exit ();
}

bool create(const char *file, unsigned initial_size)
{
	check_address(file);							// 포인터 안정성 체크

	if (filesys_create(file, initial_size))			// 커널 내부에서 파일을 생성하는 함수
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool remove(const char *file)
{
	check_address(file);

	if (filesys_remove(file))
	{
		return true;
	}
	else
	{
		return false;
	}
}

void halt(void)
{
	power_off();
}

void exit(int status)
{
	struct thread *t = thread_current();
	printf("%s: exit(%d)\n", t->name, status);
	thread_exit();
}

// 포인터가 가르키는 주소가 사용자 영역인지 확인
void check_address(void *addr)
{
	struct thread *t = thread_current();	

	if (!is_user_vaddr(addr) || addr == NULL ||				// 1) addr이 유저 영역 가상주소인지 확인 (커널 영역 접근 방지)
	pml4_get_page(t->pml4, addr) == NULL)					// 2) addr이 NULL 포인터인지 확인
	{														// 3) 현재 프로세스의 페이지 테이블에 매핑된 물리 페이지가 없는지 확인
		exit(-1);
	}
}