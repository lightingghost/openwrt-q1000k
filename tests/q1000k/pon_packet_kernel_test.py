#!/usr/bin/env python3
"""Generate an isolated UML test of raw PON RX through a Linux packet socket."""
from pathlib import Path
import re

repo = Path(__file__).resolve().parents[2]
print(r'''
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/bitfield.h>
#include <linux/if_packet.h>
#include <linux/net.h>
#include <net/sock.h>
#include <linux/utsname.h>
#ifndef CONFIG_UML
#error This test is for a disposable UML guest only.
#endif
int q1000k_pwan_rx_prepare(struct sk_buff *skb, const void *msg,
                         unsigned int msg_len, unsigned int packet_len);
void q1000k_pwan_rx_protocol(struct sk_buff *skb, struct net_device *dev, bool omci);
''')
source = (repo / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/src/q1000k_packet.c').read_text()
print(re.sub(r'^#include[^\n]*\n', '', source, flags=re.M))
print(r'''
static netdev_tx_t drop_tx(struct sk_buff *skb, struct net_device *dev)
{
    dev_kfree_skb_any(skb); return NETDEV_TX_OK;
}
static const struct net_device_ops ops={ .ndo_start_xmit=drop_tx };
static int receive_frame(struct socket *sock,struct net_device *dev,bool omci,
                         unsigned int len,bool fragmented)
{
    unsigned char expected[64],received[80];
    struct msghdr msg={0};
    struct kvec vec={.iov_base=received,.iov_len=sizeof(received)};
    u32 words[4]={omci?BIT(8)|BIT(30):0,0,0x01000000,0};
    struct sk_buff *skb;
    unsigned int head=fragmented?4:len,i;
    int ret;
    for(i=0;i<len;i++) expected[i]=i;
    if(omci) {
        expected[3]=len==48?0x0a:0x0b;
        expected[8]=expected[9]=0; /* Extended frame with no content. */
        if(len>=14) { expected[12]=0x88; expected[13]=0x8e; }
    } else {
        memcpy(expected,dev->dev_addr,ETH_ALEN);
        expected[12]=0x88; expected[13]=0x8e;
    }
    skb=alloc_skb(128,GFP_KERNEL);
    if(!skb) return -ENOMEM;
    skb_put_data(skb,expected,head);
    if(fragmented) {
        struct page *page=alloc_page(GFP_KERNEL);
        if(!page) { kfree_skb(skb); return -ENOMEM; }
        memcpy(page_address(page),expected+head,len-head);
        skb_add_rx_frag(skb,0,page,0,len-head,PAGE_SIZE);
    }
    ret=q1000k_pwan_rx_prepare(skb,words,sizeof(words),len);
    if(ret) { kfree_skb(skb); return ret; }
    if(skb_is_nonlinear(skb)) { kfree_skb(skb); return -EINVAL; }
    q1000k_pwan_rx_protocol(skb,dev,omci);
    netif_rx(skb); /* Consumed, delivered by the kernel backlog/packet taps. */
    ret=kernel_recvmsg(sock,&msg,&vec,1,sizeof(received),0);
    if(ret!=len || memcmp(expected,received,len)) return -EBADMSG;
    return 0;
}
static int __init pon_packet_test_init(void)
{
    struct socket *sock=NULL;
    struct net_device *dev;
    struct sockaddr_ll address={.sll_family=AF_PACKET,.sll_protocol=htons(ETH_P_ALL)};
    bool registered=false;
    int ret;
    if(!strstr(init_utsname()->release,"-q1000k-pon-packet-test")) return -EPERM;
    dev=alloc_netdev(0,"ponframe%d",NET_NAME_UNKNOWN,ether_setup);
    if(!dev) return -ENOMEM;
    dev->netdev_ops=&ops; eth_hw_addr_random(dev);
    ret=register_netdev(dev); if(ret) goto out; registered=true;
    rtnl_lock(); ret=dev_open(dev,NULL); rtnl_unlock(); if(ret) goto out;
    ret=sock_create_kern(&init_net,AF_PACKET,SOCK_RAW,htons(ETH_P_ALL),&sock);
    if(ret) goto out;
    address.sll_ifindex=dev->ifindex;
    ret=kernel_bind(sock,(struct sockaddr *)&address,sizeof(address)); if(ret) goto out;
    sock->sk->sk_rcvtimeo=msecs_to_jiffies(1000);
    ret=receive_frame(sock,dev,true,48,false); if(ret) goto out;
    ret=receive_frame(sock,dev,true,48,true); if(ret) goto out;
    ret=receive_frame(sock,dev,true,14,true); if(ret) goto out;
    ret=receive_frame(sock,dev,false,60,true);
out:
    if(sock) sock_release(sock);
    if(registered) unregister_netdev(dev);
    free_netdev(dev);
    if(ret) pr_err("Q1000K_PON_PACKET_KERNEL_FAIL error=%d\n",ret);
    else pr_info("Q1000K_PON_PACKET_KERNEL_PASS raw_omci=3 ethernet=1\n");
    return ret;
}
static void __exit pon_packet_test_exit(void) {}
module_init(pon_packet_test_init);
module_exit(pon_packet_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K raw RX packet framing test");
''')
