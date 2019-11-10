#include <types.h>
#include <syscall.h>
#include <pid.h>
#include <proc.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <file.h>
#include <thread.h>
#include <kern/errno.h>
#include <current.h>
#include <limits.h>
#include <kern/wait.h>
#include <copyinout.h>
#include <vfs.h>
#include <kern/fcntl.h>

struct trapframe;

/* Fork a child process, copy filetable, address space, and call thread_fork,
* return 0 if child, pid if from the process called fork 
*/
int sys_fork(struct trapframe *tf, pid_t *retval)
{
    *retval = -1;
    int res;
    struct proc *new_proc = proc_create_runprogram("new process");
    if (new_proc == NULL)
    {
        return ENPROC;
    }
    struct trapframe *new_tf;

    res = as_copy(proc_getas(), &new_proc->p_addrspace);
    if (res)
    {
        proc_destroy(new_proc);
        return res;
    }

    res = filetable_copy(&(new_proc->p_filetable));
    if (res)
    {
        proc_destroy(new_proc);
        return res;
    }

    new_tf = kmalloc(sizeof(struct trapframe));
    if (new_tf == NULL)
    {
        proc_destroy(new_proc);
        return ENOMEM;
    }
    /* copy trapframe */
    *new_tf = *tf;

    lock_acquire(curproc->p_child_lock);
    for (int i = 0; i < PID_MAX; i++)
    {
        if (curproc->p_children[i] == NULL)
        {
            curproc->p_children[i] = &(new_proc->p_pid);
            break;
        }
    }
    lock_release(curproc->p_child_lock);

    res = thread_fork("new thread", new_proc, &enter_forked_process, new_tf, 0);
    if (res)
    {
        proc_destroy(new_proc);
        return res;
    }

    *retval = new_proc->p_pid;
    if (retval == NULL)
    {
        proc_destroy(new_proc);
        return ENPROC;
    }
    return 0;
}

/* get the pid of the current process */
int sys_getpid(pid_t *retval)
{
    *retval = curproc->p_pid;
    return 0;
}

/* wait for a process to exit */
int sys_waitpid(pid_t pid, int *status, int options, pid_t *retval)
{
    *retval = -1;
    if (options != 0)
    {
        return EINVAL;
    }
    /* pid not in range */
    if (pid <= 0 || pid > PID_MAX)
    {
        return ESRCH;
    }

    struct pid *cur_pid = pid_get(pid);
    lock_acquire(cur_pid->pid_lock);
    if (cur_pid == NULL)
    {
        lock_release(cur_pid->pid_lock);
        return ESRCH;
    }
    bool isChild = false;
    
    /* menu process */
    if(pid == 2) {
        isChild = true;
    }
    for (int i = 0; i < PID_MAX; i++)
    {
        if (curproc->p_children[i] == NULL)
        {
            break;
        }
        if (*(curproc->p_children[i]) == pid)
        {
            isChild = true;
            break;
        }
    }
    if (!isChild)
    {
        lock_release(cur_pid->pid_lock);
        return ECHILD;
    }

    /* wait for the pid to exit, if it has exited, return the status and pid */
    if (cur_pid->exited == false)
    {
        cv_wait(cur_pid->pid_cv, cur_pid->pid_lock);
    }

    /* assign status if not status is not NULL, decode in _exit and save in exitstatus */
    if (status != NULL)
    {
        int res = copyout((const void *)&(cur_pid->exit_status), (userptr_t)status, sizeof(cur_pid->exit_status));
        if (res)
        {
            return res;
        }
    }

    *retval = pid;
    lock_release(cur_pid->pid_lock);
    pid_destroy(pid);
    return 0;
}

void sys__exit(int exitcode)
{
    struct pid *pid = pid_get(curproc->p_pid);

    lock_acquire(pid->pid_lock);
    pid_t t_pid = -1;
    exitcode = _MKWAIT_EXIT(exitcode);
    pid->exited = true;
    pid->exit_status = exitcode;
    int i;
    for (i = 0; i < PID_MAX; i++)
    {
        if (curproc->p_children[i] != NULL)
        {
            sys_waitpid(*(curproc->p_children[i]), &exitcode, 0, &t_pid);
            curproc->p_children[i] = NULL;
        }
    }
    cv_signal(pid->pid_cv, pid->pid_lock);
    if (pid->pid_lock != NULL)
    {
        lock_release(pid->pid_lock);
    }
    sys_exit_helper();
}

