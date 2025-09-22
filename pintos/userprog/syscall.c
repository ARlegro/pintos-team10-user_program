#include "string.h"
#include "threads/palloc.h"
#include "string.h"
#include "lib/stdarg.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "include/threads/init.h"
#include "lib/kernel/console.h"
#include "userprog/syscall.h"
#include <stdio.h>
//#include <stdlib.h>
#include "lib/string.h"
#include <threads/malloc.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
typedef int pid_t;
static struct lock filesys_lock;
#define MAX_ARGV 64

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static uint64_t sys_get_file_length(int fd);
static pid_t sys_fork(const char *thread_name);
static pid_t sys_wait(pid_t pid);
static tid_t sys_exec(char *file);
static bool sys_remove(const char *file);
static int sys_open_file(const char *file);
static int sys_read(int fd, void *buffer, unsigned size);
static bool sys_file_create (const char *file, unsigned initial_size);
void sys_exit (int status);
static unsigned sys_tell(int fd);
static void sys_seek(int fd, unsigned position);
static int sys_write(int fd, const void *buf, size_t size);
static void sys_close(int fd);

static struct file *find_file_by_fd(int fd);
struct fd_table_entry *find_file_entry_by_fd(int fd);
static void strong_validate_addr(const void *buf, size_t size);
static void validate_fd(int fd);
static bool validate_user_vaddr(const void *addr);
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}

void syscall_handler (struct intr_frame *f UNUSED) {
	

	struct thread *cur = thread_current();
	int sys_number = f->R.rax;
	switch (sys_number){
		case SYS_HALT:{
			power_off();
			break;
		}
			
		case SYS_EXIT: {
			int status = (int) f->R.rdi;  
			sys_exit(status);
			break;
		}
			
		case SYS_FORK:{
			const char *thread_name = (char *) f->R.rdi;
			cur->tf = *f;
			f->R.rax = sys_fork(thread_name);
			break;
		}
			

		case SYS_EXEC:{
			char *command_line = (char *) f->R.rdi;
			tid_t reuslt = sys_exec(command_line);
			if (reuslt == TID_ERROR){
				f->R.rax = reuslt;
			}
			break;
		}

			

		case SYS_WAIT:{
			pid_t child_pid = (pid_t) f->R.rdi;
			f->R.rax = process_wait(child_pid);
			break;
		}
			

		case SYS_CREATE: {
			const char *file = (const char *) f->R.rdi;
			unsigned initial_size = (int) f->R.rsi;
			f->R.rax = sys_file_create(file, initial_size);
			break;
		}
			

		case SYS_REMOVE:{
			const char *file = (const char *) f->R.rdi;
			f->R.rax = sys_remove(file);
			break;
		}
			

		case SYS_OPEN:{
			const char *file = (const char *) f->R.rdi;
			int result = sys_open_file(file);
			f->R.rax = result;
			break;
		}


		case SYS_FILESIZE:{
			int fd = (int) f->R.rdi;
			f->R.rax = sys_get_file_length(fd);
			break;
		}

		case SYS_READ: {
			int fd = (int) f->R.rdi;
			void *buf = (void *) f->R.rsi;
			unsigned size = (unsigned) f->R.rdx;
			int result = sys_read(fd, buf, size);
			f->R.rax = result;
			break;
		}

		case SYS_WRITE:{
			int fd = (int) f->R.rdi;
			const void *buf = (const void *) f->R.rsi;
			size_t size = (int) f->R.rdx;
			int result = sys_write(fd, buf, size);
			f->R.rax = result;
			break;
		}

		case SYS_SEEK:{
			int fd = (int) f->R.rdi;
			unsigned position = (unsigned) f->R.rsi;
			sys_seek(fd, position);
			break;
		}
			
		// 열려진 파일 fd에서 읽히거나 써질 다음 바이트의 위치를 반환
		case SYS_TELL: {
			// unsigned tell (int fd);
			f->R.rax = sys_tell((int) f->R.rdi);
			break;
		}
			

		case SYS_CLOSE:{
			sys_close((int) f->R.rdi);
			break;
		}

//int dup2 (int oldfd, int newfd)
			

		default:{
			sys_exit(-1);
		}
	}
}

pid_t sys_fork(const char *thread_name){
	bool result = validate_user_vaddr(thread_name);
	if (result == false){
		return TID_ERROR;
	}
	return process_fork(thread_name, &thread_current()->tf);
}

