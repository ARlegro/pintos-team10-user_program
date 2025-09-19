#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

void syscall_init (void);

// Project_2
void halt (void);
void exit (int);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open(const char *file);
int write (int fd, const void *buffer, unsigned length);
void close(int fd);
void check_address (void *addr);

#endif /* userprog/syscall.h */
