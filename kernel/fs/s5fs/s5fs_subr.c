/*
 *   FILE: s5fs_subr.c
 * AUTHOR: afenn
 *  DESCR:
 *  $Id: s5fs_subr.c,v 1.1.2.1 2006/06/04 01:02:15 afenn Exp $
 */

#include "kernel.h"
#include "util/debug.h"
#include "mm/kmalloc.h"
#include "globals.h"
#include "proc/sched.h"
#include "proc/kmutex.h"
#include "errno.h"
#include "util/string.h"
#include "util/printf.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/s5fs/s5fs_subr.h"
#include "fs/s5fs/s5fs.h"
#include "mm/mm.h"
#include "mm/page.h"

#define dprintf(...) dbg(DBG_S5FS, __VA_ARGS__)

#define s5_dirty_super(fs)                                           \
        do {                                                         \
                pframe_t *p;                                         \
                int err;                                             \
                pframe_get(S5FS_TO_VMOBJ(fs), S5_SUPER_BLOCK, &p);   \
                KASSERT(p);                                          \
                err = pframe_dirty(p);                               \
                KASSERT(!err                                         \
                        && "shouldn\'t fail for a page belonging "   \
                        "to a block device");                        \
        } while (0)


static void s5_free_block(s5fs_t *fs, int block);
static int s5_alloc_block(s5fs_t *);


/*
 * Return the disk-block number for the given seek pointer (aka file
 * position).
 *
 * If the seek pointer refers to a sparse block, and alloc is false,
 * then return 0. If the seek pointer refers to a sparse block, and
 * alloc is true, then allocate a new disk block (and make the inode
 * point to it) and return it.
 *
 * Be sure to handle indirect blocks!
 *
 * If there is an error, return -errno.
 *
 * You probably want to use pframe_get, pframe_pin, pframe_unpin, pframe_dirty.
 */
int
s5_seek_to_block(vnode_t *vnode, off_t seekptr, int alloc)
{
    int block_no;
    uint32_t block_index = S5_DATA_BLOCK( seekptr );
    s5_inode_t *inode = VNODE_TO_S5INODE( vnode );
    /* A file is sparse when the block number is 0 */
    pframe_t *frame;

    /* First check if the block index would overflow the direct & 
    indirect blocks */
    if( block_index >= ( S5_MAX_FILE_BLOCKS ) ) 
        return -ENOSPC;

    /* If not, then check if it belongs in the direct or in the indirect block */
    if( block_index < S5_NDIRECT_BLOCKS ) 
        block_no = inode->s5_direct_blocks[block_index];
    else {
        block_no = inode->s5_indirect_block;
        pframe_t *frame;
        int ret;
        if( (ret = pframe_get( S5FS_TO_VMOBJ( VNODE_TO_S5FS( vnode ) )
                                , block_no, &frame )) < 0)
            return ret;
        pframe_pin( frame );
        block_no = *( (int *) frame->pf_addr 
                    + (( block_index - S5_NDIRECT_BLOCKS ) ) );
        pframe_unpin(frame);
    }
    /* If the block is sparse  */
    if( !block_no ) {
        if( !alloc ) {
            return 0;
        }
        else {
            if( (block_no = s5_alloc_block( VNODE_TO_S5FS( vnode ) )) < 0 )
                return block_no;
            
            if( block_index < S5_NDIRECT_BLOCKS ) {
                inode->s5_direct_blocks[block_index] = block_no;
                s5_dirty_inode( VNODE_TO_S5FS( vnode ), VNODE_TO_S5INODE( vnode ) );
            }
            else {
                int ret;
                *((int *) frame->pf_addr + ((block_index - S5_NDIRECT_BLOCKS) 
                    * sizeof( uint32_t )))  = block_no;
                pframe_unpin( frame );
                if( (ret = pframe_get( S5FS_TO_VMOBJ( VNODE_TO_S5FS( vnode ) )
                                        ,inode->s5_indirect_block , &frame )) < 0)
                    return ret;
                pframe_pin( frame );
                pframe_dirty( frame );
                pframe_unpin( frame );
            }
        }
    }

    return block_no;
}


