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
#include "lib/kernel/stdio.h"
#include "filesys/file.h"
#include "threads/synch.h"
#ifdef USERPROG
#include "threads/palloc.h"
#include "string.h"
#endif

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

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

	// Project_2
	// read & write 용 lock 초기화
	lock_init(&filesys_lock);
}

/* The main system call interface */
/* 메인 시스템 콜 인터페이스 */
// rax -> 시스템 콜 번호, rdi, rsi, rdx, r10, r8, r9 순서, x86-64 System Call Calling Convention
void
syscall_handler (struct intr_frame *f UNUSED) {

	// f->R.rax 는 시스템 콜 번호가 들어있는 레지스터 값
	int sys_number = f->R.rax;										// f는 인터럽트가 발생했을 때 CPU 레지스터 상태를 저장한 구조체, R은 범용 레지스터 집합, rdi = 파일 이름 문자열, rsi = 초기 크기 값
														
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
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = process_wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);														
		break;	
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	default:
		exit(-1);
	}

	// printf ("system call!\n");
	// thread_exit ();
}

void halt(void)
{
	power_off();											// 프로세스가 죽는다
}

void exit(int status)
{
	struct thread *t = thread_current();
	t->exit_status = status;
	printf("%s: exit(%d)\n", t->name, status);
	thread_exit();
}

pid_t fork (const char *thread_name)
{
	check_address(thread_name);

	return process_fork(thread_name, NULL);
}

int exec (const char *cmd_line)
{
	check_address(cmd_line);

	size_t cmd_size = strlen(cmd_line) + 1;				

	// 메모리 할당
	char *temp_buf = palloc_get_page(PAL_ZERO);
	
	if (temp_buf == NULL)
	{
		return -1;
	}
	
	// 커널 공간에 복사
	memcpy(temp_buf, cmd_line, cmd_size);

	// 프로세스 실행
	if (process_exec(temp_buf) == -1)
	{
		return -1;
	}	

	return 0;
}

bool create(const char *file, unsigned initial_size)
{
	check_address(file);									// 포인터 안정성 체크

	return filesys_create(file, initial_size);				// 커널 내부에서 파일을 생성하는 함수
}

bool remove(const char *file)
{
	check_address(file);
	
	return filesys_remove(file);							// 커널 내부 파일 시스템에서 해당 파일 삭제
}

int open(const char *file)
{
	check_address(file);									// 포인터 안정성 검사
	struct file *p_file = filesys_open(file);				// 파일 열기

	if (p_file == NULL)										// 파일 없으면
	{
		return -1;
	}

	int fd = fd_table_add_file(p_file);						// 파일 디스크립터 테이블에 파일 추가

	if (fd == -1)											// 실패시
	{
		file_close(p_file);
	}

	return fd;
}

int filesize (int fd)
{
	struct file *p_file = fd_table_get_file(fd);

	if (p_file == NULL)
	{
		return -1;
	}

	return file_length(p_file);
}

int read(int fd, void *buffer, unsigned size)
{
	if (size == 0) return 0;

	check_address(buffer);

	struct file *p_file = fd_table_get_file(fd);				// fd에 해당하는 파일 구조체를 가져옴
	int bytes = -1;												// 실패 시 -1 반환

	// 표준 입력 (키보드)
	if (fd == 0)
	{
		for (unsigned i = 0; i < size; i++)						// 한 글자씩 읽어서 buffer에 저장
		{
			((char *)buffer)[i] = input_getc();					// 키보드로부터 문자 하나 입력받기
		}

		bytes = (int)size;
		return bytes;
	}

	if (fd < 3 || p_file == NULL)								// stdout/stderr는 read 불가
	{
		return -1;
	}
																// 일반 파일, fd가 0이 아니고, fd 테이블에 해당 파일이 존재할 때
	lock_acquire(&filesys_lock);								// 파일 시스템은 공유 자원이므로 동시 접근 방지를 위해 lock 획득
	bytes = file_read(p_file, buffer, size);					// p_file에서 size 바이트를 읽어서 buffer에 저장
	lock_release(&filesys_lock);								// 다 읽었으면 lock 해제
	
	return bytes;
}

int write(int fd, const void *buffer, unsigned length)
{
	check_address(buffer);

	struct file *p_file = fd_table_get_file(fd);				
	off_t bytes = -1;											// 실제로 기록한 바이트 수

	if (fd <= 0 || p_file == NULL)
	{
		return -1;
	}

	if (fd < 3)													// 표준 출력일 경우만 처리
	{	
		putbuf(buffer, length);									// 콘솔 버퍼에 문자열 출력
		bytes = length;		
		return bytes;									
	}	
	
	lock_acquire(&filesys_lock);
	bytes = file_write(p_file, buffer, length);
	lock_release(&filesys_lock);		
	
	return bytes;												// 출력한 바이트 수 반환
}

void seek (int fd, unsigned position)
{
	struct file *p_file = fd_table_get_file(fd);

	if (fd < 3 || p_file == NULL)
	{
		return;
	}

	file_seek(p_file, position);
}

int tell (int fd)
{
	struct file *p_file = fd_table_get_file(fd);

	if (fd < 3 || p_file == NULL)
	{
		return;
	}

	return file_tell(p_file);
}

void close(int fd)
{
	struct file *file = fd_table_get_file(fd);

	if (fd < 3 || file == NULL)								// 표준 입출력 fd(0,1,2)거나, 잘못된 fd(NULL)이면 무시
	{
		return;
	}

	fd_table_close_file(fd);								// 파일 디스크립터 테이블에서 fd를 제거(NULL)
	file_close(file);										// 실제 파일 객체를 닫음(참조 해제)
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
