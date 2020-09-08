#include <rtthread.h>
#include <dfs_fs.h>
#include <melonds.h>

int mnt_init(void)
{
    if (dfs_mount(RT_NULL, "/", "squashfs", 0, (const void *)melonds) == 0)
    {
        rt_kprintf("squash file system initializated!\n");
    }
    else
    {
        rt_kprintf("squash file system initializate failed!\n");
    }
    
    return 0;
} 