/**
 * command_line으로 실행가능한 파일명 주어진 것 
 * Return 값 
 *   - 성공 시 : void 
 *   - 실패 시 : exit state -1 반환 
 * hint : file descriptor는 exec 함수 호출 시에 열린 상태로 있다
 * 흠... 힌트가 file.c의 file_reopen 쓰라는 뜻인가 
 */ 
tid_t sys_exec(char *command_line){
	// command_line은 유저 포인터니까 커널 포인터로 변경 (init에서 했던 것 처럼)
	if (validate_user_vaddr(command_line) == false){
		thread_current()->exit_status = -1;
		return TID_ERROR;
	}
	tid_t tid = syscall_process_execute(command_line);
	if (tid < 0){
		thread_current()->exit_status = -1;
		return TID_ERROR;
	} 
	return tid;
}

// 유저 가상주소인지 + 실제 매핑되어 있는지 검증 
bool validate_user_vaddr(const void *addr) {
	if (addr == NULL || !(is_user_vaddr(addr)) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		return false;
	}
	return true;
}

uint64_t sys_get_file_length(int fd) {
	struct file *file_ptr = find_file_by_fd(fd);
	if (file_ptr == NULL){
		return -1;
	}
	
	lock_acquire(&filesys_lock);
	off_t size = file_length(file_ptr);
	lock_release(&filesys_lock);
	return (uint64_t)size;
}


// return : 실제 읽은 값  (오류 시 -1)
int sys_read (int fd, void *buffer, unsigned size) {
	if (fd == STDIN_FILENO){
		for (int i = 0; i < size; i++){
			((uint8_t *)buffer)[i] = input_getc();
		}
		return size;
	}

	if (fd == STDOUT_FILENO) {
		return -1;
	}
	
	strong_validate_addr(buffer, size);
	struct file *to_read_file = find_file_by_fd(fd);
	
	if (to_read_file == NULL) {
		return -1;
	}

	lock_acquire(&filesys_lock);
	int n = file_read(to_read_file, buffer, (off_t) size);
	lock_release(&filesys_lock);
	return n;
}


/**
 * Return
 * - 성공 시 : FD
 * - 실패 시 : -1
 * 각각의 프로세스는 독립적인 파일 식별자들을 갖는다.
 * 파일 식별자는 자식 프로세스들에게 상속(전달)됩니다. 
 * 하나의 프로세스에 의해서든 다른 여러개의 프로세스에 의해서든, 하나의 파일이 두 번 이상 열리면 그때마다 open 시스템콜은 새로운 식별자를 반환합니다. 
 * FD는 포인터 주소만 -> 이중 포인터
 */
int sys_open_file(const char *file){
	
	if (file == NULL || !(is_user_vaddr(file)) || pml4_get_page(thread_current()->pml4, file) == NULL) {
		return -1;
	}
	// 커널 주소로 쓰레기 값 넘어오는 것도 

	// validate_addr(file);
	char *kernel_file = palloc_get_page(0);
	if (kernel_file == NULL){
		return -1;
	}

	strlcpy(kernel_file, file, PGSIZE);

	// 락 걸기 
	lock_acquire(&filesys_lock);
	struct file *opened_file = filesys_open(kernel_file);
	lock_release(&filesys_lock);
	// 누수 
	palloc_free_page(kernel_file);
	if (opened_file == NULL){
		return -1;
	}

	struct thread *cur = thread_current();
	struct fd_table_entry *entry = malloc(sizeof(struct fd_table_entry));
	if (entry == NULL){
		lock_acquire(&filesys_lock);
		file_close(opened_file);
		lock_release(&filesys_lock);
		return -1;
	}

	entry->fd = cur->next_fd;
	cur->next_fd++;
	entry->file = opened_file;
	list_push_back(&cur->fd_table, &entry->elem);

	return entry->fd;
}

// 파일이 성공적으로 생성 시 true 반환 
// 생성만하고 open하지 않는다.
bool sys_file_create (const char *file, unsigned initial_size) {
	validate_addr(file);
	return filesys_create(file, initial_size);
}


void sys_exit(int status){

	struct thread *cur = thread_current();
	cur->exit_status = status;
	printf("%s: exit(%d)\n", cur->name, status);

	// if (cur->parent != NULL){
	// 	if (cur->running_file){
	// 		file_allow_write(cur->running_file);
	// 		file_close(cur->running_file);
	// 		cur->running_file = NULL;
	// 	}

  if (cur->parent != NULL) {
    sema_up(&cur->wait_sema);          
    sema_down(&cur->exit_sema);        
  }
	
	thread_exit();
}


