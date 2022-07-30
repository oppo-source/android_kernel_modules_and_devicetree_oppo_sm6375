#ifndef _BSP_FW_RSAVERIFY_H_
#define _BSP_FW_RSAVERIFY_H_

#include <openssl/sha.h>
//#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include "pub_key.h"
#define SIGNED_MSG_LEN 128
#define HASH_SIZE_SHA256 32
int verify_file_withRSA(unsigned char *&header, int size);

#endif

