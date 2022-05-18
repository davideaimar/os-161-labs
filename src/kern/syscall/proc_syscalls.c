#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <current.h>


void sys__exit(int status) {

    #if OPT_PROCWAIT
    struct proc *p = curproc;
    /* Detach thread */
    proc_remthread(curthread);

    lock_acquire(p->p_lock_cv);
    /* Signal termination */
    p->p_exitstatus = status;
    cv_broadcast(p->p_cv, p->p_lock_cv);
    lock_release(p->p_lock_cv);
    
    #else
    (void)status;
    #endif

    thread_exit();

    panic("thread_exit returned (should not happen)\n");
}

#if OPT_PROCWAIT
/* Wait for a process to terminate. */
int sys_waitpid(pid_t pid){
	
    struct proc *p = proc_find(pid);
    
    if(p == NULL){
        return -1;
    }

	return proc_wait(p);
}

/* get PID of current process. */
pid_t sys_getpid(struct proc *p){
    
    if(p == NULL){
        return -1;
    }

	return proc_getpid(p);
}
/* Fork the parent process */
int sys_fork(struct trapframe * tf, int * retval) {
	
	(void) tf;
    (void) retval;

    // TODO: Implement this

	return 0;
}

#endif