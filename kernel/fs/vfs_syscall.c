/*
 *  FILE: vfs_syscall.c
 *  AUTH: mcc | jal
 *  DESC:
 *  DATE: Wed Apr  8 02:46:19 1998
 *  $Id: vfs_syscall.c,v 1.9.2.2 2006/06/04 01:02:32 afenn Exp $
 */

#include "kernel.h"
#include "errno.h"
#include "globals.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/open.h"
#include "fs/fcntl.h"
#include "fs/lseek.h"
#include "mm/kmalloc.h"
#include "util/string.h"
#include "util/printf.h"
#include "fs/stat.h"
#include "util/debug.h"

/* To read a file:
 *      o fget(fd)
 *      o call its virtual read f_op
 *      o update f_pos
 *      o fput() it
 *      o return the number of bytes read, or an error
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for reading.
 *      o EISDIR
 *        fd refers to a directory.
 *
 * In all cases, be sure you do not leak file refcounts by returning before
 * you fput() a file that you fget()'ed.
 */
int
do_read(int fd, void *buf, size_t nbytes)
{       
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( (cur_file = fget( fd )) == NULL) {
                /* errno = EBADF */
                return -EBADF;
        }
        /* Check if the file is actually a directory */
        if( cur_file->f_vnode && (cur_file->f_vnode->vn_mode & 0x200) ) {
                fput( cur_file );
                return -EISDIR;
        }
        /* Check to see if fd is open for reading */
        if( !(cur_file->f_mode & FMODE_READ) ) {
                /* errno = EBADF */
                fput( cur_file );
                return -EBADF;
        }

        /* Call the specific read func associated with the FS */
        int b_read = cur_file->f_vnode->vn_ops->read( cur_file->f_vnode, 
                                                cur_file->f_pos, buf, nbytes );
        /* Update the position in the file by the amount of bytes read */
        if( b_read >= 0 )
            cur_file->f_pos += b_read;

        fput( cur_file );

        return b_read;
}

/* Very similar to do_read.  Check f_mode to be sure the file is writable.  If
 * f_mode & FMODE_APPEND, do_lseek() to the end of the file, call the write
 * f_op, and fput the file.  As always, be mindful of refcount leaks.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for writing.
 */
int
do_write(int fd, const void *buf, size_t nbytes)
{
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( (cur_file = fget( fd )) == NULL) {
                /* errno = EBADF */
                return -EBADF;
        }
       
        /* Check to see if fd is open for writing */
        if( !(cur_file->f_mode & FMODE_WRITE) ) {
                /* errno = EBADF */
                fput( cur_file );
                return -EBADF;
        }
        int ret;
        /* Check to see if fd is in appending mode */
        if( cur_file->f_mode & FMODE_APPEND ) {
                if( (ret = do_lseek( fd, 0, SEEK_END )) < 0 ) {
                    fput(cur_file);
                    return ret; /* Move to the end of the file */
                }
        }
        /* Call the specific read func associated with the FS */
        int b_written = cur_file->f_vnode->vn_ops->write( cur_file->f_vnode, 
                                                cur_file->f_pos, buf, nbytes );
        /* Update the position in the file by the amount of bytes read */
        if( b_written > 0 )
            cur_file->f_pos += b_written;
        
        fput( cur_file );

        return b_written;
}

/*
 * Zero curproc->p_files[fd], and fput() the file. Return 0 on success
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't a valid open file descriptor.
 */
int
do_close(int fd)
{       
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( fd < 0 || (cur_file = fget( fd )) == NULL ) {
                /* errno = EBADF */
                return -EBADF;
        }
        
        /* Set the pointer to the open file to NULL */
        curproc->p_files[fd] = NULL;
        /* Decrement reference count twice ??*/
        fput( cur_file );
        if( cur_file->f_refcount > 0 )
             fput( cur_file ); /* Once for the fget in this function */
        /* fput( cur_file );  Once for actually closing */

        return 0;
}

/* To dup a file:
 *      o fget(fd) to up fd's refcount
 *      o get_empty_fd()
 *      o point the new fd to the same file_t* as the given fd
 *      o return the new file descriptor
 *
 * Don't fput() the fd unless something goes wrong.  Since we are creating
 * another reference to the file_t*, we want to up the refcount.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't an open file descriptor.
 *      o EMFILE
 *        The process already has the maximum number of file descriptors open
 *        and tried to open a new one.
 */
int
do_dup(int fd)
{
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( (cur_file = fget( fd )) == NULL) {
                /* errno = EBADF */
                return -EBADF;
        }

        if( !(cur_file->f_vnode) ) {
            fput( cur_file );
            return -EBADF;
        }

        /* Get another file descriptor, erroring out if max fds are open */
        int new_fd = get_empty_fd( curproc );
        if( new_fd < 0 ) {
                /* errno = EMFILE */
                fput( cur_file );
                return -EMFILE;
        }
        /* Point the new fd to cur_file */
        curproc->p_files[new_fd] = cur_file;

        return new_fd;
}