/*
 * Locks the mutex for the whole file system
 */
static void
lock_s5(s5fs_t *fs)
{
        kmutex_lock(&fs->s5f_mutex);
}

/*
 * Unlocks the mutex for the whole file system
 */
static void
unlock_s5(s5fs_t *fs)
{
        kmutex_unlock(&fs->s5f_mutex);
}


/*
 * Write len bytes to the given inode, starting at seek bytes from the
 * beginning of the inode. On success, return the number of bytes
 * actually written (which should be 'len', unless there's only enough
 * room for a partial write); on failure, return -errno.
 *
 * This function should allow writing to files or directories, treating
 * them identically.
 *
 * Writing to a sparse block of the file should cause that block to be
 * allocated.  Writing past the end of the file should increase the size
 * of the file. Blocks between the end and where you start writing will
 * be sparse.
 *
 * Do not call s5_seek_to_block() directly from this function.  You will
 * use the vnode's pframe functions, which will eventually result in a
 * call to s5_seek_to_block().
 *
 * You will need pframe_dirty(), pframe_get(), memcpy().
 */
int
s5_write_file(vnode_t *vnode, off_t seek, const char *bytes, size_t len)
{   
    pframe_t *frame;
    size_t temp = len;
    int bytes_offset = 0, ret;
    uint32_t file_offset;
    void *curr_offset;
    
    /* Loop until we have nothing more to write */
    while( temp > 0 ) {

        /* First, get the block/page number that we must write to */
        uint32_t block_index = S5_DATA_BLOCK( seek );

        /* Check to see we are overflowing */
        if( block_index >= S5_MAX_FILE_BLOCKS && temp != 0 )  
            return len - temp;

        if( (ret = pframe_get( &(vnode->vn_mmobj), block_index, &frame )) < 0 )
            return ret;
        pframe_pin( frame );
        /* Now, we must write to the pf_addr + file offset */
        file_offset = S5_DATA_OFFSET( seek );

        curr_offset = frame->pf_addr + file_offset;

        if( S5_BLOCK_SIZE - file_offset >= temp ) {
            if( seek >= vnode->vn_len ) {
                int diff = seek - vnode->vn_len;
                memset( frame->pf_addr + vnode->vn_len, 0, diff );
                vnode->vn_len += (diff + temp);
                VNODE_TO_S5INODE( vnode )->s5_size += (diff + temp);
            }
            memcpy( curr_offset, bytes + bytes_offset, temp );
            pframe_dirty( frame );
            pframe_unpin( frame );
            break;
        }
        else {
            memcpy( curr_offset, bytes + bytes_offset, ( S5_BLOCK_SIZE - file_offset ) );
            if( seek >= vnode->vn_len ) {
                vnode->vn_len += (S5_BLOCK_SIZE - file_offset);
                VNODE_TO_S5INODE( vnode )->s5_size += (S5_BLOCK_SIZE - file_offset);
            }
            temp -= (S5_BLOCK_SIZE - file_offset);
            seek += (S5_BLOCK_SIZE - file_offset + 1);
            bytes_offset += ( S5_BLOCK_SIZE - file_offset + 1 );
        }
        pframe_dirty( frame );
        pframe_unpin( frame );
    }
    return len;
}

/*
 * Read up to len bytes from the given inode, starting at seek bytes
 * from the beginning of the inode. On success, return the number of
 * bytes actually read, or 0 if the end of the file has been reached; on
 * failure, return -errno.
 *
 * This function should allow reading from files or directories,
 * treating them identically.
 *
 * Reading from a sparse block of the file should act like reading
 * zeros; it should not cause the sparse blocks to be allocated.
 *
 * Similarly as in s5_write_file(), do not call s5_seek_to_block()
 * directly from this function.
 *
 * If the region to be read would extend past the end of the file, less
 * data will be read than was requested.
 *
 * You probably want to use pframe_get(), memcpy().
 */

 /*  TODO: Add check for end of file, while there are subsequent blocks free */
