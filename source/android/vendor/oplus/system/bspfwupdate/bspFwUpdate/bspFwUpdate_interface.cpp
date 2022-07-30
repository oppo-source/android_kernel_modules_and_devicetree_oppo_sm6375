/************************************************************************************
 ** File: - vendor\oplus\apps\bspFwUpdate\bspFwUpdate_interface..cpp
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
#include <sys/stat.h>
#include <sys/types.h>
#include <cutils/log.h>
#include <fcntl.h>
#include <cutils/properties.h>
#include "rsaVerify.h"
#include "bspFwUpdate_interface.h"

#define TAG "bspFwUpdate:Interface "

int fd_select_firmware_oplus(struct firmware_info *&fw, int fd_sys, int fd_data, const char *file_name)
{
    int ret = -1;
    struct stat st_fw;
    int update_state = FW_UPDATE_NONE;

    struct signed_head data_header = {0};
    struct signed_head sys_header = {0};
    fw = (struct firmware_info *)malloc(sizeof(struct firmware_info));
    if (!fw) {
        ALOGD(TAG"fw is null\n");
        return FW_UPDATE_NONE;
    }
    if(fd_sys == -1 && fd_data == -1) {
        ALOGD(TAG"data firmware && sys firmware are not exist\n");
        return FW_UPDATE_NONE;
    }

    if (fd_sys != -1) {
        ALOGD(TAG"open fd_sys success, going to read sys_header\n");
        ret = read(fd_sys, &sys_header, HEADER_SIZE);
    }

    if (fd_data != -1) {
        ret = read(fd_data, &data_header, HEADER_SIZE);
        if (!file_name) {
            ALOGD(TAG"file_name is NULL\n");
            goto Exit_Data;
        }
        if (!strstr(file_name, data_header.file_name) || (data_header.magic_num != MAGIC_NUM)) {
            ALOGD(TAG"file:%s != %s magic_num:0x%x\n", file_name, data_header.file_name, data_header.magic_num);
        } else {
            ALOGD(TAG"data_header.fw_ver:%lx sys_header.fw_ver:%lx \n", data_header.fw_ver, sys_header.fw_ver);
            if (data_header.fw_ver > sys_header.fw_ver) {
                if (fstat(fd_data, &st_fw) < 0) {
                    ALOGD(TAG"datafw fstat fail\n");
                    goto Exit_Data;
                }
                if (st_fw.st_size > MAX_FW_SIZE) {
                    ALOGD(TAG"max size exceed %x\n", MAX_FW_SIZE);
                    goto Exit_Data;
                }
                ALOGD(TAG"data update alloc %ld byte \n", st_fw.st_size);
                fw->header = (unsigned char *)malloc(st_fw.st_size);
                if (fw->header == NULL) {
                    ALOGD(TAG"data update alloc %ld byte fail\n", st_fw.st_size);
                    goto Exit_Data;
                }
                lseek(fd_data, 0, SEEK_SET);
                ret = read(fd_data, fw->header, st_fw.st_size);
                if (ret < 0) {
                    ALOGD(TAG"read file %s fail:%d\n", file_name, ret);
                    free(fw->header);
                } else {
                    ret = verify_file_withRSA(fw->header, st_fw.st_size);
                    if (!ret) {
                        fw->data_size = st_fw.st_size - 128 - HEADER_SIZE;
                        fw->data = fw->header + HEADER_SIZE;
                        fw->sign = fw->data + fw->data_size;
                        return FW_UPDATE_DATA;
                    }
                    ALOGD(TAG"Verify %s fail %d\n", file_name, ret);
                }
            } else {
                update_state |= FW_UPDATE_REMOVE;
                ALOGD(TAG"Mark remove data files: %s\n", file_name);
            }
        }
    }

Exit_Data:
    if (fd_sys != -1) {
        if (fstat(fd_sys, &st_fw) < 0) {
            ALOGD(TAG"datafw fstat fail\n");
            return FW_UPDATE_NONE;
        }
        if (st_fw.st_size > MAX_FW_SIZE) {
            ALOGD(TAG"max size exceed %x\n", MAX_FW_SIZE);
            return FW_UPDATE_NONE;
        }
        fw->header = (unsigned char *)malloc(st_fw.st_size);
        if (fw->header == NULL) {
            ALOGD(TAG"sys update alloc %ld byte fail\n", st_fw.st_size);
            return FW_UPDATE_NONE;
        }
        lseek(fd_sys, 0, SEEK_SET);
        ALOGD(TAG"read HEADER_SIZE %d sizeof(long):%d \n", HEADER_SIZE, sizeof(long));
        ret = read(fd_sys, fw->header, st_fw.st_size);
        if (ret < 0) {
            ALOGD(TAG"read file %s fail:%d\n", file_name, ret);
        } else {
            fw->data_size = st_fw.st_size - 128 - HEADER_SIZE;
            fw->data = fw->header + HEADER_SIZE;
            fw->sign = fw->data + fw->data_size;
            update_state |= FW_UPDATE_SYSTEM;
            return update_state;
        }
    }

Exit:
    return FW_UPDATE_NONE;
}

int path_select_firmware_oplus(struct firmware_info *&fw, char *file_sys, char *file_data)
{
    int fd_data = -1;
    int fd_sys = -1;
    int update_state = FW_UPDATE_NONE;

    fd_sys = open(file_sys, O_RDONLY | O_CLOEXEC);
    if (fd_sys != -1) {
        ALOGD(TAG"firmware name:%s\n", file_sys);
    } else {
        ALOGD(TAG"sys firmware %s open fail,errno:%d\n", file_data, errno);
    }

    fd_data = open(file_data, O_RDONLY, 0660);
    if (fd_data != -1) {
        ALOGD(TAG"data firmware:%s found\n", file_data);
    } else {
        ALOGD(TAG"data firmware %s open fail,errno:%d\n", file_data, errno);
    }
    update_state = fd_select_firmware_oplus(fw, fd_sys, fd_data, file_data);

Exit:
    if (fd_data != -1) {
        close(fd_data);
        if(update_state & FW_UPDATE_REMOVE)
            remove(file_data);
    }
    if (fd_sys != -1)
        close(fd_sys);
    update_state &= 0x0F;
    return update_state;
}

void release_firmware_oplus(struct firmware_info *&fw)
{
    if (!fw) {
        ALOGD(TAG"fw is NULL\n");
        return;
    }
    if (!fw->header) {
        free(fw->header);
    }
    free(fw);
    return;
}
