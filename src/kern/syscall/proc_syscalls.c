#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>


void sys__exit(int status) {
    struct addrspace * cur =  proc_getas();
    as_destroy(cur);
    thread_exit();

    panic("thread_exit returned (should not happen)\n");
    (void) status; // TODO: status handling
}