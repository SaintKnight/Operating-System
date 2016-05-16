#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <synch.h>
#include "opt-A2.h"
#if OPT_A2
#include <mips/trapframe.h>
#include <limits.h>
#include <kern/fcntl.h>
#include <vfs.h>
#endif

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
#if OPT_A2
  KASSERT(globalarrs != NULL);

  lock_acquire(globalarrs);
  unsigned t = array_num(pidarr);
  struct pidinfo *temp;
  unsigned i = 0;
  for(; i < t; i++){
    temp = (struct pidinfo *)array_get(pidarr, i);
    if(temp->pid == p->pid){
      break;
    }
  }
  // check children
  struct pidinfo *child;
  unsigned j = 0;
  int test = 0;
  for(; j < t; j++){
    if(test == 1){
      j--;
      t--;
      test = 0;
    }
    child = (struct pidinfo *)array_get(pidarr, j);
    if(child->parent == p->pid){
      // remove exited child
      if(child->exitalready == 1){
        array_remove(pidarr, j);
        if(j != 0){
          j--;
          t--;
        } else {
          test = 1;
        }
        pid_t *val1;
        val1 = kmalloc(sizeof(*val1));
        *val1 = child->pid;
        array_add(reusepid, val1, NULL);
        pidinfo_destroy(child);
      } else { // set others to orphan
        child->parent = 0;
      }
    }
  }
  // check parent
  if(temp->parent == 0){
    array_remove(pidarr, i);
    pid_t *val;
    val = kmalloc(sizeof(*val));
    *val = temp->pid;
    array_add(reusepid, val, NULL);
    pidinfo_destroy(temp);
  } else {
    temp->exitalready = 1;
    temp->exitno = _MKWAIT_EXIT(exitcode);
    cv_broadcast(cvpid, globalarrs);
  }
  lock_release(globalarrs);
#else
  (void)exitcode;
#endif

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  #if OPT_A2
    *retval = curproc->pid;
  #else
    *retval = 1;
  #endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
      userptr_t status,
      int options,
      pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */
  if (options != 0) {
    return(EINVAL);
  }
#if OPT_A2
  // EFAULT
  if (status == NULL) {
    return(EFAULT);
  }

  KASSERT(globalarrs != NULL);

  lock_acquire(globalarrs);
  int isfound = 0;
  unsigned t = array_num(pidarr);
  struct pidinfo *temp;
  unsigned i = 0;
  for(; i < t; i++){
    temp = (struct pidinfo *)array_get(pidarr, i);
    if(temp->pid == pid){
      isfound = 1;
      break;
    }
  }
  // ESRCH
  if (isfound == 0){
    lock_release(globalarrs);
    return(ESRCH);
  }
  // ECHILD not interested in
  if(temp->parent != curproc->pid) {
    lock_release(globalarrs);
    return(ECHILD);
  }

  while(temp->exitalready == 0){
    cv_wait(cvpid,globalarrs);
  }
  exitstatus = temp->exitno;
  lock_release(globalarrs);
#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

void
remove_unsuccess_child(pid_t pid){
  lock_acquire(globalarrs);
  struct pidinfo *temp;
  unsigned i = 0;
  unsigned t = array_num(pidarr);
  for(; i < t; i++){
    temp = (struct pidinfo *)array_get(pidarr, i);
    if(temp->pid == pid){
      // remove
      array_remove(pidarr, i);
      pid_t *k;
      k = &pid;
      array_add(reusepid, k, NULL);
      pidinfo_destroy(temp);
      break;
    }
  }
  lock_release(globalarrs);
}

#if OPT_A2
int
sys_fork(pid_t *retval, struct trapframe *tf){
  // Create process structure for child process
  struct proc * p = proc_create_runprogram("prog");
  if(p == NULL){
    return ENOMEM;
  }
  if(p->pid > PID_MAX){
    lock_acquire(globalarrs);
    struct pidinfo *temp;
    unsigned i = 0;
    unsigned t = array_num(pidarr);
    for(; i < t; i++){
      temp = (struct pidinfo *)array_get(pidarr, i);
      if(temp->pid == p->pid){
        // remove
        array_remove(pidarr, i);
        pidinfo_destroy(temp);
        break;
      } 
    }
    lock_release(globalarrs);
    proc_destroy(p);
    return ENPROC;
  }
  // Create and copy address space
  // copy addr space
  int q;
  struct addrspace *ret;
  q = as_copy(curproc->p_addrspace, &ret);
  if(q != 0){
    remove_unsuccess_child(p->pid);
    proc_destroy(p);
    return ENOMEM;
  }
  // associate with new proc
  spinlock_acquire(&p->p_lock);
  p->p_addrspace = ret;
  spinlock_release(&p->p_lock);

  // Assign PID to child process and create the parent/child relationship
  lock_acquire(globalarrs);
  p->info->parent = curproc->pid;
  lock_release(globalarrs);
  // Create thread for child process
  // thread fork
  int s;
  // thrfunc put the trapframe onto the stack, 
  //         modify the trapframe, 
  //         call mips_usermode to go back to userspace.
  struct trapframe *tfcopy ;
  tfcopy = kmalloc(sizeof(*tfcopy));
  if(tfcopy == NULL){
    remove_unsuccess_child(p->pid);
    kfree(ret);
    proc_destroy(p);
    return ENOMEM;
  }
  *tfcopy = *tf;
  s = thread_fork("newthr", p, enter_forked_process, tfcopy, 0);
  if(s != 0){
    remove_unsuccess_child(p->pid);
    kfree(tfcopy);
    kfree(ret);
    proc_destroy(p);
    return ENOMEM;
  }
  *retval = p->pid;
  return 0;
  // pass trapframe to the child thread
  // Child thread needs to put the trapframe onto its stack and modify it so that it returns the current value
}