int
s5_read_file(struct vnode *vnode, off_t seek, char *dest, size_t len)
{
    pframe_t *frame = NULL;
    int dest_offset = 0;
    if( len > (vnode->vn_len - seek) )
        len = vnode->vn_len - seek;
    size_t temp = len;

    while( temp > 0 ) {

        /* First, get the block/page number that we must read from */
        if( seek >= VNODE_TO_S5INODE( vnode )->s5_size )
            return len - temp;

        uint32_t block_index = S5_DATA_BLOCK( seek );

        /* Check to see we are overflowing */
        if( block_index >= S5_MAX_FILE_BLOCKS && temp != 0 ) 
            return len - temp;

        /* pframe_get will not actually allocate the block, pframe_dirty will */
        pframe_get( &(vnode->vn_mmobj), block_index, &frame );

        uint32_t file_offset = S5_DATA_OFFSET( seek );
           
        /* Now, we must write to the pf_addr + file offset */
        void* curr_offset = frame->pf_addr + file_offset;
        if( S5_BLOCK_SIZE - file_offset >= temp ) {
            memcpy( dest + dest_offset, curr_offset, temp );
            break;
        }
        else {
            memcpy( dest + dest_offset, curr_offset, ( S5_BLOCK_SIZE - file_offset ) );
            temp -= (S5_BLOCK_SIZE - file_offset);
            seek += (S5_BLOCK_SIZE - file_offset);
            dest_offset += ( S5_BLOCK_SIZE - file_offset );
        }
    }
    return len;
}

/*
 * Allocate a new disk-block off the block free list and return it. If
 * there are no free blocks, return -ENOSPC.
 *
 * This will not initialize the contents of an allocated block; these
 * contents are undefined.
 *
 * If the super block's s5s_nfree is 0, you need to refill 
 * s5s_free_blocks and reset s5s_nfree.  You need to read the contents 
 * of this page using the pframe system in order to obtain the next set of
 * free block numbers.
 *
 * Don't forget to dirty the appropriate blocks!
 *
 * You'll probably want to use lock_s5(), unlock_s5(), pframe_get(),
 * and s5_dirty_super()
 * ERROR: Using s5_lock throws error
 */
static int
s5_alloc_block(s5fs_t *fs)
{
    /* Get a new disk block form the free block list in the superblock
    // But, first check if there are any free blocks left */
   /* lock_s5( fs ); */
    s5_super_t *superblock = fs->s5f_super;
    /* The last block of the free_block_list points to the next set of free
        blocks, therefore, we first decrement nfree and then check if it's zero */
    superblock->s5s_nfree--;
    /* If there are no free blocks left */
    if( !superblock->s5s_nfree ) {
        /* Now copy over the next set, first get the last one */
        uint32_t next = superblock->s5s_free_blocks[ S5_NBLKS_PER_FNODE - 1 ];
        /* Get the page of the superblock */
        pframe_t *next_frame;
        pframe_get( S5FS_TO_VMOBJ(fs), next, &next_frame );
        pframe_pin( next_frame );
        memcpy( (void *) &(superblock->s5s_free_blocks[0]), next_frame->pf_addr
                , S5_NBLKS_PER_FNODE * sizeof( uint32_t ) );
        /* Reset number of blocks that are free */
        superblock->s5s_nfree = S5_NBLKS_PER_FNODE - 1;
        s5_dirty_super( fs );
        pframe_unpin( next_frame );
        return next;
    }

    s5_dirty_super( fs );
    int ret = superblock->s5s_free_blocks[S5_NBLKS_PER_FNODE 
                                          - superblock->s5s_nfree - 1];
    /* unlock_s5( fs ); */
    return ret;
}


/*
 * Given a filesystem and a block number, frees the given block in the
 * filesystem.
 *
 * This function may potentially block.
 *
 * The caller is responsible for ensuring that the block being placed on
 * the free list is actually free and is not resident.
 */