// 열려진 파일 fd에서 읽히거나 써질 다음 바이트의 위치를 반환
// 파일의 시작지점부터 몇바이트인지로 표현
unsigned sys_tell(int fd) {
	if (fd < 0 || fd > 126) {
		sys_exit(-1);
	}
	
	struct file *file_ptr = find_file_by_fd(fd);
	if (file_ptr == NULL){
		sys_exit(-1);
	}
	
	return (unsigned) file_tell(file_ptr);
}


void sys_seek(int fd, unsigned position) {
	if (fd < 0 || fd > 126) {
		sys_exit(-1);
	}
	struct file *file_ptr = find_file_by_fd(fd);
	if (file_ptr == NULL){
		sys_exit(-1);
	}
	
	lock_acquire(&filesys_lock);
	file_seek(file_ptr, position);
	lock_release(&filesys_lock);
}

// Note : arg buf는 사용자 프로세스 주소 공간에 있는 포인터 
int sys_write(int fd, const void *buf, size_t size){ 
	if (size == 0) return 0;
	if (buf == NULL || fd <= STDIN_FILENO){
		return -1;
	} 	
	
	strong_validate_addr(buf, size);

	// 1 = STDOUT
	if (fd == STDOUT_FILENO) {
		putbuf(buf, size);
		return (int) size;
	}

	struct file *to_write_file = find_file_by_fd(fd);
	if (to_write_file == NULL){
		sys_exit(-1);
	}

	lock_acquire(&filesys_lock);
	int n = file_write(to_write_file, buf, size);
	lock_release(&filesys_lock);
	return n;
}


void sys_close (int fd) {
	if (fd < 0 || fd > 126) {
		return;
	}
	
	const struct thread *cur = thread_current();
	// 이거 매우 비효율적임. 나중에 배열 형식으로 바꾸기 
	struct fd_table_entry *entry = find_file_entry_by_fd(fd);
	if (entry == NULL){
		return;
	}
	
	struct file *file_ptr = entry->file;
	lock_acquire(&filesys_lock);
	file_close(file_ptr);
	lock_release(&filesys_lock);	
	list_remove(&entry->elem);
	free(entry);
}

bool sys_remove(const char *file) {
	validate_addr(file);
	lock_acquire(&filesys_lock);
	int result = filesys_remove(file);
	lock_release(&filesys_lock);
	return result;
}


/**
 * 검증할 것 
 * - buf가 실제로 매핑된 메모리가 있는지
 * - buf가 user영역에 속하는지
 */ 
void strong_validate_addr(const void *buf, size_t size) {
	// TODO: Your implementation goes here.
	const uint8_t *start = (uint8_t *)buf;
	const uint8_t *end = (uint8_t *)buf + size - 1;
	
	// 유효한 주소 인지 
	if (!is_user_vaddr((void *)start) || !is_user_vaddr((void *)end)) {
		sys_exit(-1);
	}

	// pg_round_down(va) : 이 va주소가 속한 페이지의 시작 주소를 구하는 메크로 
	// 페이지가 걸칠 수 있기 때문에 페이지 단위로 loop돌면서 pml4_get_page()로 매핑 여부 확인 
	for (uint8_t *p = pg_round_down((void *)start); p <= pg_round_down((void *)end); p += PGSIZE){
		if (pml4_get_page(thread_current()->pml4, p) == NULL) {
			sys_exit(-1);
		}
	}
}

void validate_fd(int fd) {
	const struct thread *cur = thread_current();	
	if (fd < 0 || fd > 512) {
		sys_exit(-1);
	}
	// if (cur->fd_table[fd] == NULL) {
	// 	sys_exit(-1);
	// }
}

void validate_addr(const void *addr) {
	if (addr == NULL || !(is_user_vaddr(addr)) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		sys_exit(-1);
	}
}

struct file *find_file_by_fd(int fd){
	const struct thread *cur = thread_current();

	for(struct list_elem *e = list_begin(&cur->fd_table);
		e != list_end(&cur->fd_table);
		e = list_next(e))
	{
		struct fd_table_entry *entry = list_entry(e, struct fd_table_entry, elem);
		if (entry->fd == fd){
			return entry->file;
		}
	}
	return NULL;
}



struct fd_table_entry *find_file_entry_by_fd(int fd){
	const struct thread *cur = thread_current();

	for(struct list_elem *e = list_begin(&cur->fd_table);
		e != list_end(&cur->fd_table);
		e = list_next(e))
	{
		struct fd_table_entry *entry = list_entry(e, struct fd_table_entry, elem);
		if (entry->fd == fd){
			return entry;
		}
	}
	return NULL;
}