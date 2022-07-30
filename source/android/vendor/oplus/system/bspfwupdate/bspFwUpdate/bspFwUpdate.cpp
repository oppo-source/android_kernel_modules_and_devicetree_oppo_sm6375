/************************************************************************************
 ** File: - vendor\oplus\apps\bspFwUpdate\bspFwUpdate..cpp
 ** 
 ** Copyright (C), 2014-2018, OPLUS Mobile Comm Corp., Ltd
 **
 ** Description:
 **      common part for FwUpdate module
 **
 ** Version: 1.0
 ** Date created: 3/12/2017
 **
 ** --------------------------- Revision History: --------------------------------
 **     <author>    <data>            <desc>
 ************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "bspFwUpdate.h"
#include "bspFwUpdate_interface.h"
#include <pthread.h>

#define TAG "[bspFwUpdate]"

#define MAX_RETRY_CYCLE 7
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

static int fw_comp_check = 0x0;
static int fw_comp_kick = 0x0;
pthread_mutex_t bspFwUpdate_mutex;

/*************Module Config*********/
extern_func(tp);
extern_get_func(tp);
extern_func(fastchg);
extern_get_func(fastchg);
extern_func(ufs);
extern_get_func(ufs);

int (*init_funcs[]) (struct fw_update_module *fw_module) = {
    init_func_tp,
    init_func_fastchg,
    init_func_ufs
};

int (*get_ic_num_funcs[]) (void) = {
    get_ic_num_func_tp,
    get_ic_num_func_fastchg,
    get_ic_num_func_ufs
};

/*************Module Config End*********/

void trigger_check(void)
{
    if (fw_comp_kick == fw_comp_check) {
        ALOGD(TAG"going to set property fw_boot done\n");
        property_set("sys.fw_boot", "progress_done");
    }
}

void stuck_progress_done(struct fw_update_module *temp_module)
{
    ALOGE(TAG" fw_comp_kick[0x%x] fw_comp_check[0x%x] stuck_mask[%d]\n", fw_comp_kick, fw_comp_check, temp_module->stuck_mask);
    pthread_mutex_lock(&bspFwUpdate_mutex);
    fw_comp_kick |= (0x01 << temp_module->stuck_mask);
    pthread_mutex_unlock(&bspFwUpdate_mutex);
    trigger_check();
}

int request_firmware_comp(struct fw_update_module *temp_module, struct firmware_info *&fw)
{
    char *file_data = NULL;
    char *file_sys = NULL;
    int fd_data = -1;
    int fd_sys = -1;
    int ret;
    int i = 0;
    int up_state = FW_UPDATE_NONE;

    ret = asprintf(&file_data, FW_PATH_DATA"%s", temp_module->fw_path);
    if (ret == -1) {
        file_data = NULL;
        ALOGD(TAG"alloc file_data failed\n");
        goto Exit;
    }

    //get file_sys header
    for (i = 0; i < ARRAY_SIZE(firmware_dirs_system); i++) {
        ret = asprintf(&file_sys, "%s/%s", firmware_dirs_system[i], temp_module->fw_path);
        if (ret == -1) {
            file_sys = NULL;
            ALOGD(TAG"asprintf file_sys failed\n");
        } else {
            fd_sys = open(file_sys, O_RDONLY | O_CLOEXEC);
            if (fd_sys != -1) {
                ALOGD(TAG"firmware found:%s\n", file_sys);
                break;
            }
        }
    }
    fd_data = open(file_data, O_RDONLY, 0660);
    if (fd_data != -1) {
        ALOGD(TAG"data firmware:%s found\n", file_data);
    } else {
        ALOGD(TAG"data firmware %s open fail,errno:%d\n", file_data, errno);
    }
    up_state = fd_select_firmware_oplus(fw, fd_sys, fd_data, file_data);
    if (fd_data != -1) {
        close(fd_data);
        if(up_state & FW_UPDATE_REMOVE)
            remove(file_data);
    }

    if (fd_sys != -1)
        close(fd_sys);
    up_state &= 0x0F;

    if (FW_UPDATE_SYSTEM == up_state) {
        if (file_sys) {
            strncpy(temp_module->up_path, file_sys, PATH_SIZE - 1);
        }
    } else if (FW_UPDATE_DATA == up_state) {
        if (file_data) {
            strncpy(temp_module->up_path, file_data, PATH_SIZE - 1);
        }
    }

    if (file_sys) {
        free(file_sys);
        file_sys = NULL;
    }
    if (file_data) {
        free(file_data);
        file_sys = NULL;
    }
Exit:
    return up_state;
}

