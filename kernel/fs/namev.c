#include "kernel.h"
#include "globals.h"
#include "types.h"
#include "errno.h"

#include "util/string.h"
#include "util/printf.h"
#include "util/debug.h"

#include "fs/dirent.h"
#include "fs/fcntl.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"

/* This takes a base 'dir', a 'name', its 'len', and a result vnode.
 * Most of the work should be done by the vnode's implementation
 * specific lookup() function, but you may want to special case
 * "." and/or ".." here depnding on your implementation.
 *
 * If dir has no lookup(), return -ENOTDIR.
 *
 * Note: returns with the vnode refcount on *result incremented.
 */
int
lookup(vnode_t *dir, const char *name, size_t len, vnode_t **result)
{
    if ( dir->vn_ops->lookup == NULL ) {
        return -ENOTDIR;
    }
    return dir->vn_ops->lookup( dir, name, len, result );

}


/* When successful this function returns data in the following "out"-arguments:
 *  o res_vnode: the vnode of the parent directory of "name"
 *  o name: the `basename' (the element of the pathname)
 *  o namelen: the length of the basename
 *
 * For example: dir_namev("/s5fs/bin/ls", &namelen, &name, NULL,
 * &res_vnode) would put 2 in namelen, "ls" in name, and a pointer to the
 * vnode corresponding to "/s5fs/bin" in res_vnode.
 *
 * The "base" argument defines where we start resolving the path from:
 * A base value of NULL means to use the process's current working directory,
 * curproc->p_cwd.  If pathname[0] == '/', ignore base and start with
 * vfs_root_vn.  dir_namev() should call lookup() to take care of resolving each
 * piece of the pathname.
 * NAME_LEN
 * Note: A successful call to this causes vnode refcount on *res_vnode to
 * be incremented.
 */
int
dir_namev(const char *pathname, size_t *namelen, const char **name,
          vnode_t *base, vnode_t **res_vnode)
{
    /* As in example above, iterate backwards over pathname and get 'ls' */
    int i, j;
    char buf[256];
    memset( buf, 0, 256 );
    buf[0] = '\0'; /* Initially, just the empty string */

    for( i = strlen( pathname ); i >= 0; i--) {
        /* Checks if we have hit a slash, and also if we have extra slashes in the end */
        if( pathname[i] == '/' ) {
            if( i == 0 ) {
                *name = &pathname[i + 1];  
                *namelen = strlen( &pathname[i + 1] );
            }
            else {
                if ( pathname[i - 1] == '/' || pathname[i + 1] == '/' )
                    continue;
                else {
                    *name = &pathname[i + 1];  
                    *namelen = strlen( &pathname[i + 1] );
                }
            }
            break;
        }
        if( i == 0 ) {
            *name = &pathname[i];
            *namelen = strlen( &pathname[i] );
        }
    } /* At this point, name contains 'ls' and namelen contains 2 */

    /* Check name for errors, specifically for being too long */
    if( (*namelen) > NAME_LEN ) { 
            return -ENAMETOOLONG;
    }
    
    if( i < 0 )
        buf[0] = '\0';
    else {
        memcpy( buf, pathname, i );
        buf[i + 1] = '\0';
    }
    /* Get the first token */
    char s[2] = "/";
    char* token = NULL;
    token = strtok( buf, s );
     
    /* Handle edge cases */
    if( !token ) { /* Meaning that we were given something like "/", in the correct case */
        if( pathname[0] == '/' ) { /* We actually have just a slash */
            if( lookup( vfs_root_vn, ".", strlen( "." ), res_vnode ) < 0)
                return -ENOENT;
            return 0;
        }
        else { /* If there is no token, and the first is not "/", then we want
                to look up the current directory */
            if( lookup( curproc->p_cwd, ".", strlen("."), res_vnode ) < 0 ) {
                if( base && (lookup( base, ".", strlen("."), res_vnode ) < 0) )
                    return -ENOENT;
                return -ENOENT;
            }

            return 0;
        }
    }
    /* Here, we want to check a file in the root dir */
    if( strlen( token ) > NAME_LEN ) { /* If the dir name is too long */
            return -ENAMETOOLONG;
    }

    int ret;
    if( pathname[0] == '/' ) { /* Ignore base and start with vfs_root_vn */
        if( (ret = lookup( vfs_root_vn, token, strlen( token ), res_vnode )) < 0)
            return -ENOENT;
    } else if( base == NULL ) { /* Use the process's curr working dir */
        if( (ret = lookup( curproc->p_cwd, token, strlen( token ), res_vnode )) < 0)
            return -ENOENT;
    } else { /* Otherwise, start at the specific base */
        if( (ret = lookup( base, token, strlen( token ), res_vnode )) < 0)
            return -ENOENT;
    }

    vnode_t *child;
    while( token != NULL ) {
        /* In the beginning, token is s5fs */
        /* Perform tests */
        if( strlen( token ) > NAME_LEN ) { /* If the dir name is too long */
            vput( *res_vnode );
            return -ENAMETOOLONG;
        }
        else if( !((*res_vnode)->vn_mode & S_IFDIR) ) { /*If the 'dir' is not actually a dir */
            vput( *res_vnode );
            return -ENOTDIR;
        }

        /* Get next token, => bin */
        if ( !(token = strtok( NULL, s )) ) {
            
            /* Don't vput here because we must return with the res node incremented */
            break;
        }

        /* Perform lookup in the parent dir */
        if( lookup( *res_vnode, token, strlen( token ), &child ) < 0 )
            return -ENOENT;

        /* Decrement the refcount of parent */
        vput( *res_vnode );
        (*res_vnode) = child;
    }

    return 0;
}

