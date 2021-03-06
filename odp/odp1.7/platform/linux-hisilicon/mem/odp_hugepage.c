/*
 * Copyright(c) 2010-2015 Intel Corporation.
 * Copyright(c) 2014-2015 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the BSD-3-Clause License as published by
 * the Free Software Foundation.
 *
 */


#include <string.h>
#include <sys/types.h>
#include <sys/file.h>
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/queue.h>

#include <odp_memory.h>
#include <odp_mmdistrict.h>
#include <odp_base.h>
#include <odp/config.h>

#include <odp_core.h>
#include <odp_common.h>
#include "odp_local_cfg.h"
#include "odp_hugepage.h"
#include "odp_filesystem.h"
#include "odp_debug_internal.h"

static const char sys_dir_path[] = "/sys/kernel/mm/hugepages";

static int get_num_hugepages(const char *subdir)
{
	char path[ODP_PATH_MAX];
	unsigned long resv_pages, num_pages = 0;
	const char   *nr_hp_file;

	snprintf(path, sizeof(path), "%s/%s/%s",
		 sys_dir_path, subdir, "resv_hugepages");
	resv_pages = odp_parse_sysfs_value(path);
	if (resv_pages < 0)
		return 0;

	if (local_config.process_type == ODP_PROC_SECONDARY)
		nr_hp_file = "nr_hugepages";
	else
		nr_hp_file = "free_hugepages";

	snprintf(path, sizeof(path), "%s/%s/%s",
		 sys_dir_path, subdir, nr_hp_file);
	num_pages = odp_parse_sysfs_value(path);
	if (num_pages < 0)
		return 0;

	if (num_pages == 0)
		ODP_PRINT("No free hugepages repohodpd in %s\n", subdir);

	if ((num_pages > 0) && (local_config.process_type == ODP_PROC_PRIMARY))
		num_pages -= resv_pages;

	return (int)num_pages;
}

static uint64_t get_default_hp_size(void)
{
	const char proc_meminfo[] = "/proc/meminfo";
	const char str_hugepagesz[] = "Hugepagesize:";
	unsigned int hugepagesz_len = sizeof(str_hugepagesz) - 1;
	char buffer[256];
	unsigned long long size = 0;

	FILE *fd = fopen(proc_meminfo, "r");

	if (!fd)
		ODP_PRINT("Cannot open %s\n", proc_meminfo);

	while (fgets(buffer, sizeof(buffer), fd))
		if (strncmp(buffer, str_hugepagesz, hugepagesz_len) == 0) {
			size = odp_str_to_size(&buffer[hugepagesz_len]);
			break;
		}

	fclose(fd);
	if (size == 0)
		ODP_PRINT("Cannot get default hugepage "
			  "size from %s\n", proc_meminfo);

	return size;
}

#define HUGETLBFS_DESC	   "hugetlbfs"
#define HGTLB_DSC_LEN	   9
#define HUGEPAGE_SIZE_DESC "pagesize="
#define HP_DESC_LEN	   9
static const char *get_hugepage_dir(uint64_t hugepage_sz)
{
	enum proc_mount_fieldnames {
		DEVICE = 0,
		MOUNTPT,
		FSTYPE,
		OPTIONS,
		_FIELDNAME_MAX
	};
	static uint64_t default_size;
	const char split_tok = ' ';
	char *splitstr[_FIELDNAME_MAX];
	char  buf[ODP_BUFF_SIZE];
	char *retval = NULL;

	FILE *fd = fopen("/proc/mounts", "r");

	if (!fd)
		ODP_PRINT("Cannot open proc_mount\n");

	if (default_size == 0)
		default_size = get_default_hp_size();

	while (fgets(buf, sizeof(buf), fd)) {
		if (odp_strsplit(buf, sizeof(buf), splitstr, _FIELDNAME_MAX,
				 split_tok) != _FIELDNAME_MAX) {
			ODP_PRINT("Error parsing proc_mount\n");
			break; /* return NULL */
		}

		if (strncmp(splitstr[FSTYPE], HUGETLBFS_DESC,
			    HGTLB_DSC_LEN) == 0) {
			const char *pagesz_str = strstr(splitstr[OPTIONS],
							HUGEPAGE_SIZE_DESC);

			if (!pagesz_str) {
				if (hugepage_sz == default_size) {
					retval = strdup(splitstr[MOUNTPT]);
					break;
				}
			}

			else {
				uint64_t pagesz =
					odp_str_to_size(
						&pagesz_str[HP_DESC_LEN]);

				if (pagesz == hugepage_sz) {
					retval = strdup(splitstr[MOUNTPT]);
					break;
				}
			}
		}
	}

	fclose(fd);
	return retval;
}

