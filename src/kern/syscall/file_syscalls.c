#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>

int sys_write(int fd, userptr_t buf_ptr, size_t size){
    int i;
    char *p = (char *)buf_ptr;

    if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
        kprintf("sys_write: invalid file descriptor\n");
        return -1;
    }

    for (i=0; i<(int)size; i++) {
        putch(p[i]);
    }

    return (int)size;
}

int sys_read(int fd, userptr_t buf_ptr, size_t size){
    int i;
    char *p = (char *)buf_ptr;

    if (fd!=STDIN_FILENO) {
        kprintf("sys_read: invalid file descriptor\n");
        return -1;
    }

    for (i=0; i<(int)size; i++) {
        p[i] = getch();
        if (p[i]<0){
            return i;
        }
    }

    return (int)size;
}