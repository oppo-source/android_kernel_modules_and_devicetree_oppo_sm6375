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
#include <cutils/properties.h>
#include <errno.h>

//Runsheng.Pei modify with oplusex to decouple begin: {
//#include "../../../../frameworks/base_oplus/libs/opluscriticallog/ProxyJniEntry.h"
#include "CriticalLogExProxy.h"
//end. }
#include "../bspFwUpdate.h"

#include <unistd.h>


#define TAG "[bspFwUpdate][touch]"
#define TP_FW_PATH "/proc/touchpanel"
#define TP_DEVINFO_PATH "/proc/devinfo/tp"

static int do_fw_update(struct fw_update_module *fw_module, struct firmware_info *fw)
{
    int fd = -1;
    int ret = 0;
    char path[PATH_SIZE] = {0};

    if (fw_module->index == 0) {
        snprintf(path, sizeof(path), "%s", TP_FW_PATH"/tp_fw_update");
    } else {
        snprintf(path, sizeof(path), "%s%d%s", TP_FW_PATH, fw_module->index, "/tp_fw_update");
    }

    ALOGD(TAG" [touch] do_fw_update called !\n");
    fd = open(path, O_RDWR, 0660);
    if (fd != -1) {
        write(fd, "2", 1);
        close(fd);
    } else {
        ALOGD(TAG"fd err = %d ", errno);
    }
    return ret;
}

#define MAX_LINE 256
static int get_fw_info(struct fw_update_module *fw_module)
{
    FILE *file;
    char path[PATH_SIZE]  = {0};

    if (fw_module->index == 0) {
        snprintf(path, sizeof(path), "%s", TP_DEVINFO_PATH);
    } else {
        snprintf(path, sizeof(path), "%s%d", TP_DEVINFO_PATH, fw_module->index);
    }

    file = fopen(path, "r");
    char line[MAX_LINE];
    if (!file) {
        ALOGD(TAG"fail to open: %s\n", path);
        return -1;
    }
    while(fgets(line, MAX_LINE, file)) {
        /*if (!strncmp(line, "Device version:", 15)) {
            sscanf(line, "Device version:\t\t0x%x\n", &(fw_module->fw_ver));
        }*/
        if (!strncmp(line, "Device fw_path:", 14)) {
            sscanf(line, "Device fw_path:\t\t%s\n", fw_module->fw_path);
            break;
        }
    }
    ALOGD(TAG" chip info: 0x%x  fw_path:%s", fw_module->fw_ver, fw_module->fw_path);
    fclose(file);
    return 0;
}

static int fw_replace_process(struct fw_update_module *fw_module, struct firmware_info *fw)
{
    ALOGD(TAG" fw_replace_process called\n");
    return 0;
}

int init_func_tp(struct fw_update_module *fw_module)
{
    ALOGD(TAG"init_func called\n", errno);
    fw_module->booting_upgrade_support = true;
    fw_module->stuck_boot = true;
    fw_module->fw_update = do_fw_update;

    //   fw_module->fw_replace = fw_replace_process;
    //   fw_module->monitor_fwfile_support = true;

    get_fw_info(fw_module);
    return 0;
}

static int wait_tp_ic_probe_done(int support_num)
{
    #define MAX_WAIT_PROBE_TIME   20
    #define WAIT_PROBE_TIME       1
    int wait_cycle = 0;
    int count = 0;
    char path[PATH_SIZE]  = {0};
    int ret = 0;

    ALOGE(TAG" %s called, support_num %d \n", __func__, support_num);

    for (count = 0; count <= support_num; count++) {
        memset(path, 0 , sizeof(path));
        if (count == 0) {
            snprintf(path, sizeof(path), "%s", TP_DEVINFO_PATH);
        } else {
            snprintf(path, sizeof(path), "%s%d", TP_DEVINFO_PATH, count);
        }

        wait_cycle = 0;
        do {
            ret = access(path, F_OK);
            if (ret) {
                sleep(WAIT_PROBE_TIME);
                wait_cycle++;
                ALOGE(TAG"fail to access: %s,err:%s\n", path, strerror(errno));
            } else {
                break;
            }
        } while (wait_cycle < MAX_WAIT_PROBE_TIME);
    }
    return 0;
}

int  get_ic_num_func_tp(void)
{
    int prop_flag;
    char value[PROPERTY_VALUE_MAX] = {'\0'};
    int support_num  = 0;

    prop_flag = property_get("persist.sys.tpic.num", value, NULL);

    ALOGE("get_ic_num_func_tp:%s, prop_flag:%d", value, prop_flag);

    if (prop_flag <= 0) {
        wait_tp_ic_probe_done(0);
        support_num = 1;
    } else {
        if (!strncmp(value, "1", 1)) {
            wait_tp_ic_probe_done(0);
            support_num = 1;
        } else if (!strncmp(value, "2", 1)) {
            wait_tp_ic_probe_done(1);
            support_num = 2;
        } else if (!strncmp(value, "3", 1)) {
            wait_tp_ic_probe_done(2);
            support_num = 3;
        } else {
            wait_tp_ic_probe_done(0);
            support_num = 1;
        }
    }
    ALOGE(TAG" %s called, support_num %d \n", __func__, support_num);

    return support_num;
}