static void
s5_free_block(s5fs_t *fs, int blockno)
{
        s5_super_t *s = fs->s5f_super;


        lock_s5(fs);

        KASSERT(S5_NBLKS_PER_FNODE > s->s5s_nfree);

        if ((S5_NBLKS_PER_FNODE - 1) == s->s5s_nfree) {
                /* get the pframe where we will store the free block nums */
                pframe_t *prev_free_blocks = NULL;
                KASSERT(fs->s5f_bdev);
                pframe_get(&fs->s5f_bdev->bd_mmobj, blockno, &prev_free_blocks);
                KASSERT(prev_free_blocks->pf_addr);

                /* copy from the superblock to the new block on disk */
                memcpy(prev_free_blocks->pf_addr, (void *)(s->s5s_free_blocks),
                       S5_NBLKS_PER_FNODE * sizeof(int));
                pframe_dirty(prev_free_blocks);

                /* reset s->s5s_nfree and s->s5s_free_blocks */
                s->s5s_nfree = 0;
                s->s5s_free_blocks[S5_NBLKS_PER_FNODE - 1] = blockno;
        } else {
                s->s5s_free_blocks[s->s5s_nfree++] = blockno;
        }

        s5_dirty_super(fs);

        unlock_s5(fs);
}

/*
 * Creates a new inode from the free list and initializes its fields.
 * Uses S5_INODE_BLOCK to get the page from which to create the inode
 *
 * This function may block.
 */
int
s5_alloc_inode(fs_t *fs, uint16_t type, devid_t devid)
{
        s5fs_t *s5fs = FS_TO_S5FS(fs);
        pframe_t *inodep;
        s5_inode_t *inode;
        int ret = -1;

        KASSERT((S5_TYPE_DATA == type)
                || (S5_TYPE_DIR == type)
                || (S5_TYPE_CHR == type)
                || (S5_TYPE_BLK == type));


        lock_s5(s5fs);

        if (s5fs->s5f_super->s5s_free_inode == (uint32_t) -1) {
                unlock_s5(s5fs);
                return -ENOSPC;
        }

        pframe_get(&s5fs->s5f_bdev->bd_mmobj,
                   S5_INODE_BLOCK(s5fs->s5f_super->s5s_free_inode),
                   &inodep);
        KASSERT(inodep);

        inode = (s5_inode_t *)(inodep->pf_addr)
                + S5_INODE_OFFSET(s5fs->s5f_super->s5s_free_inode);

        KASSERT(inode->s5_number == s5fs->s5f_super->s5s_free_inode);

        ret = inode->s5_number;

        /* reset s5s_free_inode; remove the inode from the inode free list: */
        s5fs->s5f_super->s5s_free_inode = inode->s5_next_free;
        pframe_pin(inodep);
        s5_dirty_super(s5fs);
        pframe_unpin(inodep);


        /* init the newly-allocated inode: */
        inode->s5_size = 0;
        inode->s5_type = type;
        inode->s5_linkcount = 0;
        memset(inode->s5_direct_blocks, 0, S5_NDIRECT_BLOCKS * sizeof(int));
        if ((S5_TYPE_CHR == type) || (S5_TYPE_BLK == type))
                inode->s5_indirect_block = devid;
        else
                inode->s5_indirect_block = 0;

        s5_dirty_inode(s5fs, inode);

        unlock_s5(s5fs);

        return ret;
}


/*
 * Free an inode by freeing its disk blocks and putting it back on the
 * inode free list.
 *
 * You should also reset the inode to an unused state (eg. zero-ing its
 * list of blocks and setting its type to S5_FREE_TYPE).
 *
 * Don't forget to free the indirect block if it exists.
 *
 * You probably want to use s5_free_block().
 */