/* Same as do_dup, but insted of using get_empty_fd() to get the new fd,
 * they give it to us in 'nfd'.  If nfd is in use (and not the same as ofd)
 * do_close() it first.  Then return the new file descriptor.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        ofd isn't an open file descriptor, or nfd is out of the allowed
 *        range for file descriptors.
 */
int
do_dup2(int ofd, int nfd)
{
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( (cur_file = fget( ofd )) == NULL ) {
                /* errno = EBADF */
                return -EBADF;
        }
        if( !(cur_file->f_vnode) ) {
            fput( cur_file );
            return -EBADF;
        }
        /* Check for errors in nfd */
       if( nfd < 0 || nfd >= NFILES ) {
                /* errno = EBADF */
                fput( cur_file );
                return -EBADF;
       }
        /* If both fds are valid and are equal, do nothing and return nfd */
       /* DO FPUT HERE?? */
        if( ofd == nfd ) {
                fput( cur_file );
                return nfd;
        }
        /* Check if nfd is in use, close if necessary */
        file_t *temp;
        if( (temp = fget( nfd )) != NULL ) {
                fput( temp );
                do_close( nfd );
        }

        /* Point the new fd to cur_file */
        curproc->p_files[nfd] = cur_file;

        return nfd;
}

/*
 * This routine creates a special file of the type specified by 'mode' at
 * the location specified by 'path'. 'mode' should be one of S_IFCHR or
 * S_IFBLK (you might note that mknod(2) normally allows one to create
 * regular files as well-- for simplicity this is not the case in Weenix).
 * 'devid', as you might expect, is the device identifier of the device
 * that the new special file should represent.
 *
 * You might use a combination of dir_namev, lookup, and the fs-specific
 * mknod (that is, the containing directory's 'mknod' vnode operation).
 * Return the result of the fs-specific mknod, or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        mode requested creation of something other than a device special
 *        file.
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_mknod(const char *path, int mode, unsigned devid)
{
        /* Check mode for errors */
        if( mode != S_IFCHR && mode != S_IFBLK ) {
                /* errno = EINVAL */
                return -EINVAL;
        }
        /* Check if directory exists by calling dir_namev */
        vnode_t *res_node;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret;
        if( (ret = dir_namev( path, &namelen, &name, NULL, &res_node )) < 0 ) {
                return ret; /* Takes care of ENAMETOOLONG, ENOENT, ENOTDIR */
        }
        vnode_t *path_node;
        /* Now, check if the file we are trying to create already exists */
        if( lookup( res_node, name, namelen, &path_node ) == 0 ) {
                /* errno = EEXIST */
                /* Decrement ref counts for both parent and child */
                vput( res_node );
                vput( path_node );
                return -EEXIST;
        }
        /* Do we have decrement ref count for parent here?? probably */
        ret = res_node->vn_ops->mknod( res_node, name, namelen, mode, devid );
        vput( res_node );
        return ret;
}

/* Use dir_namev() to find the vnode of the dir we want to make the new
 * directory in.  Then use lookup() to make sure it doesn't already exist.
 * Finally call the dir's mkdir vn_ops. Return what it returns.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_mkdir(const char *path)
{
        /* Check if directory exists by calling dir_namev */
        vnode_t *res_node;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret;

        if( (ret = dir_namev( path, &namelen, &name, NULL, &res_node )) < 0 ) 
                return ret; /* Takes care of ENAMETOOLONG, ENOENT, ENOTDIR */
        vnode_t *path_node;
        /* Now, check if the file we are trying to create already exists */
        if( lookup( res_node, name, namelen, &path_node ) == 0 ) {
                vput( res_node );
                vput( path_node );
                return -EEXIST;
        }
        ret = res_node->vn_ops->mkdir( res_node, name, namelen );
        vput( res_node );
        return ret;
}

/* Use dir_namev() to find the vnode of the directory containing the dir to be
 * removed. Then call the containing dir's rmdir v_op.  The rmdir v_op will
 * return an error if the dir to be removed does not exist or is not empty, so
 * you don't need to worry about that here. Return the value of the v_op,
 * or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        path has "." as its final component.
 *      o ENOTEMPTY
 *        path has ".." as its final component.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_rmdir(const char *path)
{       
        vnode_t *res_node;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret;
        if( (ret = dir_namev( path, &namelen, &name, NULL, &res_node )) < 0 ) 
                return ret; /* Takes care of ENAMETOOLONG, ENOENT, ENOTDIR */
        /* Check other error condition */
        if( strcmp( name, ".") == 0 ) {
                vput(res_node);
                return -EINVAL;
        }
        if( strcmp( name, "..") == 0 ) {
                vput(res_node);
                return -ENOTEMPTY;
        }
        ret = res_node->vn_ops->rmdir( res_node, name, namelen );
        vput( res_node );
        return ret;
}

