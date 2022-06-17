#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <kern/errno.h>
#if OPT_IO
#include <vnode.h>
#include <vfs.h>
#include <current.h>
#include <copyinout.h>
#include <uio.h>

#define USE_KBUFFER 1
#define SYSTEM_OPENFILE_MAX (16*OPEN_MAX)
#endif

#if OPT_IO
// should be used with spinlocks
struct openfile openfile_table[SYSTEM_OPENFILE_MAX];
#endif

#if OPT_SYSCALLS
int sys_write(int fd, userptr_t buf_ptr, size_t size){
    int i;
    char *p = (char *)buf_ptr;

    if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
        #if OPT_IO
        return file_write(fd, buf_ptr, size);
        #else
        kprintf("sys_write: invalid file descriptor\n");
        return -1;
        #endif
    }

    for (i=0; i<(int)size; i++) {
        putch(p[i]);
    }

    return size;
}

int sys_read(int fd, userptr_t buf_ptr, size_t size){
    int i;
    char *p = (char *)buf_ptr;

    if (fd!=STDIN_FILENO) {
        #if OPT_IO
        return file_read(fd, buf_ptr, size);
        #else
        kprintf("sys_read: invalid file descriptor\n");
        return -1;
        #endif
    }

    for (i=0; i<(int)size; i++) {
        p[i] = getch();
        if (p[i]<0){
            return i;
        }
    }

    return size;
}
#endif /* OPT_SYSCALLS */

#if OPT_IO

size_t file_write(int fd, userptr_t buf_ptr, size_t size){
    struct iovec iov; // input/output vector
    struct uio u; // user input output
    struct vnode *vn;
    struct openfile *of;
    struct proc *cp = curproc;
    #if USE_KBUFFER
    void *kbuf =  kmalloc(size);
    #endif
    of = cp->p_filetable[fd];
    if (of==NULL) {
        return -1;
    }
    vn = of->of_vn;
    if (vn==NULL) {
        return -1;
    }

    #if USE_KBUFFER
    kbuf =  kmalloc(size); // create kernel buffer 
    copyin(buf_ptr, kbuf, size); // copy user data to kernel buffer
    uio_kinit(&iov, &u, (void *)kbuf, size, of->of_offset, UIO_WRITE); // copy kernel buffer to final file
    #else 
    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_offset = of->of_offset;
    u.uio_resid= size;
    u.uio_rw = UIO_WRITE;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_space = cp->p_addrspace;
    #endif

    if (VOP_WRITE(vn, &u)) {
        #if USE_KBUFFER
        kfree(kbuf);
        #endif
        return -1;
    }

    #if USE_KBUFFER
    kfree(kbuf);
    #endif

    of->of_offset = u.uio_offset;
    return size - u.uio_resid;
}

size_t file_read(int fd, userptr_t buf_ptr, size_t size){
    /*
    iovec:
        - iov_base: pointer to buffer for data to manage in the IO
        - iov_len: length of the data buffer
        could have more than one operations
        what is the buffer? example reading elf program header:
            - iov_base: pointer to program header
            - iov_len: sizeof(elf header)
    */
    struct iovec iov; // input/output vector
    /*
    uio:
        - *uio_iov: pointer to the iovec structure that contains the data to be read/written
        - uio_iovcnt: count of the iovec structures used
        - uio_offset: offset in the file to start reading/writing
        - uio_resid: amount of data to read/write
        - uio_segflg: specify the type of pointer (User code / User data / Kernel )
        - uio_rw: specify the type of operation (read / write)
        - uio_space: specify the address space to be used in case of user pointer 
    */
    struct uio u; // user input output
    struct vnode *vn;
    struct openfile *of;
    struct proc *cp = curproc;
    #if USE_KBUFFER
    void *kbuf;
    #endif

    of = cp->p_filetable[fd];
    if (of==NULL) {
        return -1;
    }
    vn = of->of_vn;
    if (vn==NULL) {
        return -1;
    }
    iov.iov_ubase = buf_ptr;
    iov.iov_len = size;

    #if USE_KBUFFER
    kbuf = kmalloc(size);
    uio_kinit(&iov, &u, kbuf, size, of->of_offset, UIO_READ);
    #else

    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_offset = of->of_offset;
    u.uio_resid = size;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_rw = UIO_READ;
    u.uio_space = cp->p_addrspace;

    #endif

    if (VOP_READ(vn, &u)){
        kfree(kbuf);
        return -1;
    }
    
    #if USE_KBUFFER
    copyout(kbuf, buf_ptr, size-u.uio_resid);
    kfree(kbuf);
    #endif

    of->of_offset = u.uio_offset;
    return size;
}

int sys_open(userptr_t filename, int flags, mode_t mode, size_t *ret_fd){
    struct openfile *of;
    struct vnode *vn = NULL;	
    int result, fd, i;
    int filetable_index = -1;
    for (i = 0; i < SYSTEM_OPENFILE_MAX; i++) {
        if (openfile_table[i].of_vn == NULL) {
            filetable_index = i;
            break;
        }
    }
    if (filetable_index == -1) {
        return ENFILE;
    }
    if (filename == NULL) {
        return -1;
    }
    result = vfs_open((char *)filename, flags, mode, &vn);
    if (result || vn==NULL) {
        return ENOENT;
    }
    of = &(openfile_table[filetable_index]);
    of->of_vn = vn;
    of->of_offset = 0;
    of->of_ref_count = 1;
    for (fd = STDERR_FILENO+1; fd < OPEN_MAX; fd++) {
        if (curproc->p_filetable[fd] == NULL) {
            curproc->p_filetable[fd] = of;
            *ret_fd = fd;
            return 0;
        }
    }
    vfs_close(vn);
    return EMFILE;
}

int sys_close(int filehandle){
    /*
        * Close the file associated with filehandle.
        * Return 0 on success, or an error number on failure.
        * Note that the filehandle is not an index into the filetable,
        * but rather a filehandle returned by open and referrent to the current process.
        * The filehandle is not valid after close returns.
    */
    if (filehandle < 0 || filehandle >= OPEN_MAX) {
        return EBADF;
    }
    if (curproc->p_filetable[filehandle] == NULL) {
        return EBADF;
    }
    if (curproc->p_filetable[filehandle]->of_ref_count == 0) {
        return EBADF;
    }
    curproc->p_filetable[filehandle]->of_ref_count--;
    if (curproc->p_filetable[filehandle]->of_ref_count == 0) {
        vfs_close(curproc->p_filetable[filehandle]->of_vn);
        curproc->p_filetable[filehandle]->of_vn = NULL;
    }
    curproc->p_filetable[filehandle] = NULL;
    return 0;
}

#endif /* OPT_IO */