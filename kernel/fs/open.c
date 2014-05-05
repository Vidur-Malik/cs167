/*
 *  FILE: open.c
 *  AUTH: mcc | jal
 *  DESC:
 *  DATE: Mon Apr  6 19:27:49 1998
 */

#include "globals.h"
#include "errno.h"
#include "fs/fcntl.h"
#include "util/string.h"
#include "util/printf.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/file.h"
#include "fs/vfs_syscall.h"
#include "fs/open.h"
#include "fs/stat.h"
#include "util/debug.h"

/* find empty index in p->p_files[] */
int
get_empty_fd(proc_t *p)
{
        int fd;

        for (fd = 0; fd < NFILES; fd++) {
                if (!p->p_files[fd])
                        return fd;
        }

        dbg(DBG_ERROR | DBG_VFS, "ERROR: get_empty_fd: out of file descriptors "
            "for pid %d\n", curproc->p_pid);
        return -EMFILE;
}

/*
 * There a number of steps to opening a file:
 *      1. Get the next empty file descriptor.
 *      2. Call fget to get a fresh file_t.
 *      3. Save the file_t in curproc's file descriptor table.
 *      4. Set file_t->f_mode to OR of FMODE_(READ|WRITE|APPEND) based on
 *         oflags, which can be O_RDONLY, O_WRONLY or O_RDWR, possibly OR'd with
 *         O_APPEND.
 *      5. Use open_namev() to get the vnode for the file_t.
 *      6. Fill in the fields of the file_t.
 *      7. Return new fd.
 *
 * If anything goes wrong at any point (specifically if the call to open_namev
 * fails), be sure to remove the fd from curproc, fput the file_t and return an
 * error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        oflags is not valid.
 *      o EMFILE
 *        The process already has the maximum number of files open.
 *      o ENOMEM
 *        Insufficient kernel memory was available.
 *      o ENAMETOOLONG
 *        A component of filename was too long.
 *      o ENOENT
 *        O_CREAT is not set and the named file does not exist.  Or, a
 *        directory component in pathname does not exist.
 *      o EISDIR
 *        pathname refers to a directory and the access requested involved
 *        writing (that is, O_WRONLY or O_RDWR is set).
 *      o ENXIO
 *        pathname refers to a device special file and no corresponding device
 *        exists.
 */

int
do_open(const char *filename, int oflags)
{   
    int cur_fd;
    /* Get next empty file descriptor */
    if( (cur_fd = get_empty_fd( curproc )) < 0 ) {
        /* errno = EMFILE; */
        return -EMFILE;
    }
    /* Get fresh file by calling fget */
    file_t *cur_file;
    if( (cur_file = fget( -1 )) == NULL ) {
        return -ENOMEM; /* No more kernel mem available for a new file */
    }
    /* Save the file in the current process's fd table*/
    curproc->p_files[cur_fd] = cur_file; /* Set the pointer */
    
    /* Check the flags, erroring out if necessary */
    
    if( oflags & O_WRONLY ) {
        cur_file->f_mode = FMODE_WRITE;
        if( oflags & O_RDWR )
            goto error;
    }
    else if( oflags & O_RDWR ) 
        cur_file->f_mode = (FMODE_READ | FMODE_WRITE);

    else if( (oflags & O_RDONLY) == 0 ) {
        cur_file->f_mode = FMODE_READ; 
        if( oflags & O_RDWR )
            goto error;
    }
    if( oflags & O_APPEND )
        cur_file->f_mode |= FMODE_APPEND;

    /* Now, get the vnode associated with the filename */
    vnode_t *cur_vnode;
    int ret = open_namev( filename, oflags, &cur_vnode, NULL );

    /* Set the fields of cur_file */
    if( ret < 0 ) {
        /* Clean up the created file */
        curproc->p_files[cur_fd] = NULL;
        fput( cur_file ); 
        return ret; /* Will check for ENAMETOOLONG, ENOENT */
    }
    /* Check the cur_vnode for errors */
    /* If the opened vnode is a dir and we have write permission */
    if( (cur_vnode->vn_mode & S_IFDIR) && (oflags & O_WRONLY || oflags & O_RDWR) ) {
        curproc->p_files[cur_fd] = NULL;
        fput( cur_file );
        vput( cur_vnode );
        return -EISDIR;
    }
    /* If the opened vnode is a device special file and no device exists */
    if( (cur_vnode->vn_mode & S_IFCHR && !( cur_vnode->vn_cdev )) ||
        (cur_vnode->vn_mode & S_IFBLK && !( cur_vnode->vn_bdev )) ) {
        curproc->p_files[cur_fd] = NULL;
        fput( cur_file );
        vput( cur_vnode );
        return -ENXIO;
    }

    facq( cur_file, cur_vnode ); /* Set the vnode of this file */

    return cur_fd;

error:
    curproc->p_files[cur_fd] = NULL;
    fput( cur_file );
    return -EINVAL;
}
