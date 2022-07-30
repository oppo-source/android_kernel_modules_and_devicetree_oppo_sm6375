#ifndef _BSP_FW_UPDATE_INTERFACE_H_
#define _BSP_FW_UPDATE_INTERFACE_H_

#include "rsaVerify.h"
#include <errno.h>

struct firmware_info {
    unsigned char *header;
    unsigned char *data;
    unsigned char *sign;
    int  data_size;
};

#pragma pack(push)
#pragma pack(4)
struct signed_head {
    int magic_num;
    long fw_ver;
    int origin_file_size;
    int sign_size;
    char file_name[60];
};
#pragma pack(pop)

typedef enum {
    FW_UPDATE_NONE = 0x00,
    FW_UPDATE_SYSTEM = 0x01,
    FW_UPDATE_DATA = 0x02,
    FW_UPDATE_REMOVE = 0x80,
} fw_update_state;

int path_select_firmware_oplus(struct firmware_info *&fw, char *file_sys, char *file_data);
int fd_select_firmware_oplus(struct firmware_info *&fw, int fd_sys, int fd_data, const char *file_name);

void release_firmware_oplus(struct firmware_info *&fw_op);

#define HEADER_SIZE sizeof(struct signed_head)
#define MAX_FW_SIZE 0x5000000 /*10M*/
#define MAGIC_NUM 0x70557746
#define PATH_SIZE 60

#endif