int detect_data_firmware(struct fw_update_module *temp_module, struct firmware_info *&fw)
{
    char *file_data = NULL;
    int fd_data = -1;
    int ret;
    struct stat st_fw;
    struct signed_head data_header = {0};
    char prop_char[PROPERTY_VALUE_MAX] = {'\0'};
    int wait_cycle = 0;

    fw = (struct firmware_info *)malloc(sizeof(struct firmware_info));
    if (!fw) {
        ALOGD(TAG"fw is null\n");
        return FW_UPDATE_NONE;
    }

    //Start_compare:
    //get file_data header
    ret = asprintf(&file_data, FW_PATH_DATA"%s", temp_module->fw_path);
    if (ret == -1) {
        ALOGD(TAG"asprintf file_data failed\n");
        goto Exit;
    } else {
        do {
            sleep(2);
            fd_data = open(file_data, O_RDWR, 0660);
            if (fd_data == -1) {
                property_get("sys.fw_boot", prop_char, NULL);
                if (!strncmp(prop_char, "sau_start", 9)) {
                    ALOGD(TAG"SAU is in progress ,errno=%d\n", errno);
                } else {
                    ALOGD(TAG"Other scene only retry once\n");
                    wait_cycle += MAX_RETRY_CYCLE - 2;
                }
                ALOGE(TAG"open fail,wait 1s cycle:%d\n", wait_cycle);
            } else {
                break;
            }
            wait_cycle++;
        } while(wait_cycle < MAX_RETRY_CYCLE);

        if (fd_data != -1) {
            //strncpy(temp_module->fw_path, file_data, PATH_SIZE-1);
            strncpy(temp_module->up_path, file_data, PATH_SIZE - 1);
            free(file_data);
            ALOGD(TAG"HEADER_SIZE %lu\n", HEADER_SIZE);
            ret = read(fd_data, &data_header, HEADER_SIZE);
            if (!strstr(temp_module->fw_path, data_header.file_name) || data_header.magic_num != MAGIC_NUM) {
                ALOGD(TAG"file:%s != %s magic_num:0x%x\n", temp_module->fw_path, data_header.file_name, data_header.magic_num);
            } else {
                ALOGD(TAG"data_header.fw_ver:%x temp_module->fw_ver:%x\n", data_header.fw_ver, temp_module->fw_ver);
                if (data_header.fw_ver > temp_module->fw_ver) {
                    if (fstat(fd_data, &st_fw) < 0) {
                        ALOGD(TAG"datafw fstat fail\n");
                        goto Exit;
                    }
                    if (st_fw.st_size > MAX_FW_SIZE) {
                        ALOGD(TAG"max size exceed %x\n", MAX_FW_SIZE);
                        goto Exit;
                    }
                    ALOGD(TAG"data update alloc %d byte \n", st_fw.st_size);
                    fw->header = (unsigned char *)malloc(st_fw.st_size);
                    if (fw->header == NULL) {
                        ALOGD(TAG"data update alloc %d byte fail\n", st_fw.st_size);
                        goto Exit;
                    }
                    lseek(fd_data, 0, SEEK_SET);
                    ret = read(fd_data, fw->header, st_fw.st_size);
                    if (ret < 0) {
                        ALOGD(TAG"read file %s fail:%d\n", temp_module->fw_path, ret);
                    } else {
                        ret = verify_file_withRSA(fw->header, st_fw.st_size);
                        if (!ret) {
                            fw->data_size = st_fw.st_size - 128 - HEADER_SIZE;
                            fw->data = fw->header + HEADER_SIZE;
                            fw->sign = fw->data + fw->data_size;
                            close(fd_data);
                            if (temp_module->fw_replace) {
                                ret = temp_module->fw_replace(temp_module, fw);
                                if (ret) {
                                    ALOGD(TAG"%s:fw_update fail\n", temp_module->name);
                                } else {
                                    temp_module->fw_ver = data_header.fw_ver;
                                    ALOGD(TAG"%s:fw_replace success\n", temp_module->name);
                                }
                            }
                            return FW_UPDATE_DATA;
                        }
                    }
                }
            }
        } else {
            ALOGD(TAG"file:%s open fail,%d\n", file_data, errno);
            free(file_data);
        }
    }
Exit:
    if (fd_data != -1)
        close(fd_data);
    return FW_UPDATE_NONE;
}

