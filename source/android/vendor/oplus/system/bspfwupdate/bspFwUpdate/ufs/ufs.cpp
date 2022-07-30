/************************************************************************************
 ** File: - vendor\oplus\apps\bspFwUpdate\touchpanel\touchpanel_update..cpp
 ** 
 ** Copyright (C), 2014-2018, OPLUS Mobile Comm Corp., Ltd
 **
 ** Description:
 **      touchpanel part for FwUpdate module
 **
 ** Version: 1.0
 ** Date created: 3/12/2017
 **
 ** --------------------------- Revision History: --------------------------------
 **     <author>    <data>            <desc>
 ************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <termios.h>
#include <inttypes.h>
#include <cutils/log.h>
//Runsheng.Pei modify with oplusex to decouple begin: {
//#include "../../../../frameworks/base_oplus/libs/opluscriticallog/ProxyJniEntry.h"
#include "CriticalLogExProxy.h"
//end. }
#include "../bspFwUpdate.h"

#define TAG "[bspFwUpdate][ufs]"
#define MAX_LINE 256

static int do_fw_update(struct fw_update_module *fw_module, struct firmware_info *fw)
{
    int ret = -1;
    pid_t status = 0;
    (void)fw_module;
    (void)fw;
    char script[MAX_LINE] = "/system/bin/sh /odm/firmware/ufs/move_target_ufs_fw_to_oplusreserve1.sh";

    ALOGD(TAG" do_fw_update called !\n");

    strcat(script, " ");
    strcat(script, fw_module->up_path);
    status = system(script);

    if ((-1 != status) && WIFEXITED(status) && (0 == WEXITSTATUS(status))) {
        ALOGD(TAG" ufs fw move succ!\n");
        ret = 0;
    }
    return ret;
}

static int get_fw_info(struct fw_update_module *fw_module)
{
    FILE *file;
    char line[MAX_LINE] = {0};
    char prod_name[MAX_LINE] = {0};
    char manufacture[MAX_LINE] = {0};
    char path[MAX_LINE] = "ufs/";

    file = fopen("/proc/devinfo/ufs", "r");
    if (!file) {
        ALOGD(TAG" fail to open: %s\n", "/proc/devinfo/ufs");
        return -1;
    }
    while(fgets(line, MAX_LINE, file)) {
        if (!strncmp(line, "Device version:", 15)) {
            sscanf(line, "Device version:%s\n", prod_name);
        }
        if (!strncmp(line, "Device manufacture:", 19)) {
            sscanf(line, "Device manufacture:\t\t%s\n",  manufacture);
            strcat(path, manufacture);
            strcat(path, "-");
            strcat(path, prod_name);
            strcat(path, ".bin");
            strncpy(fw_module->fw_path, path, PATH_SIZE - 1);
            break;
        }
    }
    ALOGD(TAG" fw_path:%s", fw_module->fw_path);
    fclose(file);

    file = fopen("/proc/devinfo/ufs_version", "r");
    if (!file) {
        ALOGD(TAG" fail to open: %s\n", "/proc/devinfo/ufs_version");
        return -1;
    }
    while(fgets(line, MAX_LINE, file)) {
        if (!strncmp(line, "Device version:", 15)) {
            sscanf(line, "Device version:\t\t%lx\n", &(fw_module->fw_ver));
            fw_module->fw_ver = 0;//for ufs, ignore the fw version chek
            break;
        }
    }
    ALOGD(TAG" fw_ver: 0x%lx", fw_module->fw_ver);
    fclose(file);
    return 0;
}

static int fw_replace_process(struct fw_update_module *fw_module, struct firmware_info *fw)
{
    id_t status = 0;
    (void)fw_module;
    (void)fw;
    char script[MAX_LINE] = "/system/bin/sh /odm/firmware/ufs/move_target_ufs_fw_to_oplusreserve1.sh";

    ALOGD(TAG" fw_replace_process called, fw_path:%s\n", fw_module->up_path);
    strcat(script, " ");
    strcat(script, fw_module->up_path);
    status = system(script);

    if ((-1 != status) && WIFEXITED(status) && (0 == WEXITSTATUS(status))) {
        ALOGD(TAG" ufs fw move succ!\n");
    }
    return 0;
}

int init_func_ufs(struct fw_update_module *fw_module)
{
    ALOGD(TAG"init_func_ufs called\n", errno);
    fw_module->booting_upgrade_support = true;
    fw_module->stuck_boot = false;
    fw_module->fw_update = do_fw_update;

    fw_module->fw_replace = fw_replace_process;
    fw_module->monitor_fwfile_support = true;

    get_fw_info(fw_module);
    return 0;
}

int  get_ic_num_func_ufs(void)
{
    return 1;
}

