/************************************************************************************
 ** File: - pub_key.cpp
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
#include <unistd.h>
#include <string.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <sys/types.h>
#include "pub_key.h"

/*
add the different platform key
*/
const struct public_key_info public_key_info_arr[] = {
/*************Qualcomm*********************************************/
{
    "kona",/*SM8250*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDMYyJUOd0HWc+unxl/VKUUNq1W\n"
    "M4upHb63x+RF1kMFkJNJUyLIaoXJyAvoM3W34qtOkI+PX3MaDrYNfNqgT/g47V2D\n"
    "UDl2/cDWG3PhMHTUuB6ffJoSu5IkAYgYtIudF2guzvTB4/ZABAbElHjS2GnJ5sOm\n"
    "Vgdio4hi3CecwMCIXQIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "atoll",/*SM7125*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCaCJgslJfJPr65IvOSB6OsbBfr\n"
    "Qs3pz5IdsjD/26NXXQ5MwTBTkrgh13CIrHi/Q9QbTvJT345EC5XwKshAw4ct6UPY\n"
    "sRfDuzqV37rL5SmR4CZ2Z4P0kyLbF6PVmxRa73cFOYzqnhxDeo6ufAzS8iIoTrtf\n"
    "67Nbv8H8EKS9rNAGUwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "lito",/*SM7250*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCaCJgslJfJPr65IvOSB6OsbBfr\n"
    "Qs3pz5IdsjD/26NXXQ5MwTBTkrgh13CIrHi/Q9QbTvJT345EC5XwKshAw4ct6UPY\n"
    "sRfDuzqV37rL5SmR4CZ2Z4P0kyLbF6PVmxRa73cFOYzqnhxDeo6ufAzS8iIoTrtf\n"
    "67Nbv8H8EKS9rNAGUwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "trinket", /* SM6125 */
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDk8eFsZ5nz66HVehpJoyXa5XNH\n"
    "8DdsPjYaw3sj8MvCtvGcAYFTgw/J4zMnupGItyI46qWHn/80hSKFi3pl5rG+57aj\n"
    "8Fq+e/V7mGpk30djWfLYa5ZO9OjftJDJg6yzsiL6frSAunnFpQ8lahT2su9pG1eR\n"
    "VKwIXp5Kwx8VAk0axwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
/*************MTK*************************************************/
{
    "mt6779",
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDuAV7TxYrOsYh1fpavb2SApXLU\n"
    "8Mcx1ke/ym2SvHG/6lLuZ4Fi4tKD31aUS4lidcVQkftSpxEMa4Tm3VRMFxoAba/Y\n"
    "GqOfVue+C3t3rjTmHbYneksTUaXlRlbyIbHgwT3HxxDm6UJE9sfKWaRcml4050hx\n"
    "yUywbLE8e6uzXru81wIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6771",/*P70*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDuAV7TxYrOsYh1fpavb2SApXLU\n"
    "8Mcx1ke/ym2SvHG/6lLuZ4Fi4tKD31aUS4lidcVQkftSpxEMa4Tm3VRMFxoAba/Y\n"
    "GqOfVue+C3t3rjTmHbYneksTUaXlRlbyIbHgwT3HxxDm6UJE9sfKWaRcml4050hx\n"
    "yUywbLE8e6uzXru81wIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6873",/*MTK5G 6873*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6853",/*MTK5G 6853*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6885",/*MTK5G 6885*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6768",/*MTK for pascal*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6893",/*MTK5G 6893*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "msmnile",/*SM8150*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDMYyJUOd0HWc+unxl/VKUUNq1W\n"
    "M4upHb63x+RF1kMFkJNJUyLIaoXJyAvoM3W34qtOkI+PX3MaDrYNfNqgT/g47V2D\n"
    "UDl2/cDWG3PhMHTUuB6ffJoSu5IkAYgYtIudF2guzvTB4/ZABAbElHjS2GnJ5sOm\n"
    "Vgdio4hi3CecwMCIXQIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "sm6150",/*SM7150*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCiMQj5r/QH78wuxrNdWAPU18px\n"
    "XN7bb0VJuPO4lg4dQtYSokWJTJMBdhil6Vs9xU05Kn/nuu+zaHhPxDi8iVzIMFqK\n"
    "PSfbxHYVbiqYxIQDRc4w6kFeKDhBXuWaZ7MBEcKS3CmmFEuk1DevlX/2XcdGRkqP\n"
    "mW2W/yGlNEs6opBhbwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6785",/*MT6785*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
{
    "mt6765",/*MT6765*/
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDfHGrkIhvoNy9TGFMmAWTsvoyf\n"
    "b4Tz4Y3iZjo/hXV2g5BGztRhJ8Oab3kFE1WvIVo6cuBwPGmUyIqV4lZCaODuCoRu\n"
    "cDLVk0Hc2i6rqEEBT9v/4BGJ6rHnzg48i503GxI9zq05eiAeRNSK/nv5jmWuWpsp\n"
    "Gb+l7Vo1UHdytTp2iwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
},
};

/*
return 0--success, other--fail
*/
int get_public_key(struct public_key_info *p_public_key_info)
{
    int ret = -1;
    int i = 0;

    if (!p_public_key_info) {
        ALOGE("Para p_public_key_platform is null");
        return -1;
    }
    for (i = 0; i < ARRAY_SIZE(public_key_info_arr); i ++) {
        if (!strcmp(public_key_info_arr[i].platform_name, p_public_key_info->platform_name)) {
            memcpy(p_public_key_info, &public_key_info_arr[i], sizeof(public_key_info_arr[i]));
            return 0;
        }
    }
    ret = -1;
    ALOGE("%s platform is not found,using default pubilc key", p_public_key_info->platform_name);
    memcpy(p_public_key_info->platform_key, &public_key_info_arr[0].platform_key,
            sizeof(public_key_info_arr[0].platform_key));
    return ret;
}