static inline void swap_hpt(struct odp_hugepage_type *a,
			    struct odp_hugepage_type *b)
{
	char buf[sizeof(*a)];

	memcpy(buf, a, sizeof(buf));
	memcpy(a, b, sizeof(buf));
	memcpy(b, buf, sizeof(buf));
}

static int clear_hugedir(const char *hugedir)
{
	DIR *dir;
	struct dirent *dirent;
	int  dir_fd, fd, lck_result;
	const char filter[] = "*map_*";

	dir = opendir(hugedir);
	if (!dir) {
		ODP_PRINT("Unable to open hugepage directory %s\n", hugedir);
		goto error;
	}

	dir_fd = dirfd(dir);

	dirent = readdir(dir);
	if (!dirent) {
		ODP_PRINT("Unable to read hugepage directory %s\n", hugedir);
		goto error;
	}

	while (dirent) {
		if (fnmatch(filter, dirent->d_name, 0) > 0) {
			dirent = readdir(dir);
			continue;
		}

		fd = openat(dir_fd, dirent->d_name, O_RDONLY);

		if (fd == -1) {
			dirent = readdir(dir);
			continue;
		}

		lck_result = flock(fd, LOCK_EX | LOCK_NB);

		if (lck_result != -1) {
			flock(fd, LOCK_UN);
			unlinkat(dir_fd, dirent->d_name, 0);
		}

		close(fd);
		dirent = readdir(dir);
	}

	closedir(dir);
	return 0;

error:
	if (dir)
		closedir(dir);

	ODP_PRINT("Error while clearing hugepage dir: %s\n", strerror(errno));

	return -1;
}

#define ODP_SYS_HGPG_STR_LEN 10
int odp_hugepage_info_init(void)
{
	unsigned i, num_sizes = 0;
	struct dirent *dirent = NULL;
	DIR *dir = NULL;
	struct odp_hugepage_type *hpt = NULL;

	dir = opendir(sys_dir_path);
	if (!dir)
		ODP_PRINT("Cannot open directory %s to "
			  "read system hugepage info\n", sys_dir_path);

	dirent = readdir(dir);

	while (dirent) {
		if (strncmp(dirent->d_name, "hugepages-",
			    ODP_SYS_HGPG_STR_LEN) == 0) {
			hpt = &local_config.odp_hugepage_type[num_sizes];

			hpt->hugepage_sz =
				odp_str_to_size(&dirent->d_name[
							ODP_SYS_HGPG_STR_LEN]);
			hpt->hugedir = get_hugepage_dir(hpt->hugepage_sz);

			if (!hpt->hugedir) {
				int32_t num_pages;

				num_pages = get_num_hugepages(dirent->d_name);
				if (num_pages > 0) {
					ODP_PRINT("%d hugepages of"
						  " size %lu reserved, ",
						  num_pages,
						  hpt->hugepage_sz);
					ODP_PRINT("but no mounted hugetlbfs"
						  " found for that size\n");
				}
			} else {
				hpt->lock_descriptor = open(hpt->hugedir,
							    O_RDONLY);

				if (flock(hpt->lock_descriptor,
					  LOCK_EX) == -1) {
					ODP_ERR("Failed to lock hugepage"
						" directory!\n");
					closedir(dir);
					return -1;
				}

				if (clear_hugedir(hpt->hugedir) == -1) {
					closedir(dir);
					return -1;
				}

				hpt->num_pages[0] =
					get_num_hugepages(dirent->d_name);
				hpt->num_pages[0] =
					ODP_MIN(hpt->num_pages[0],
						ODP_PAGE_MEMORY_MAX /
						hpt->hugepage_sz);
				num_sizes++;
			}
		}

		dirent = readdir(dir);
	}

	closedir(dir);
	local_config.num_hugepage_types = num_sizes;

	for (i = 0; i < num_sizes; i++) {
		unsigned int j;

		for (j = i + 1; j < num_sizes; j++)
			if (local_config.odp_hugepage_type[j - 1].hugepage_sz <
			    local_config.odp_hugepage_type[j].hugepage_sz)
				swap_hpt(
					&local_config.odp_hugepage_type[j - 1],
					&local_config.odp_hugepage_type[j]);
	}

	for (i = 0; i < num_sizes; i++)
		if (local_config.odp_hugepage_type[i].hugedir &&
		    (local_config.odp_hugepage_type[i].num_pages[0] > 0))
			return 0;

	return -1;
}