void mkdirs(char *path)
{
    int i, len;
    char str[256];
    strncpy(str, path, 256);
    len = strlen(str);
    ALOGD(TAG"Going to detect %s\n", str);
    for (i = 0; i < len; i++) {
        if (str[i] == '/') {
            str[i] = '\0';
            if (access(str, F_OK) != 0) {
                ALOGD(TAG"Going to create %s\n", str);
                mkdir(str, 0777);
            }
            str[i] = '/';
        }
    }
    if (len > 0 && access(str, F_OK) != 0) {
        mkdir(str, 0777);
    }

    return;
}

void monitor_fw_path(struct fw_update_module *temp_module)
{
    int ino_wd;
    int ret;
    int nread = 0;
    int len;
    char buf[1024] = {0};
    char tmp_path[PATH_SIZE] = {0};
    char *tmp_ptr = NULL;
    char *dentry_monitor;
    int fd_watch = inotify_init();
    struct firmware_info *fw = NULL;
    struct inotify_event *event;
    int up_state = FW_UPDATE_NONE;

    tmp_ptr = strrchr(temp_module->fw_path, '/');
    if (tmp_ptr == NULL)
        ALOGD(TAG"%s: base dir NULL\n", temp_module->name);
    else
        strncpy(tmp_path, temp_module->fw_path, tmp_ptr - temp_module->fw_path);

    ret = asprintf(&dentry_monitor, FW_PATH_DATA"%s", tmp_path);
    mkdirs(dentry_monitor);
    ino_wd = inotify_add_watch(fd_watch, dentry_monitor, IN_MODIFY | IN_MOVED_TO);
    if (ino_wd < 0) {
        ALOGD(TAG"inotify add watch fail %s\n", dentry_monitor);
    }
    while(1) {
        ALOGD(TAG"%s,monitor read begin\n", temp_module->name);
        len = read(fd_watch, buf, 1024 - 1);
        event = (struct inotify_event *)&buf[nread];
        if (len < 0) {
            ALOGD(TAG"inotify read error\n");
        } else {
            while(len > 0) {
                ALOGD(TAG"%s len:%d--- mask:%d\n", event->name, len, event->mask);
                nread = nread + sizeof(struct inotify_event) + event->len;
                len = len - sizeof(struct inotify_event) - event->len;
                if (strstr(temp_module->fw_path, event->name)) {
                    up_state = detect_data_firmware(temp_module, fw);
                    release_firmware_oplus(fw);
                    break;
                }
            }
            nread = 0;
        }
    }
    inotify_rm_watch(fd_watch, ino_wd);
    close(fd_watch);
    free(dentry_monitor);
}

void do_fw_update(struct fw_update_module *temp_module)
{
    int ret = -1;
    struct firmware_info *fw = NULL;
    struct signed_head *sign_header = NULL;
    int up_state = FW_UPDATE_NONE;

    //do fw update during bootup
    ALOGD(TAG"fw_update get in \n");
    up_state = request_firmware_comp(temp_module, fw);
    temp_module->up_state = up_state;
    if (fw == NULL ) {
        if (up_state == FW_UPDATE_NONE)
            ALOGD(TAG"%s:no need to update\n", temp_module->name, errno);
        else
            ALOGD(TAG"verify files fail %d\n", errno);
    } else {
        switch (up_state) {
        case FW_UPDATE_SYSTEM:
        case FW_UPDATE_DATA:
            if (temp_module->fw_update) {
                sign_header = (struct signed_head *)fw->header;
                if (sign_header->fw_ver != temp_module->fw_ver) {
                    ret = temp_module->fw_update(temp_module, fw);
                    if (ret) {
                        ALOGD(TAG"%s:fw_update fail\n", temp_module->name, errno);
                    } else {
                        temp_module->fw_ver = sign_header->fw_ver;
                        ALOGD(TAG"%s:fw_update success\n", temp_module->name, errno);
                    }
                } else {
                    ALOGD(TAG"%s:fw version same, skip update\n", temp_module->name);
                }
            }
            break;
        default:
            break;
        }
        release_firmware_oplus(fw);
    }
}

