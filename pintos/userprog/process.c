#include "threads/synch.h"
#include "include/lib/string.h"
#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "userprog/syscall.h"

#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_ARGV 64

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static bool validate_user_vaddr(const void *addr);
static int tokenize_command_line(char *command_line, char **argv);
static void *push(struct intr_frame *interrupt_frame, const void *src, size_t n);
static void align_stack(struct intr_frame *interrupt_frame);
static void build_stack(struct intr_frame *interrupt_frame, char *argv[], int argc);
static struct thread *find_child_thread_by_tid(tid_t child_tid);
static tid_t syscall_exec(char *command_line);


/* General process initializer for initd and other process. */
// init_thread는 커널쓰레드까지 포함
// process_init은 user-only 자원을 가진 process를 init
static void process_init (void) {
	struct thread *cur = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}


/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	if (validate_user_vaddr(name) == false){
		return TID_ERROR;	
	}

	char thread_name[16];
	strlcpy(thread_name, name, sizeof(thread_name));
	
	// 1. 메모리 할당 
	struct fork_args *args = palloc_get_page (0);
	if (args == NULL){
		return TID_ERROR;
	}

	// 2. 값 채워 넣기 
	args->parent = thread_current ();
	args->parent_intr_f = *if_;
	args->is_forked = true;
	sema_init(&args->fork_sema, 0);

	// 3. thead_create() 호출 (전달할 데이터 전달하기)
	tid_t tid = thread_create (thread_name, PRI_DEFAULT, __do_fork, (void *) args);
	if (tid == TID_ERROR){
			palloc_free_page (args);
			return TID_ERROR;
	}

	// 4. 자식 신호 대기 (자식의 fork_sema를 기다림)
	struct thread *child = find_child_thread_by_tid(tid);
	if (child == NULL) {
			palloc_free_page(args);
			return TID_ERROR;
	}
	
	sema_down(&args->fork_sema);
	// 5. 깬 뒤 메모리 정리 
	if (args->is_forked == false) { // fork 실패 
		tid = TID_ERROR;
		list_remove(&child->child_elem);
		sema_up(&child->exit_sema); // 자식도 정리하라고 깨우기
	}
	palloc_free_page (args);
	
	return tid;
}
// 유저 가상주소인지 + 실제 매핑되어 있는지 검증 
bool validate_user_vaddr(const void *addr) {
	if (addr == NULL || !(is_user_vaddr(addr)) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		return false;
	}
	return true;
}


#ifndef VM
/**
 * 역할 : Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2.
 * 
 * Args:
 * 1. pte 
 * - 부모 프로세스의 페이지 테이블에서, 가상주소 va에 대응하는 리프 엔트리의 주소
 * - 용도 : 페이지 존재 여부, USER영역 여부, 쓰기 가능 여부
 * 
 * 2. va : 복제해야 할 가상 페이지의 기준 주소 
 * 3. aux : 인자 
 */ 

/** 
 * Page 내용이 있는 커널 주소 얻는 법 = pml4_get_page(parent->pml4, va) 
 * (Warning : parent->pml4는 부모의 PML4 테이블 최상위 포인터일 뿐 부모의 데이터 페이지가 아님)
 * 즉, 부모의 데이터 페이지는 pml4_get_page(parent->pml4, va)로 구하기
 */ 
