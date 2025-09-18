#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

void syscall_init (void);
void check_address (void *addr);
void halt (void);
void exit (int);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);

#endif /* userprog/syscall.h */