void *thread_fw_upgrade(void *data)
{
    struct fw_update_module *temp_module = (struct fw_update_module *)data;
    ALOGD(TAG" thread_fw_upgrade called !\n");
    if (!temp_module) {
        ALOGD(TAG" temp_module NULL !\n");
        goto Exit;
    }
    if (temp_module->booting_upgrade_support)
        do_fw_update(temp_module);

    if (temp_module->stuck_boot)
        stuck_progress_done(temp_module);

    /*do monitor fw change*/
    if (temp_module->monitor_fwfile_support)
        monitor_fw_path(temp_module);

    free(temp_module);
Exit:
    return NULL;
}

int main(int argc, char **argv)
{
    int ret;
    int prop_flag;
    char value[PROPERTY_VALUE_MAX] = {'\0'};
    struct fw_update_module *temp_module;
    char stuck_mask = 0;
    int count = 0;
    int index = 0;
    int support_ic_num = 0;

    ALOGE(TAG" main called\n");
    ret = pthread_mutex_init(&bspFwUpdate_mutex, NULL);
    if (ret) {
        ALOGE(TAG" bspFwUpdate_mutex create failed[%d]\n", ret);
    }
    prop_flag = property_get("sys.fw_boot", value, NULL);

    ALOGE(TAG" sizeof(init_funcs):%d/sizeof(int *) %d main called\n", sizeof(init_funcs), sizeof(int *));
    for (count = 0; count < sizeof(init_funcs) / sizeof(int *); count++) {
        if (!init_funcs[count]) {
            ALOGD(TAG" module should contain init_func\n");
            continue;
        }

        if (!get_ic_num_funcs[count]) {
            ALOGD(TAG" module should contain get_ic_num_funcs\n");
            support_ic_num = 1;
        } else {
            support_ic_num = get_ic_num_funcs[count]();
        }
        for (index = 0; index < support_ic_num; index ++) {
            temp_module = ( struct fw_update_module *)malloc(sizeof(struct fw_update_module));
            memset(temp_module, 0, sizeof(struct fw_update_module));
            temp_module->index = index;
            temp_module->init_func = init_funcs[count];
            ret = temp_module->init_func(temp_module);
            if (ret) {
                free(temp_module);
                continue;
            }
            if (prop_flag <= 0) {
                if (temp_module->stuck_boot) {
                    ALOGD(TAG"temp_module %s should stuck boot! stuck_mask:%d\n", temp_module->name, stuck_mask);
                    temp_module->stuck_mask = stuck_mask;
                    fw_comp_check |= (0x01 << stuck_mask);
                    if (fw_comp_check == 0x01) {
                        ALOGD(TAG"property set sys.fw_boot progress_satrt\n");
                        property_set("sys.fw_boot", "progress_start");
                    }
                    stuck_mask++;
                }
            } else {
                ALOGD(TAG"%s, property sys.fw_boot already set\n", temp_module->name);
                temp_module->stuck_boot = false;
                temp_module->booting_upgrade_support = false;
            }
            ret = pthread_create(&temp_module->ntid, NULL, thread_fw_upgrade, temp_module);
            if (ret != 0) {
                ALOGD(TAG"create thread for %s error !\n", temp_module->name);
                free(temp_module);
                goto Exit;
            }
        }
    }
Exit:
    trigger_check();
    pthread_exit(NULL);
    pthread_mutex_destroy(&bspFwUpdate_mutex);
    return 0;
}
