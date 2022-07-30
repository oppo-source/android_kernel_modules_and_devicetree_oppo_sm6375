/************************************************************************************
 ** File: - vendor\oplus\apps\bspFwUpdate\test\test_update.cpp
 ** 
 ** Copyright (C), 2014-2018, OPLUS Mobile Comm Corp., Ltd
 **
 ** Description:
 **      test module for bspFwUpdate
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
//Runsheng.Pei modify with oplusex to decouple begin: {
//#include "../../../../frameworks/base_oplus/libs/opluscriticallog/ProxyJniEntry.h"
#include "CriticalLogExProxy.h"
//end. }
#include "../bspFwUpdate.h"

#define TAG "[bspFwUpdate][test]"


static int fw_replace_process(struct fw_update_module *fw_module, struct firmware_info *fw)
{
    ALOGD(TAG" fw_replace_process called\n");
    return 0;
}

int init_func_test(struct fw_update_module *fw_module)
{
    fw_module->fw_replace = fw_replace_process;
    fw_module->monitor_fwfile_support = true;
    return 0;
}
