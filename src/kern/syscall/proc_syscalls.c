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
    /* Set exit status */
    p->p_exitstatus = status & 0xff;

    lock_acquire(p->p_lock_cv);
    /* Signal termination */
    cv_signal(p->p_cv, p->p_lock_cv);
    lock_release(p->p_lock_cv);
    
    #else
    struct addrspace *as = proc_getas();
    as_destroy(as);
    (void)status;
    #endif

    thread_exit();

    panic("thread_exit returned (should not happen)\n");
}

#if OPT_PROCWAIT
/* Wait for a process to terminate. */
pid_t sys_waitpid(pid_t pid, userptr_t *status_ptr){
	int ret_status;
    struct proc *p = proc_find(pid);
    
    if(p == NULL){
        return -1;
    }

    ret_status = proc_wait(p);

    if (status_ptr != NULL) {
        *(int*)status_ptr = ret_status;
    }

    return pid;
}

/* get PID of current process. */
pid_t sys_getpid(struct proc *p){
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