/*
 * Same as do_rmdir, but for files.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EISDIR
 *        path refers to a directory.
 *      o ENOENT
 *        A component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_unlink(const char *path)
{
        vnode_t *res_node;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret;
        if( (ret = dir_namev( path, &namelen, &name, NULL, &res_node )) < 0 ) 
                return ret; /* Takes care of ENAMETOOLONG, ENOENT, ENOTDIR */
        /* Check if the name is actually a dir */
        vnode_t *path_node;
        /* Now, check if the file we're trying to delete is actually a dir */
        if( lookup( res_node, name, namelen, &path_node ) == 0 ) {
                if( path_node->vn_mode & S_IFDIR ) {
                        vput( res_node );
                        vput( path_node );
                        return -EPERM;
                }
                /* ?? */
                vput( path_node );
        }
        else {
            vput( res_node );
            return -ENOENT;
        }
        /* ?? */
        ret = res_node->vn_ops->unlink( res_node, name, namelen ); 
        vput( res_node );
        return ret;
}

/* To link:
 *      o open_namev(from)
 *      o dir_namev(to)
 *      o call the destination dir's (to) link vn_ops.
 *      o return the result of link, or an error
 *
 * Remember to vput the vnodes returned from open_namev and dir_namev.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        to already exists.
 *      o ENOENT
 *        A directory component in from or to does not exist.
 *      o ENOTDIR
 *        A component used as a directory in from or to is not, in fact, a
 *        directory.
 *      o ENAMETOOLONG
 *        A component of from or to was too long.
 */
int
do_link(const char *from, const char *to)
{       
        vnode_t *res_node_from;
        /* Open the directory where we are linking from */
        if( open_namev( from, O_CREAT, &res_node_from, NULL ) < 0 )
                return -ENOENT;

        vnode_t *res_node_to;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret;
        /* Check and get the vnode of the dir we are trying to link to */
        if( (ret = dir_namev( to, &namelen, &name, NULL, &res_node_to )) < 0 ) {
                vput( res_node_from );
                return ret; /* Takes care of ENAMETOOLONG, ENOENT, ENOTDIR */
        }

        vnode_t *path_node;
        /* Now, check if the file we are trying to link to already exists */
        if( lookup( res_node_to, name, namelen, &path_node ) == 0 ) {
                vput( res_node_to );
                vput( res_node_from );
                return -EEXIST;
        }

        ret = res_node_to->vn_ops->link( res_node_from, res_node_to, name, namelen );
        /* Call the link function */
        vput( res_node_from );
        vput( res_node_to ); 
        return ret;
}

/*      o link newname to oldname
 *      o unlink oldname
 *      o return the value of unlink, or an error
 *
 * Note that this does not provide the same behavior as the
 * Linux system call (if unlink fails then two links to the
 * file could exist).
 */
int
do_rename(const char *oldname, const char *newname)
{       
        int link_res;
        if( (link_res = do_link( oldname, newname )) < 0 )
                return link_res;

        return do_unlink( oldname );
}

/* Make the named directory the current process's cwd (current working
 * directory).  Don't forget to down the refcount to the old cwd (vput()) and
 * up the refcount to the new cwd (open_namev() or vget()). Return 0 on
 * success.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        path does not exist.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 *      o ENOTDIR
 *        A component of path is not a directory.
 */
int
do_chdir(const char *path)
{
        vnode_t *res_node;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret; 
        if( (ret = open_namev( path, 0, &res_node, NULL )) < 0 )
                return ret; /* Takes care of ENAMETOOLONG, ENOENT, ENOTDIR */
        if( !(res_node->vn_mode & S_IFDIR) ) {
                vput( res_node );
                return -ENOTDIR;
        }
        /* Decrement the refcount of the old cwd */
        vput( curproc->p_cwd );
        /* Set the new cwd */
        curproc->p_cwd = res_node; /* will this res_node be popped off after return?? */

        return 0;
}

/* Call the readdir f_op on the given fd, filling in the given dirent_t*.
 * If the readdir f_op is successful, it will return a positive value which
 * is the number of bytes copied to the dirent_t.  You need to increment the
 * file_t's f_pos by this amount.  As always, be aware of refcounts, check
 * the return value of the fget and the virtual function, and be sure the
 * virtual function exists (is not null) before calling it.
 *
 * Return either 0 or sizeof(dirent_t), or -errno.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        Invalid file descriptor fd.
 *      o ENOTDIR
 *        File descriptor does not refer to a directory.
 */
