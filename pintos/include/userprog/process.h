#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

// Project_2
void argument_stack(char **argv, int argc, struct intr_frame *if_);
void check_address(void *addr);

int fd_table_add_file(struct file *p_file);
struct file *fd_table_get_file(int fd);
int fd_table_close_file(int fd);

#endif /* userprog/process.h */
