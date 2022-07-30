#ifndef _BSP_FW_UPDATE_H_
#define _BSP_FW_UPDATE_H_

#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include "bspFwUpdate_interface.h"
#include "rsaVerify.h"

#define FW_PATH_DATA "/data/oplus/fw_update/"

static const char *firmware_dirs_system[] = {
    "/etc/firmware",
    "/vendor/firmware",
    "/odm/firmware",
    "/firmware/image"
};

struct fw_update_module {
    pthread_t ntid;
    bool booting_upgrade_support;
    bool monitor_fwfile_support;
    bool stuck_boot;
    char stuck_mask;
    int up_state;
    char up_path[PATH_SIZE];

    char fw_path[PATH_SIZE];
    long  fw_ver;     /*Read fw version from driver module.*/
    char *name;      /*module name(path)*/
    int  index;
    int  (*fw_replace)(struct fw_update_module *fw_module, struct firmware_info *fw);
    int   (*fw_update) (struct fw_update_module *fw_module, struct firmware_info *fw);
    int  (*init_func) (struct fw_update_module *fw_module);
};

#define init_func_(arg)     init_func_##arg
#define extern_func(arg) int init_func_(arg)(struct fw_update_module * fw_module);

#define get_func_(arg)     get_ic_num_func_##arg
#define extern_get_func(arg) int get_func_(arg)(void);

#endif

