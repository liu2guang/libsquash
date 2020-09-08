/*
 * Copyright (c) 2017 - 2020 Minqi Pan <pmq2001@gmail.com>
 *                           Shengyuan Liu <sounder.liu@gmail.com>
 *
 * This file is part of libsquash, distributed under the MIT License
 * For full terms see the included LICENSE file
 */

#include "squash.h"
#include <stdlib.h>

struct squash_fdtable squash_global_fdtable;
MUTEX squash_global_mutex;

static int dupxx(int __fildes )
{
	static int fd = 3; 
	fd++; 
	return fd; 
}

int squash_open_inner(sqfs *fs, const char *path, short follow_link)
{
	sqfs_err error;
	struct squash_file *file = calloc(1, sizeof(struct squash_file));
	short found;
	int fd;
	size_t nr;
	int *handle;

	// try locating the file and fetching its stat
	if (NULL == file)
	{
		errno = ENOMEM;
		return -1;
	}
	error = sqfs_inode_get(fs, &file->node, sqfs_inode_root(fs));
	if (SQFS_OK != error)
	{
		goto failure;
	}
	error = sqfs_lookup_path_inner(fs, &file->node, path, &found, follow_link);
	if (SQFS_OK != error)
	{
		goto failure;
	}
	file->filename = strdup(path);

	if (!found)
	{
		errno = ENOENT;
		goto failure;
	}
	error = sqfs_stat(fs, &file->node, &file->st);
	if (SQFS_OK != error)
	{
		goto failure;
	}
	file->fs = fs;
	file->pos = 0;

	// get a dummy fd from the system
	// fd = dupxx(0);
	// fd = 0; 
	extern int fd_new(void); 
	fd = fd_new(); 
	if (-1 == fd) {
		goto failure;
	}
	// make sure that our global fd table is large enough
	nr = fd + 1;

	MUTEX_LOCK(&squash_global_mutex);
	if (squash_global_fdtable.nr < nr)
	{
		// we secretly extend the requested size
		// in order to minimize the number of realloc calls
		nr *= 10;
		squash_global_fdtable.fds = realloc(squash_global_fdtable.fds,
						    nr * sizeof(struct squash_file *));
		if (NULL == squash_global_fdtable.fds)
		{
			errno = ENOMEM;
			goto failure;
		}
		memset(squash_global_fdtable.fds + squash_global_fdtable.nr,
		       0,
		       (nr - squash_global_fdtable.nr) * sizeof(struct squash_file *));
		squash_global_fdtable.nr = nr;
	}
	MUTEX_UNLOCK(&squash_global_mutex);

	// construct a handle (mainly) for win32
	handle = (int *)malloc(sizeof(int));
	if (NULL == handle) {
		errno = ENOMEM;
		goto failure;
	}
	*handle = fd;
	file->payload = (void *)handle;

	// insert the fd into the global fd table
	file->fd = fd;
	MUTEX_LOCK(&squash_global_mutex);
	squash_global_fdtable.fds[fd] = file;
        if (squash_global_fdtable.end < fd + 1) {
        	squash_global_fdtable.end = fd + 1;
        }
	MUTEX_UNLOCK(&squash_global_mutex);
	return fd;

failure:
	if (!errno) {
		errno = ENOENT;
	}
	free(file);
	return -1;
}

int squash_open(sqfs *fs, const char *path)
{
    return squash_open_inner(fs, path, 1);
}

int squash_close(int vfd)
{
	int ret;
	struct squash_file *file;

	if (!SQUASH_VALID_VFD(vfd)) {
		errno = EBADF;
		return -1;
	}
	// ret = close(vfd);
	extern struct dfs_fd *fd_get(int fd); 
	extern void fd_put(struct dfs_fd *fd); 

	fd_put(fd_get(vfd)); 

	// if (-1 == ret) {
	// 	printf("#############2\n"); 
	// 	return -1;
	// }
	MUTEX_LOCK(&squash_global_mutex);
	if (S_ISDIR(squash_global_fdtable.fds[vfd]->st.st_mode)) {
			SQUASH_DIR *dir = (SQUASH_DIR *) (squash_global_fdtable.fds[vfd]->payload);
			free(dir);
	} else {
			int *handle = (int *) (squash_global_fdtable.fds[vfd]->payload);
			free(handle);
	}

	file = squash_global_fdtable.fds[vfd];
	free(file->filename);
	free(file);

	squash_global_fdtable.fds[vfd] = NULL;
	if (vfd + 1 == squash_global_fdtable.end) {
			while (vfd >= 0 && NULL == squash_global_fdtable.fds[vfd]) {
					vfd -= 1;
			}
			squash_global_fdtable.end = vfd + 1;
	} else {
			assert(squash_global_fdtable.end > vfd + 1);
	}
	MUTEX_UNLOCK(&squash_global_mutex);
	return 0;
}

ssize_t squash_read(int vfd, void *buf, sqfs_off_t nbyte)
{
	sqfs_err error;
	struct squash_file *file;

	if (!SQUASH_VALID_VFD(vfd))
	{
		errno = EBADF;
		goto failure;
	}
	file = squash_global_fdtable.fds[vfd];

	error = sqfs_read_range(file->fs, &file->node, file->pos, &nbyte, buf);
	if (SQFS_OK != error)
	{
		goto failure;
	}
	file->pos += nbyte;
	return nbyte;
failure:
	if (!errno) {
		errno = EBADF;
	}
	return -1;
}

off_t squash_lseek(int vfd, off_t offset, int whence)
{
	struct squash_file *file;
	if (!SQUASH_VALID_VFD(vfd))
	{
		errno = EBADF;
		return -1;
	}
	file = squash_global_fdtable.fds[vfd];
	if (SQUASH_SEEK_SET == whence)
	{
		file->pos = offset;
	}
	else if (SQUASH_SEEK_CUR == whence)
	{
		file->pos += offset;
	}
	else if (SQUASH_SEEK_END == whence)
	{
		assert(S_ISREG(file->node.base.mode));
		file->pos = file->node.xtra.reg.file_size;
	}
	return file->pos;
}

static void squash_halt()
{
	if (squash_global_fdtable.fds) {
		free(squash_global_fdtable.fds);
	}
	MUTEX_DESTORY(&squash_global_mutex);
	squash_extract_clear_cache();
}

sqfs_err squash_start()
{
	int ret;
	squash_global_fdtable.nr = 0;
	squash_global_fdtable.fds = NULL;
	MUTEX_INIT(&squash_global_mutex);
	ret = atexit(squash_halt);
	if (0 == ret) {
		return SQFS_OK;
	} else {
		return SQFS_ERR;
	}
}

struct squash_file * squash_find_entry(void *ptr)
{
	size_t i;
	struct squash_file * ret = NULL;
	MUTEX_LOCK(&squash_global_mutex);
	for (i = 0; i < squash_global_fdtable.end; ++i) {
		if (squash_global_fdtable.fds[i] && ptr == squash_global_fdtable.fds[i]->payload) {
			ret = squash_global_fdtable.fds[i];
			break;
		}
	}
	MUTEX_UNLOCK(&squash_global_mutex);
	return ret;
}