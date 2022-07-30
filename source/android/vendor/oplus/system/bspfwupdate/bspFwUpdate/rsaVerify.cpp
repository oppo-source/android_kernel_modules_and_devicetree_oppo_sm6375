/************************************************************************************
 ** File: - vendor\oplus\apps\bspFwUpdate\rsaVerify..cpp
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
#include <sys/stat.h>
#include <sys/types.h>

#include "bspFwUpdate.h"
#include "rsaVerify.h"
#include "pub_key.h"

#define SIGNED_MSG_LEN 128
#define HASH_SIZE_SHA256 32

//------------------------------------------------------
void print_hex(char *buff)
{
    for (int i = 0; buff[i]; i++)
        ALOGD(":%02x", (unsigned char)buff[i]);
    ALOGD("\n");
}

/*
return 0--success, other--fail
*/
#define PLATFORM_NAME_PROP  "ro.board.platform"

int rsa_veryfy_fw(unsigned char *origintext, unsigned char *verifiedText, int origin_size)
{
    RSA  *pubRsa = NULL;
    int ret;
    int keyLen;
    size_t out_len;
    unsigned char text_discrypto[256] = {0};
    unsigned char text_hash_origin[256] = {0};
    BIO *bio = NULL;
    struct public_key_info public_key_platform;

    //step1:rsa verify
    if (!verifiedText) {
        ALOGD("Invalid plaintext\n");
        return -1;
    }
    memset(&public_key_platform, 0, sizeof(struct public_key_info));
    property_get(PLATFORM_NAME_PROP, public_key_platform.platform_name, "SM8250");
    ALOGE("rsa_veryfy_fw: platform_name: %s", public_key_platform.platform_name);

    get_public_key(&public_key_platform);

    bio = BIO_new_mem_buf((void *)public_key_platform.platform_key, -1);
    if (bio)
        pubRsa = PEM_read_bio_RSA_PUBKEY(bio, NULL, NULL, NULL);

    if (pubRsa == NULL) {
        ERR_print_errors_fp(stdout);
        ALOGD("get RSA public key fail\n");
        ret = -EINVAL;
        goto VERIFY_EXIT;
    }
    keyLen = RSA_size(pubRsa);
    ALOGD("keyLen:%d\n", keyLen);
    ret = RSA_verify_raw(pubRsa, &out_len, (unsigned char *)text_discrypto, keyLen, verifiedText, SIGNED_MSG_LEN, RSA_PKCS1_PADDING);
    RSA_free(pubRsa);

    if (ret != 1) {
        ERR_print_errors_fp(stdout);
        ALOGD("RSA_verify fail\n");
        ret = 1;
        goto VERIFY_EXIT;
    } else {
        ALOGD("RSA_verify success\n");
    }

    //step2:sha256 verify
    SHA256((uint8_t *)origintext, origin_size, (uint8_t *)text_hash_origin);
    ALOGD("sha256 testing\n");
    ret = memcmp(text_discrypto, text_hash_origin, HASH_SIZE_SHA256);
    if (ret) {
        print_hex((char *)text_hash_origin);
        ALOGD(" SHA256 verify failed\n");
        print_hex((char *)text_discrypto);
    }
VERIFY_EXIT:
    if(bio)
        BIO_free(bio);
    return ret;
}

/*
return 0--success, other--fail
*/
int verify_file_withRSA(unsigned char *&header, int size)
{
    int ret = -1;
    unsigned char *signedtext;
    unsigned char *origintext;
    /*print_hex((char*)read_buf);*/
    ALOGD("going to test rsa verify\n");
    origintext = header;
    signedtext = &header[size - SIGNED_MSG_LEN];
    ret = rsa_veryfy_fw(origintext, signedtext, size - SIGNED_MSG_LEN);
    return ret;
}