void
s5_free_inode(vnode_t *vnode)
{
        uint32_t i;
        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
        s5fs_t *fs = VNODE_TO_S5FS(vnode);

        KASSERT((S5_TYPE_DATA == inode->s5_type)
                || (S5_TYPE_DIR == inode->s5_type)
                || (S5_TYPE_CHR == inode->s5_type)
                || (S5_TYPE_BLK == inode->s5_type));

        /* free any direct blocks */
        for (i = 0; i < S5_NDIRECT_BLOCKS; ++i) {
                if (inode->s5_direct_blocks[i]) {
                        dprintf("freeing block %d\n", inode->s5_direct_blocks[i]);
                        s5_free_block(fs, inode->s5_direct_blocks[i]);

                        s5_dirty_inode(fs, inode);
                        inode->s5_direct_blocks[i] = 0;
                }
        }

        if (((S5_TYPE_DATA == inode->s5_type)
             || (S5_TYPE_DIR == inode->s5_type))
            && inode->s5_indirect_block) {
                pframe_t *ibp;
                uint32_t *b;

                pframe_get(S5FS_TO_VMOBJ(fs),
                           (unsigned)inode->s5_indirect_block,
                           &ibp);
                KASSERT(ibp
                        && "because never fails for block_device "
                        "vm_objects");
                pframe_pin(ibp);

                b = (uint32_t *)(ibp->pf_addr);
                for (i = 0; i < S5_NIDIRECT_BLOCKS; ++i) {
                        KASSERT(b[i] != inode->s5_indirect_block);
                        if (b[i])
                                s5_free_block(fs, b[i]);
                }

                pframe_unpin(ibp);

                s5_free_block(fs, inode->s5_indirect_block);
        }

        inode->s5_indirect_block = 0;
        inode->s5_type = S5_TYPE_FREE;
        s5_dirty_inode(fs, inode);

        lock_s5(fs);
        inode->s5_next_free = fs->s5f_super->s5s_free_inode;
        fs->s5f_super->s5s_free_inode = inode->s5_number;
        unlock_s5(fs);

        s5_dirty_inode(fs, inode);
        s5_dirty_super(fs);
}

/*
 * Locate the directory entry in the given inode with the given name,
 * and return its inode number. If there is no entry with the given
 * name, return -ENOENT.
 *
 * You'll probably want to use s5_read_file and name_match
 *
 * You can either read one dirent at a time or optimize and read more.
 * Either is fine.
 */
int
s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen) 
{
    int offset;
    uint32_t i;
    for( i = 0, offset = 0; i < S5_DIRENTS_PER_BLOCK * S5_MAX_FILE_BLOCKS; 
                            i++, offset += sizeof( s5_dirent_t ) ) {
        s5_dirent_t entry;
        memset( &entry, 0, sizeof(s5_dirent_t) );
        if( s5_read_file( vnode, offset, (char *) (&entry), sizeof(s5_dirent_t) ) < 0 )
            return -ENOENT;

        /* ERROR: Seems inefficient to iterate over everything */
        
        if( name_match( entry.s5d_name, name, namelen ) ) 
            return entry.s5d_inode;
    }
    return -ENOENT;
}

/*
 * Locate the directory entry in the given inode with the given name,
 * and delete it. If there is no entry with the given name, return
 * -ENOENT.
 *
 * In order to ensure that the directory entries are contiguous in the
 * directory file, you will need to move the last directory entry into
 * the remove dirent's place.
 *
 * When this function returns, the inode refcount on the removed file
 * should be decremented.
 *
 * It would be a nice extension to free blocks from the end of the
 * directory file which are no longer needed.
 *
 * Don't forget to dirty appropriate blocks!
 *
 * You probably want to use vget(), vput(), s5_read_file(),
 * s5_write_file(), and s5_dirty_inode().
 */
