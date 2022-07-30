/************************************************************************************
 ** File: -pub_key.h
 ** 
 ** Copyright (C), 2014-2018, OPLUS Mobile Comm Corp., Ltd
 **
 ** Description:
 **      common part for FwUpdate module
 **
 ** Version: 1.0
 ** Date created: 3/12/2020
 **
 ** --------------------------- Revision History: --------------------------------
 **     <author>    <data>            <desc>
 ************************************************************************************/
#ifndef _PUB_KEY_H_
#define _PUB_KEY_H_
#include <cutils/properties.h>

#define ARRAY_SIZE(a) (sizeof((a)) / sizeof((a)[0]))
#define PLATFORM_KEY_LEN 300

struct public_key_info {
    char platform_name[PROPERTY_VALUE_MAX];
    unsigned char platform_key[PLATFORM_KEY_LEN];
};

int get_public_key(struct public_key_info *p_public_key_info);

#endif