int sys_execv(userptr_t progname, userptr_t args){
  struct addrspace *asnew;
  struct addrspace *asold;
  struct vnode *v;
  vaddr_t entrypoint, stackptr;
  int result;

  // count and copy arguments///////////////////////////////////////////
  unsigned long num = 0;
  if(!args){
    return EFAULT;// ?
  }
  while(((char **)args)[num] != NULL) num++;//consider

  // copy arr
  size_t len = sizeof(char *) * (num + 1);
  char** copyargs = kmalloc(len);
  // check kmalloc
  if(!copyargs){
    return ENOMEM;
  }
  result = copyin(args, copyargs, len);
  if(result){
    // free
    kfree(copyargs);
    return result;
  }

  // copy args
  size_t totalen = 0;
  for(unsigned long i = 0; i < num; i++){
    len = strlen(((char **)args)[i]) + 1;
    totalen = totalen + len;
    if(totalen > ARG_MAX){
      // free
      for(unsigned long j = 0; j < i; j++){
        kfree(copyargs[j]);
      }
      kfree(copyargs);
      return E2BIG;
    }
    
    copyargs[i] = kmalloc(len);
    // check kmalloc
    if(!copyargs[i]){
      // free
      for(unsigned long j = 0; j < i; j++){
        kfree(copyargs[j]);
      }
      kfree(copyargs);
      return ENOMEM;
    }

    result = copyinstr((userptr_t)((char**)args)[i], copyargs[i], len, NULL); //args + i * 4?
    if(result) {
      // free
      for(unsigned long j = 0; j <= i; j++){
        kfree(copyargs[j]);
      }
      kfree(copyargs);
      return result;
    }
  }

  // copy program path////////////////////////////////////////////////////
  if(!progname){
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    return EFAULT; //?
  }
  
  len = strlen((char *)progname) + 1;
  if(len > PATH_MAX) {
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    return E2BIG; // ? path max
  }
  char* copypath = kmalloc(len);
  // check kmalloc
  if(!copypath){
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    return ENOMEM;
  }

  result = copyinstr(progname, copypath, len, NULL);
  if(result){
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    kfree(copypath);
    return result;
  }

  /* Open the file. */////////////////////////////////////////////////////////
  result = vfs_open(copypath, O_RDONLY, 0, &v);
  kfree(copypath);
  if (result) {
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    return result;
  }

  /* Create a new address space. */
  asnew = as_create();
  if (asnew == NULL) {
    vfs_close(v);
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    return ENOMEM;
  }
  /* Switch to it and activate it. */
  asold = curproc_getas();
  curproc_setas(asnew);
  as_activate();

  /* Load the executable. */
  result = load_elf(v, &entrypoint);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    vfs_close(v);
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    return result;
  }
  /* Done with the file now. */
  vfs_close(v);

  // copy arguments into new addr space////////////////////////////////////////
  /* Define the user stack in the address space */
  result = as_define_stack(asnew, &stackptr);
  if (result) {
    // free
    for(unsigned long i = 0; i < num; i++){
      kfree(copyargs[i]);
    }
    kfree(copyargs);
    /* p_addrspace will go away when curproc is destroyed */
    return result;
  }

  // copy args to user
  for(unsigned long i = 0; i < num; i++){
    len = strlen(copyargs[i]) + 1;
    stackptr = stackptr - ROUNDUP(len, 8);//? limit or valid stackptr
    result = copyoutstr(copyargs[i], (userptr_t)stackptr, len, NULL);
    if(result){
      // free
      for(unsigned long j = i; j < num; j++){
        kfree(copyargs[j]);
      }
      kfree(copyargs);
      return result;
    }
    kfree(copyargs[i]);
    copyargs[i] = (char *)stackptr;
  }

  // copy arr
  len = sizeof(char *) * (num + 1);
  stackptr = stackptr - ROUNDUP(len, 8);
  result = copyout(copyargs, (userptr_t)stackptr, len);
  if(result){
    kfree(copyargs);
    return result;
  }
  kfree(copyargs);

  userptr_t stackk = (userptr_t)stackptr;

  // Delete old address space
  as_destroy(asold);

  // Call enter_new_process
  /* Warp to user mode. */
  enter_new_process(num /*argc*/, stackk /*userspace addr of argv*/,
        stackptr, entrypoint);
  
  /* enter_new_process does not return. */
  panic("enter_new_process returned\n");
  return EINVAL;

}

#endif