int
s5_remove_dirent(vnode_t *vnode, const char *name, size_t namelen)
{
    /* First find the block (ERROR: Can we call find_dirent here? Then we wouldn't know the
    // address of the entry) */
    int offset, flag = 0, block_offset = 0, bytes_read;
    uint32_t i;
    for( i = 0, offset = 0; i < S5_DIRENTS_PER_BLOCK * S5_MAX_FILE_BLOCKS; 
                            i++, offset += sizeof( s5_dirent_t ) ) {
        s5_dirent_t entry;
        if( (bytes_read = s5_read_file( vnode, offset, (char *) (&entry)
                                        , sizeof(s5_dirent_t) )) < 0) 
            return bytes_read;
        else if( bytes_read == 0 ) {
            /* Meaning there is nothing left to read */
            /* Need to read the last dirent */
            if( flag ) {
                s5_read_file( vnode, offset - sizeof(s5_dirent_t), (char *) &entry, sizeof(s5_dirent_t) );
                s5_write_file( vnode, block_offset, (char *) &entry, sizeof( s5_dirent_t ) );
                vnode->vn_len -= sizeof( s5_dirent_t );
                VNODE_TO_S5INODE(vnode)->s5_size -= sizeof( s5_dirent_t );
                s5_dirty_inode( VNODE_TO_S5FS( vnode ), VNODE_TO_S5INODE( vnode ) );
                return 0;
            }
            else {
                return -ENOENT;
            }
        } 
        
        if( name_match( entry.s5d_name, name, namelen ) ) {
            /* Now, we've found the block, need to iterate forward to find the last block */
            flag = 1;
            /* Since we are in this function, we can assume that it is safe to free */
            vnode_t *to_remove = vget( vnode->vn_fs, entry.s5d_inode );
            /* One for the previous vget and one for actual deletion */
            VNODE_TO_S5INODE( to_remove )->s5_linkcount--;
            vput( to_remove );
            block_offset = offset;
        }
            
    }
    return -ENOENT;

}

/*
 * Create a new directory entry in directory 'parent' with the given name, which
 * refers to the same file as 'child'.
 *
 * When this function returns, the inode refcount on the file that was linked to
 * should be incremented.
 *
 * Remember to incrament the ref counts appropriately
 *
 * You probably want to use s5_find_dirent(), s5_write_file(), and s5_dirty_inode().
 */
int
s5_link(vnode_t *parent, vnode_t *child, const char *name, size_t namelen)
{
    int child_inumber, ret;
    if( (ret = s5_find_dirent( parent, name, namelen )) >= 0 ) {
        return -EEXIST;
    }
    /* We can write to the end by giving the size of the file as the offset
     to write_file */
    uint32_t file_size = VNODE_TO_S5INODE( parent )->s5_size;
    /* Now, write to the end of the file */
    s5_dirent_t dirent;
    dirent.s5d_inode = VNODE_TO_S5INODE( child )->s5_number;
    memcpy( dirent.s5d_name, name, S5_NAME_LEN);

    if( (ret = s5_write_file( parent, file_size, (char *) &dirent, sizeof( s5_dirent_t ) )) < 0 )
        return ret;

    /* Increment ref count of the child */
    /* vget( child->vn_fs, VNODE_TO_S5INODE( child )->s5_number ); */
    VNODE_TO_S5INODE( child )->s5_linkcount += 1;
    /* Dirty the inode since we wrote to it */
    s5_dirty_inode( VNODE_TO_S5FS(parent), VNODE_TO_S5INODE(parent) );
    s5_dirty_inode( VNODE_TO_S5FS(child), VNODE_TO_S5INODE(child) );

    return 0;

}

/*
 * Return the number of blocks that this inode has allocated on disk.
 * This should include the indirect block, but not include sparse
 * blocks.
 *
 * This is only used by s5fs_stat().
 *
 * You'll probably want to use pframe_get().
 */
int
s5_inode_blocks(vnode_t *vnode)
{
    s5_inode_t *inode = VNODE_TO_S5INODE( vnode );
    uint32_t i, counter = 0;
    /* First handle the direct blocks */
    for( i = 0; i < S5_NDIRECT_BLOCKS; i++ ) {
        if( inode->s5_direct_blocks[i] )
            counter++;
    }

    /* Now handle the indirect blocks */
    if( inode->s5_indirect_block ) {
        pframe_t *frame;
        pframe_get( S5FS_TO_VMOBJ( VNODE_TO_S5FS( vnode ) )
                    , inode->s5_indirect_block, &frame );
        for( i = 0; i < S5_NIDIRECT_BLOCKS; i++ ) {
            if( *( (uint32_t *) (frame->pf_addr + (i * sizeof( uint32_t ))) ) )
                counter++;
        }
    }

    return counter;
}

