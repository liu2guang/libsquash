#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>

#include "squash.h"
#include "dfs_squashfs.h"
#include "string.h"

int dfs_squashfs_mount(struct dfs_filesystem *fs, unsigned long rwflag, const void *data)
{
    sqfs_err ret;
    sqfs *fs_sq = NULL;

    /* 判断数据的合法性, 如果不是squashfs文件系统则挂载失败 */ 
    if(data == NULL || strncmp((char *)data, "hsqs", 4))
    {
        rt_kprintf("dfs mount squashfs failed\n"); 
        return (-EIO); 
    }
    
    fs_sq = (sqfs *)rt_calloc(1, sizeof(sqfs)); 
    if(!fs_sq)
    {
        rt_kprintf("squash fs alloc fail\n"); 
        return (-ENOMEM); 
    }

    ret = sqfs_open_image(fs_sq, (const uint8_t *)data, 0);
    if(SQFS_OK != ret)
    {
        rt_kprintf("open squashfs image fail\n");
        return (-EIO); 
    }

    fs->data = (void *)fs_sq; 
    rt_kprintf("Mount 0x%p SquashFS v%d.%d\n", data, fs_sq->sb->s_major, fs_sq->sb->s_minor); 
    
    return RT_EOK;
}

int dfs_squashfs_unmount(struct dfs_filesystem *fs)
{
    sqfs *fs_sq = (sqfs *)(fs->data); 
    RT_ASSERT(fs_sq != RT_NULL);
    sqfs_destroy(fs_sq);
    rt_free(fs_sq); 
    fs->data = RT_NULL; 

    return RT_EOK; 
}

int dfs_squashfs_statfs(struct dfs_filesystem *fs, struct statfs *buf)
{
    sqfs *fs_sq = (sqfs *)(fs->data); 

    RT_ASSERT(fs_sq != RT_NULL);
    RT_ASSERT(buf != NULL);

    /* TODO */ 
    buf->f_bfree = 0;
    buf->f_blocks = 8;
    buf->f_bsize = 512;

    return RT_EOK; 
}

// ls使用, 获取文件的状态
int dfs_squashfs_stat(struct dfs_filesystem *fs, const char *path, struct stat *st)
{
    sqfs *fs_sq = (sqfs *)(fs->data); 
    RT_ASSERT(fs_sq != RT_NULL); 
    squash_stat(fs_sq, path, st);
    return RT_EOK;
}

int dfs_squashfs_open(struct dfs_fd *file)
{
    struct dfs_filesystem *fs = (struct dfs_filesystem *)file->data;
    sqfs *fs_sq = (sqfs *)(fs->data); 
    
    /* 打开目录 */ 
    if(file->flags & O_DIRECTORY)
    {
        SQUASH_DIR *dir = NULL; 
        // rt_kprintf("file->path = %s\n", file->path); 
        dir = squash_opendir(fs_sq, file->path);
        if(!dir)
            return -EIO; 
        file->data = dir;
    }
    else
    {
        int fd = -1;

        if(file->flags & O_WRONLY || file->flags & O_RDWR  || 
           file->flags & O_CREAT  || file->flags & O_TRUNC || 
           file->flags & O_EXCL)
        {
            return -EIO;
        }

        // rt_kprintf("file->fike = %s\n", file->path); 
        fd = squash_open(fs_sq, file->path);
        if(fd > 0)
        {
            struct stat st;
            squash_fstat(fd, &st);

            file->size = st.st_size;
            file->data = (void *)fd;
            file->pos = 0; 

            if(file->flags & O_APPEND)
            {
                int pos = squash_lseek(fd, 0, SQUASH_SEEK_END);
                file->pos = pos;
            }
        }
        else
        {
            return -EIO;
        }
    }

    return RT_EOK;
}

int dfs_squashfs_close(struct dfs_fd *file)
{
    if (file->type == FT_DIRECTORY)
    {
        SQUASH_DIR *dir = (SQUASH_DIR *)(file->data);
        RT_ASSERT(dir != RT_NULL);
        squash_closedir(dir); 
    }
    else if (file->type == FT_REGULAR)
    {
        int fd = (int)(file->data);
        squash_close(fd); 
    }
    
    return RT_EOK; 
}

int dfs_squashfs_read(struct dfs_fd *file, void *buf, size_t len)
{
    int fd = -1; 

    if(file->type == FT_DIRECTORY)
    {
        return -EISDIR;
    }

    /* TODO: update position */

    fd = (int)(file->data);
    return squash_read(fd, buf, len);
}

int dfs_squashfs_lseek(struct dfs_fd *file, rt_off_t offset)
{
    if(file->type == FT_REGULAR)
    {
        int fd = (int)(file->data); 
        int pos = squash_lseek(fd, offset, SQUASH_SEEK_CUR);
        file->pos = pos;
        return file->pos; 
    }
    else if(file->type == FT_DIRECTORY)
    {
        SQUASH_DIR *dir = (SQUASH_DIR *)(file->data);
        RT_ASSERT(dir != RT_NULL);

        squash_seekdir(dir, offset);
        int pos = squash_telldir(dir);
        file->pos = pos;
        return file->pos; 
    }

    return (-EIO); 
}

int dfs_squashfs_getdents(struct dfs_fd *file, struct dirent *dirp, uint32_t count)
{
    SQUASH_DIR *dir = (SQUASH_DIR *)(file->data);
    rt_uint32_t index;
    struct dirent *d;
    struct SQUASH_DIRENT *dirent;

    RT_ASSERT(dir != RT_NULL);

    /* make integer count */
    count = (count / sizeof(struct dirent)) * sizeof(struct dirent);
    if (count == 0)
        return -EINVAL;

    index = 0;
    while(1)
    {
        d = dirp + index;

        dirent = squash_readdir(dir); 
        if(!dirent)
        {
            break;
        } 
        d->d_type = dirent->d_type; 
        d->d_namlen = dirent->d_namlen; 
        d->d_reclen = dirent->d_ino; 
        memcpy(d->d_name, dirent->d_name, DFS_PATH_MAX); 

        index++; 
        if (index * sizeof(struct dirent) >= count)
            break; 
    }

    if(index == 0)
    {
        return -EIO; 
    }

    file->pos += index * sizeof(struct dirent);

    return index * sizeof(struct dirent);
}

static const struct dfs_file_ops _squashfs_fops =
{
    dfs_squashfs_open, 
    dfs_squashfs_close, 
    RT_NULL, /* ioctl */ 
    dfs_squashfs_read, 
    RT_NULL, /* write */ 
    RT_NULL, /* flush */ 
    dfs_squashfs_lseek, 
    dfs_squashfs_getdents, 
    RT_NULL  /* poll */ 
};

static const struct dfs_filesystem_ops _squashfs =
{
    "squashfs",
    DFS_FS_FLAG_DEFAULT,
    &_squashfs_fops,

    dfs_squashfs_mount,
    dfs_squashfs_unmount,
    RT_NULL, /* no mkfs */ 
    dfs_squashfs_statfs, 

    RT_NULL, /* no unlink */ 
    dfs_squashfs_stat,
    RT_NULL, /* no rename */ 
};

int dfs_squashfs_init(void)
{
    squash_start();
    dfs_register(&_squashfs);
    return 0;
}
INIT_COMPONENT_EXPORT(dfs_squashfs_init); 