int
sys_execv(const char *program, char **args) {

    // Copy the arguments from the old address space
    if(program == NULL || args == NULL){
        return EFAULT;
    }

    if(strlen((char *)program) > PATH_MAX){
        return E2BIG;
    }

    int res;
    char *dest;
    dest = kstrdup((char *) program);
    if (dest == NULL) {
        return ENOMEM;
    }

    if(dest[0] == '\0'){
        kfree(dest);
        return EINVAL;
    }

    int argc = 0;
    while(args[argc] != NULL){
        argc++;
    }
    
    char **argv = kmalloc(sizeof(char *) * (argc + 1));
    if(argv == NULL){
        return ENOMEM;
    }
    int i;
    for(i = 0; i <= argc; i++){
        argv[i] = NULL;
    }
    
    for(i = 0; i < argc; i++){
        size_t len = sizeof(char)*(strlen(args[i]) + 1); // Plus the NULL terminator
        argv[i] = kmalloc(len); 
        res = copyinstr((const_userptr_t) args[i], argv[i], len, NULL);
        if (res) {
            for(i = 0; i < argc; i++){
                kfree(argv[i]);
            }
            kfree(argv);
            return res;
        }
    }

    // Get a new address space
    struct addrspace *as = as_create();
    if (as == NULL) {
        for(i = 0; i < argc; i++){
            kfree(argv[i]);
        }
        kfree(argv);
        return ENOMEM;
    }

    // Switch to the new address space
    as_deactivate();
    struct addrspace *old_as = proc_setas(as);
    as_activate();

    // Load a new executable
    struct vnode *vn;
    res = vfs_open(dest, O_RDONLY, 0, &vn);
    if(res){
        for(i = 0; i < argc; i++){
            kfree(argv[i]);
        }
        kfree(argv);
        as_deactivate();
        as = proc_setas(old_as);
        as_destroy(as);
        return EFAULT;
    }
    vaddr_t entrypoint;
    res = load_elf(vn, &entrypoint);
    if(res){
        for(i = 0; i < argc; i++){
            kfree(argv[i]);
        }
        kfree(argv);
        as_deactivate();
        as = proc_setas(old_as);
        as_destroy(as);
        vfs_close(vn);
        return ENOMEM;
    }
    kfree(dest);
    vfs_close(vn);

    // Define a new stack region
    vaddr_t stackptr;
    res = as_define_stack(as, &stackptr);
    if(res){
        for(i = 0; i < argc; i++){
            kfree(argv[i]);
        }
        kfree(argv);
        as_deactivate();
        as = proc_setas(old_as);
        as_destroy(as);
        return ENOMEM;
    }
    size_t len;

    // Copy the arguments to the new address space, properly arranging them
    vaddr_t *userargvptr = kmalloc(sizeof(vaddr_t) * (argc + 1));
    userargvptr[argc] = 0; // NULL
    size_t aligned;

    for(i = argc - 1; i >= 0; i--){
        len = sizeof(char) * (strlen(argv[i]) + 1); // Plus null terminator '\0'
        aligned = ROUNDUP(len,4);
        stackptr -= aligned;
        userargvptr[i] = stackptr;
        res = copyoutstr(argv[i], (userptr_t) stackptr, aligned, &len); // error check
        if(res){
            for(i = 0; i < argc; i++){
                kfree(argv[i]);
            }
            kfree(argv);
            kfree(userargvptr);
            as_deactivate();
            as = proc_setas(old_as);
            as_destroy(as);
            return res;
        }
    }
    len = sizeof(vaddr_t);

    for(i = argc; i >= 0; i--){
        stackptr -= len;
        copyout(&userargvptr[i], (userptr_t) stackptr, len);
        if(res){
            for(i = 0; i < argc; i++){
                kfree(argv[i]);
            }
            kfree(argv);
            kfree(userargvptr);
            as_deactivate();
            as = proc_setas(old_as);
            as_destroy(as);
            return res;
        }
    }

    vaddr_t userspaceAddr = stackptr;

    // Clean up the old address space, 
    for(i = 0; i < argc; i++){
        kfree(argv[i]);
    }
    kfree(argv);
    kfree(userargvptr);
    as_destroy(old_as);

    // Warp to user mode
    enter_new_process(argc, (userptr_t) userspaceAddr, NULL, (vaddr_t) stackptr, entrypoint);
    
    panic("Somehow returned from enter_new_process.\n");

    return -1;
}