static bool duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;

	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	// 커널용 페이지는 복제 ㄴㄴ 
	if (pte == NULL || is_kern_pte(pte)) {
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL) {
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL) {
		return false;
	}

	// memcpy(newpage, parent_page, PGSIZE); 
	memcpy(newpage, pg_round_down(parent_page), PGSIZE); // 페이지 시작 기준으로 전체 페이지 복사
	writable = is_writable(pte);

	// 자식 pml4에 매핑 
	if (!pml4_set_page (current->pml4, pg_round_down(va), newpage, writable)) {
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/**
 * 1. 부모가 넘겨준 값 받기 
 * 2. 부모의 CPU 레지스터 복사
 * 3. 자식의 페이지 테이블 생성 및 활성화 
 * 4. 부모의 가상 주소 공간 복사
 * 5. 부모의 fd 테이블 복사
 * 6. 부모 깨우기
 * 7. 성공 처리 
 */
 static void __do_fork (void *aux) {
	struct intr_frame if_;

	// 1. 부모가 넘겨준 값 받기 
	struct fork_args *fork_args = (struct fork_args *) aux;
	struct thread *parent = fork_args->parent;
	struct thread *current = thread_current ();
	struct intr_frame parent_if = fork_args->parent_intr_f;

	bool succ = true;

	// 2. 부모의 CPU 레지스터(유저 컨텍스트) 복사 
	memcpy (&if_, &parent_if, sizeof (struct intr_frame));
	current->tf = if_;
	
	// 3. 자식의 페이지 테이블 생성 및 활성화 
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	// 4. 부모의 가상 주소 공간 복사
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	// 5. 부모의 fd 테이블 복사
	if (!list_empty(&parent->fd_table)){

		for (struct list_elem *e = list_front(&parent->fd_table);
			e != list_end(&parent->fd_table); 
			e = list_next(e)) {
			
			struct fd_table_entry *parent_entry = list_entry(e, struct fd_table_entry, elem);
			if (parent_entry == NULL || parent_entry ->fd == 0 || parent_entry->fd == 1){
				continue;
			}
			
			struct fd_table_entry *child_entry = malloc(sizeof(struct fd_table_entry));
			if (child_entry == NULL){
				goto error;
			}

			child_entry->fd = parent_entry->fd;
			child_entry->file = file_duplicate(parent_entry->file);
			list_push_back(&current->fd_table, &child_entry->elem);
		}
	}

	current->next_fd = parent->next_fd;

	process_init ();

	// 7. 성공 시 rax 0세팅 + do_iret, 실패 시 에러로 goto
	if (!succ) {
		goto error;
	} 

	// 6. 부모 깨우기 (자식의 fork_sema로 신호)
	sema_up(&fork_args->fork_sema);	
	current->tf.R.rax = 0;
	do_iret (&current->tf);
		
error:
	fork_args->is_forked = false;
	sema_up(&fork_args->fork_sema);
	
	// test 
	sema_init(&current->exit_sema, 0);
	sema_down(&current->exit_sema);
	thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */

/**
 * <페이지 버퍼 해제>
 * thread_create에서 부모가 넘겨준 f_name은 page할당을 받아서 힙에 있음 
 * 자식 thread_create가 실패하면 부모가 해제한다.
 * But 자식 thread_create가 성공하면 소유권을 갖고 있는 자식이 페이지 버퍼를 해제해야 함 
 */ 
 /**
 * arg(f_name) = 전체 커멘드 라인의 시작 주소 
 * ./filename -p 8080:8181 good 
 */ 
int process_exec (void *f_name) {
	struct thread *cur = thread_current ();
	char *command_line = f_name;
	bool success;
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 이전 컨텍스트 정리: 페이지 테이블/파일 핸들 등 정리 */
	process_cleanup ();

	char *argv[MAX_ARGV];  // main에 넘길 argv는 이중 포인터
	int argc = tokenize_command_line(command_line, argv);
	if (argc == 0){
			palloc_free_page (f_name);
			return -1;
	}
	
	success = load (argv[0], &_if);
	if (!success){
			thread_current()->exit_status = -1;
			palloc_free_page (f_name);
			return TID_ERROR;
	}
	
	build_stack(&_if, argv, argc);

	strlcpy(thread_current()->name, argv[0], sizeof(thread_current()->name));

	palloc_free_page (f_name);
	
	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

tid_t syscall_process_execute (const char *file_name) {
	char *fn_copy;
	tid_t tid;
	if (!validate_user_vaddr(file_name)) {
		return TID_ERROR;
	}

	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL){
		return TID_ERROR;
	}
		
	strlcpy (fn_copy, file_name, PGSIZE);
	tid = syscall_exec(fn_copy);
	// 사실 여기 도달하면 무조건 TID_ERROR임 
	if (tid == TID_ERROR){
		palloc_free_page (fn_copy);		
	}
	return tid;
}

tid_t syscall_exec(char *command_line){
	struct thread *cur = thread_current();
	struct intr_frame _if;
	bool success;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	char *argv[MAX_ARGV];
	int argc = tokenize_command_line(command_line, argv);
	if (argc == 0){
			return TID_ERROR;
	}

	process_cleanup ();

	success = load (argv[0], &_if);
	if (!success){
			return TID_ERROR;
	}
	
	build_stack(&_if, argv, argc);
	palloc_free_page (command_line);
	do_iret (&_if);
	NOT_REACHED ();
	return TID_ERROR;
}



// return arg count
int tokenize_command_line(char *command_line, char **argv) {
	char *save_ptr = NULL;
	int argc = 0;
	for(char *token = strtok_r(command_line, " ", &save_ptr); 
			(token != NULL && argc < MAX_ARGV); 
			token = strtok_r(NULL, " ", &save_ptr))
	{			
				argv[argc++] = token;
	}

	if (argc < MAX_ARGV) {
		argv[argc] = NULL;
	}
	return argc;
}

void *push (struct intr_frame *interrupt_frame, const void *src, size_t n) {
	interrupt_frame->rsp -= n;
	memcpy((void *)interrupt_frame->rsp, src, n);
	return (void *)interrupt_frame->rsp;
}



/** todo 
 * 1. argv에 저장된 문자열을 stack에 push (rsp부터)
 * 2. 정렬하기
 * 3. NULL 센티널 넣기 
 * 4. argv 포인터들을 역순으로 push 
 * 5. 레지스터 세팅
 * 6. fake return 주소 push
 */ 
void build_stack(struct intr_frame *interrupt_frame, char *argv[], int argc) {
	 // 1. argv에 저장된 문자열을 stack에 push
	 uintptr_t user_arg_ptrs[MAX_ARGV];
	 for (int i = argc - 1; i >= 0; i--){
		void *str_addr = push(interrupt_frame, argv[i], (strlen(argv[i]) + 1));
		user_arg_ptrs[i] = (uintptr_t) str_addr;
	 }

	 // 2. 정렬 
	 align_stack(interrupt_frame);
	 
	 // 3. NULL 센티널 
	 uintptr_t null_ptr = 0;
	 push(interrupt_frame, &null_ptr, sizeof(null_ptr));

	 // 4. argv 포인터들(이중 포인터) 역순으로 push
	 for (int i = argc - 1; i >= 0; i--){
		push(interrupt_frame, &user_arg_ptrs[i], sizeof(user_arg_ptrs[i]));
	 }

	 // 5. 레지스터 세팅 
	 interrupt_frame->R.rdi = (uint64_t) argc;
	 interrupt_frame->R.rsi = (uint64_t) interrupt_frame->rsp;

	 // 6. fake return 주소 push
	 uintptr_t fake_return = 0;
	 push(interrupt_frame, &fake_return, sizeof(fake_return));
}

void align_stack(struct intr_frame *interrupt_frame) {
	while ((interrupt_frame->rsp % 16) != 0){
		uint8_t zero = 0;
		push(interrupt_frame, &zero, 1);
	}
}



/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */	
	/**
	 * 1. 인자 tid에 해당하는 child thread 찾기 
	 * 2. 자식 스레드가 종료될 때까지 대기 - sema_down
	 * 3. 자식 스레드를 자식 리스트에서 제거 Cuz 자식 스레드 종료 예정
	 * 4. 자식 스레드 깨우기 - 자식도 정리하라고 
	 * 5. 자식 스레드의 종료 상태를 반환 
	 */
	struct thread *child = find_child_thread_by_tid(child_tid);
	if (child == NULL || child->is_waited){
		return -1;
	}
	
	child->is_waited = true;

	// 2. 자식 종료까지 대기 
	sema_down(&child->wait_sema);

	int status = child->exit_status;
	list_remove(&child->child_elem);
	
	// 자식 스레드 깨우기 
	sema_up(&child->exit_sema);
	return status;
}

static struct thread *find_child_thread_by_tid(tid_t child_tid) {
	struct list *child_list = &thread_current()->children;
	for (struct list_elem *e = list_begin(child_list);
			e != list_end(child_list); e = list_next(e)) {
		
		struct thread *child_thread = list_entry(e, struct thread, child_elem);
		if (child_thread->tid == child_tid){
			return child_thread;
		} 
	}
	return NULL;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit (void) {
	struct thread *cur = thread_current();

	// FD 테이블 전부 닫기
	while (!list_empty(&cur->fd_table)) {
			struct list_elem *e = list_pop_front(&cur->fd_table);
			struct fd_table_entry *entry = list_entry(e, struct fd_table_entry, elem);
			if (entry->file) {
				file_close(entry->file);
			}
			free(entry);
	}

  // 2) 실행 파일/열린 파일 정리 (중복 없이 여기서만)
  if (cur->running_file) {
    file_close(cur->running_file);
    cur->running_file = NULL;
  }

	process_cleanup ();
}

/* Free the current process's resources. */
static void process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	// build_stack()
	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	//file_close (file)
	if (success){
		file_deny_write(file);
		t->running_file = file;
	} else {
		file_close(file);
	}
	
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