/* This returns in res_vnode the vnode requested by the other parameters.
 * It makes use of dir_namev and lookup to find the specified vnode (if it
 * exists).  flag is right out of the parameters to open(2); see
 * <weenix/fnctl.h>.  If the O_CREAT flag is specified, and the file does
 * not exist call create() in the parent directory vnode.
 *
 * Note: Increments vnode refcount on *res_vnode.
 */
int
open_namev(const char *pathname, int flag, vnode_t **res_vnode, vnode_t *base)
{   
    size_t namelen;
    char name_arr[256];
    const char *name = name_arr; 
    vnode_t *dir_namev_res;
    int ret;
    /* Try and find the parent dir (returned in last arg), and the filename */
    if( (ret = dir_namev( pathname, &namelen, &name, base, &dir_namev_res )) < 0)
        return ret;

    /* If we find the parent, then lookup the file */
    ret = lookup( dir_namev_res, name, namelen, res_vnode );
    /* Only O_CREAT is specified (could also have other flags) & file not found */
    /* ,then create a new file with the specified name */
    if( (flag & O_CREAT) && ret < 0) { 
        dir_namev_res->vn_ops->create( dir_namev_res, name, namelen, res_vnode );
    } else if ( ret < 0 ) {
        vput( dir_namev_res );
        return ret;
    }    
    if( dir_namev_res )
        vput( dir_namev_res );
    KASSERT( res_vnode );
    return 0;
}

#ifdef __GETCWD__
/* Finds the name of 'entry' in the directory 'dir'. The name is writen
 * to the given buffer. On success 0 is returned. If 'dir' does not
 * contain 'entry' then -ENOENT is returned. If the given buffer cannot
 * hold the result then it is filled with as many characters as possible
 * and a null terminator, -ERANGE is returned.
 *
 * Files can be uniquely identified within a file system by their
 * inode numbers. */
int
lookup_name(vnode_t *dir, vnode_t *entry, char *buf, size_t size)
{   
    ino_t entry_no = entry->vn_vno; /* inode number of entry node */
    NOT_YET_IMPLEMENTED("GETCWD: lookup_name");
    return -ENOENT;
}


/* Used to find the absolute path of the directory 'dir'. Since
 * directories cannot have more than one link there is always
 * a unique solution. The path is writen to the given buffer.
 * On success 0 is returned. On error this function returns a
 * negative error code. See the man page for getcwd(3) for
 * possible errors. Even if an error code is returned the buffer
 * will be filled with a valid string which has some partial
 * information about the wanted path. */
ssize_t
lookup_dirpath(vnode_t *dir, char *buf, size_t osize)
{
        NOT_YET_IMPLEMENTED("GETCWD: lookup_dirpath");

        return -ENOENT;
}
#endif /* __GETCWD__ */