int
do_getdent(int fd, struct dirent *dirp)
{
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( (cur_file = fget( fd )) == NULL) 
                return -EBADF;
        if( cur_file->f_vnode == NULL ) {
                fput( cur_file );
                return -EBADF;
        }

        /* Check if the vnode is not a dir */
        if( !(cur_file->f_vnode->vn_mode & S_IFDIR) ) {
                fput( cur_file );
                return -ENOTDIR;
        }

        /* Check if the function is null */
        if( !cur_file->f_vnode->vn_ops->readdir ) {
                fput( cur_file );
                return -ENOTDIR;
        }

        int bytes_copied = cur_file->f_vnode->vn_ops->readdir( cur_file->f_vnode,
                                                        cur_file->f_pos, dirp );
        /* Increment f_pos */
        cur_file->f_pos += bytes_copied;
        /* Decrement refcount */
        fput( cur_file );
        /* Check bytes copied */

        if( bytes_copied < 0)
                return bytes_copied;
        else if( !bytes_copied )
                return 0;
        else
                return sizeof((*dirp));

}

/*
 * Modify f_pos according to offset and whence.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not an open file descriptor.
 *      o EINVAL
 *        whence is not one of SEEK_SET, SEEK_CUR, SEEK_END; or the resulting
 *        file offset would be negative.
 */
int
do_lseek(int fd, int offset, int whence)
{
        file_t *cur_file;
        /* Get the file associated with the input fd */
        if( (cur_file = fget( fd )) == NULL) 
                return -EBADF;
        if( !(cur_file->f_vnode) ) {
            fput( cur_file );
            return -EBADF;
        }
        switch( whence ) {
                case SEEK_SET:
                        if( offset < 0 ) {
                                fput( cur_file );
                                return -EINVAL;
                        }
                        cur_file->f_pos = offset;
                        break;
                case SEEK_END:
                        if( offset + cur_file->f_vnode->vn_len < 0 ) {
                                fput( cur_file );
                                return -EINVAL;
                        }
                        cur_file->f_pos = ( offset + cur_file->f_vnode->vn_len );
                        break;
                case SEEK_CUR:
                        if( offset + cur_file->f_pos < 0 ) {
                                fput( cur_file );
                                return -EINVAL;
                        }
                        cur_file->f_pos += offset;
                        break;
                default:
                        fput( cur_file );
                        return -EINVAL;

        }
        int ret = cur_file->f_pos;
        fput( cur_file );
        return ret;
}

/*
 * Find the vnode associated with the path, and call the stat() vnode operation.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        A component of path does not exist.
 *      o ENOTDIR
 *        A component of the path prefix of path is not a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_stat(const char *path, struct stat *buf)
{
        vnode_t *res_node;
        if( path[0] == '\0' )
            return -EINVAL;
        char name_buf[256]; const char *name = name_buf; size_t namelen; int ret;
        if( (ret = open_namev( path, 0, &res_node, NULL )) < 0 )
            return ret; 

        if( res_node->vn_ops->stat) {
            ret =  res_node->vn_ops->stat( res_node, buf );
            vput( res_node );
            return ret;
        }
        else {
            vput( res_node );
            return -1;
        }
}

#ifdef __MOUNTING__
/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutely sure your Weenix is perfect.
 *
 * This is the syscall entry point into vfs for mounting. You will need to
 * create the fs_t struct and populate its fs_dev and fs_type fields before
 * calling vfs's mountfunc(). mountfunc() will use the fields you populated
 * in order to determine which underlying filesystem's mount function should
 * be run, then it will finish setting up the fs_t struct. At this point you
 * have a fully functioning file system, however it is not mounted on the
 * virtual file system, you will need to call vfs_mount to do this.
 *
 * There are lots of things which can go wrong here. Make sure you have good
 * error handling. Remember the fs_dev and fs_type buffers have limited size
 * so you should not write arbitrary length strings to them.
 */
int
do_mount(const char *source, const char *target, const char *type)
{
        NOT_YET_IMPLEMENTED("MOUNTING: do_mount");
        return -EINVAL;
}

/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutley sure your Weenix is perfect.
 *
 * This function delegates all of the real work to vfs_umount. You should not worry
 * about freeing the fs_t struct here, that is done in vfs_umount. All this function
 * does is figure out which file system to pass to vfs_umount and do good error
 * checking.
 */
int
do_umount(const char *target)
{
        NOT_YET_IMPLEMENTED("MOUNTING: do_umount");
        return -EINVAL;
}
#endif
