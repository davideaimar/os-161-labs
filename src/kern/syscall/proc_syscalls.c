#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <current.h>
#include <mips/trapframe.h>

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

/* Fork */

static void
call_enter_forked_process(void *tfv, unsigned long dummy) {
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf); 
 
  panic("enter_forked_process returned (should not happen)\n");
}

int sys_fork(struct trapframe *ctf, pid_t *retval) {
  struct trapframe *tf_child;
  struct proc *newp;
  int result;

  KASSERT(curproc != NULL);

  newp = proc_create_runprogram(curproc->p_name);
  if (newp == NULL) {
    return ENOMEM;
  }

  /* done here as we need to duplicate the address space 
     of thbe current process */
  as_copy(curproc->p_addrspace, &(newp->p_addrspace));
  if(newp->p_addrspace == NULL){
    proc_destroy(newp); 
    return ENOMEM; 
  }

  /* we need a copy of the parent's trapframe */
  tf_child = kmalloc(sizeof(struct trapframe));
  if(tf_child == NULL){
    proc_destroy(newp);
    return ENOMEM; 
  }
  memcpy(tf_child, ctf, sizeof(struct trapframe));

  /* TO BE DONE: linking parent/child, so that child terminated 
     on parent exit */

  result = thread_fork(
		 curthread->t_name, newp,
		 call_enter_forked_process, 
		 (void *)tf_child, (unsigned long)0/*unused*/);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
    return ENOMEM;
  }

  *retval = newp->p_pid;

  return 0;
}

#endif