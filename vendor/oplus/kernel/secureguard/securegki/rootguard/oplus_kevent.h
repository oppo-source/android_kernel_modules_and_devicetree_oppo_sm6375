/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */
#ifndef _OPLUS_KEVENT_H
#define _OPLUS_KEVENT_H

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <linux/version.h>



struct kernel_packet_info
{
    int type;	 /* 0:root,1:only string,other number represent other type */
    char log_tag[32];	/* logTag */
    char event_id[20];	  /*eventID */
    size_t payload_length;	  /* Length of packet data */
    unsigned char payload[0];	/* Optional packet data */
}__attribute__((packed));

int kevent_send_to_user(struct kernel_packet_info *userinfo);

#endif
