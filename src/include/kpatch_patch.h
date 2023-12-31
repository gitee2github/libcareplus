/******************************************************************************
 * 2021.09.23 - libcare-ctl: introduce patch-id
 * Huawei Technologies Co., Ltd. <wanghao232@huawei.com> - 0.1.4-12
 ******************************************************************************/

#ifndef __KPATCH_PATCH__
#define __KPATCH_PATCH__

#include "kpatch_common.h"
#include "kpatch_storage.h"
#include "kpatch_file.h"
#include "rbtree.h"

enum {
	ACTION_APPLY_PATCH,
	ACTION_UNAPPLY_PATCH
};

struct patch_data {
	kpatch_storage_t *storage;
	int is_just_started;
	int send_fd;
};

struct unpatch_data {
	char **buildids;
	int nbuildids;
	const char *patch_id;
};

int process_patch(int pid, void *_data);
int process_unpatch(int pid, void *_data);

int patch_apply_hunk(struct object_file *o, size_t nhunk);

#endif
