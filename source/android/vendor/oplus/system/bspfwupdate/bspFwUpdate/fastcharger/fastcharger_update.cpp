/************************************************************************************
 ** File: - vendor\oplus\apps\bspFwUpdate\fastcharger\fastcharger_update..cpp
 ** 
 ** Copyright (C), 2014-2018, OPLUS Mobile Comm Corp., Ltd
 **
 ** Description:
 **      fastcharger part for FwUpdate module
 **
 ** Version: 1.0
 ** Date created: 7/1/2018
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
//Runsheng.Pei modify with oplusex to decouple begin: {
//#include "../../../../frameworks/base_oplus/libs/opluscriticallog/ProxyJniEntry.h"
#include "CriticalLogExProxy.h"
//end. }
#include "../bspFwUpdate.h"

#define TAG "[bspFwUpdate][fastchg]"

static int do_fw_update(struct fw_update_module *fw_module, struct firmware_info *fw)
{
    int fd = -1;
    int ret = 0;
    //    ALOGD("%d\n", mkdir("/data/oplus/fw_update/mkdir1/mkdir2/mkdir3", 0777));
    ALOGD(TAG" [fastchg] do_fw_update called !\n");
    fd = open("/proc/fastchg_fw_update", O_RDWR, 0660);
    if (fd != -1) {
        write(fd, "1", 1);
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
    file = fopen("/proc/devinfo/fastchg", "r");
    char line[MAX_LINE];
    if (!file) {
        ALOGD(TAG"fail to open: %s\n", "/proc/devinfo/fastchg");
        return -1;
    }
    while(fgets(line, MAX_LINE, file)) {
        if (!strncmp(line, "Device version:", 15)) {
            sscanf(line, "Device version:\t\t%d\n", &(fw_module->fw_ver));
        }
        if (!strncmp(line, "Device manufacture:", 18)) {
            sscanf(line, "Device manufacture:\t\t%s\n", fw_module->fw_path);
            break;
        }
    }
    ALOGD(TAG" chip info: 0x%x  fw_path:%s", fw_module->fw_ver, fw_module->fw_path);
    fclose(file);
    return 0;
}


int init_func_fastchg(struct fw_update_module *fw_module)
{
    ALOGD(TAG"init_func called\n", errno);
    fw_module->booting_upgrade_support = true;
    fw_module->stuck_boot = true;
    fw_module->fw_update = do_fw_update;

    get_fw_info(fw_module);
    return 0;
}

int  get_ic_num_func_fastchg(void)
{
    return 1;
}

