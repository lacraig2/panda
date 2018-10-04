/*!
 * @file kernelinfo.h
 * @brief Kernel specific information used for Linux OSI.
 *
 * @author Manolis Stamatogiannakis <manolis.stamatogiannakis@vu.nl>
 * @copyright This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 */
#pragma once
#include <stdint.h>

/*!
 * @brief Information and offsets related to `struct task_struct`.
 */
struct task_info {
	uint64_t init_addr;			/**< Address of the `struct task_struct` of the init task. */
	size_t size;				/**< Size of `struct task_struct`. */
	int task_offset;			/**< Offset of task_struct in the thread_info struct. */
	int tasks_offset;			/**< TODO: add documentation for the rest of the struct members */
	int pid_offset;
	int tgid_offset;
	int group_leader_offset;
	int thread_group_offset;
	int real_parent_offset;
	int parent_offset;
	int mm_offset;
	int stack_offset;
	int real_cred_offset;
	int cred_offset;
	int comm_offset;			/**< Offset of the command name in `struct task_struct`. */
	size_t comm_size;			/**< Size of the command name. */
	int files_offset;			/**< Offset for open files information. */
};

/*!
 * @brief Information and offsets related to `struct cred`.
 */
struct cred_info {
	int uid_offset;
	int gid_offset;
	int euid_offset;
	int egid_offset;
};

/*!
 * @brief Information and offsets related to `struct mm_struct`.
 */
struct mm_info {
	size_t size;				/**< Size of `struct mm_struct`. */
	int mmap_offset;
	int pgd_offset;
	int arg_start_offset;
	int start_brk_offset;
	int brk_offset;
	int start_stack_offset;
};

/*!
 * @brief Information and offsets related to `struct vm_area_struct`.
 */
struct vma_info {
	size_t size;				/**< Size of `struct vm_area_struct`. */
	int vm_mm_offset;
	int vm_start_offset;
	int vm_end_offset;
	int vm_next_offset;
	int vm_file_offset;
	int vm_flags_offset;
};

/*!
 * @brief Filesystem information and offsets.
 */
struct fs_info {
	int f_path_dentry_offset;
	int f_path_mnt_offset;
	int f_pos_offset;
	int fdt_offset;
	int fdtab_offset;
	int fd_offset;
};

/*!
 * @brief Path related information and offsets.
 */
struct path_info {
	size_t qstr_size;			/**< Size of `struct qstr`. */
	int d_name_offset;
	int d_iname_offset;
	int d_parent_offset;
	int d_op_offset;			/**< Offset of the dentry ops table. */
	int d_dname_offset;			/**< Offset of dynamic name function in dentry ops. */
	int mnt_root_offset;
	int mnt_parent_offset;
	int mnt_mountpoint_offset;
};

/*!
 * @brief Wrapper for the structure-specific structs.
 */
struct kernelinfo {
	char *name;
	struct task_info task;
	struct cred_info cred;
	struct mm_info mm;
	struct vma_info vma;
	struct fs_info fs;
	struct path_info path;
};

#if defined(__G_LIB_H__) || defined(DOXYGEN)
/*!
 * \def DEFAULT_KERNELINFO_FILE
 * Default name for the kerne info configuration file.
 */
#define DEFAULT_KERNELINFO_FILE "kernelinfo.conf"

#ifdef __cplusplus
extern "C" {
#endif

int read_kernelinfo(gchar const *file, gchar const *group, struct kernelinfo *ki);
#ifdef __cplusplus
}
#endif
#endif

/* vim:set tabstop=4 softtabstop=4 noexpandtab: */
