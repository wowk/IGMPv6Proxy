#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <error.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <linux/bpf.h>


struct fdb_t {

};

struct port_t {
    int rawsock;
    uint8_t ifindex;
    char ifname[IF_NAMESIZE];
    struct sockaddr_in6 addr;
    bool join_node_router_group;
    bool join_link_router_group;
    bool join_site_router_group;
};

struct icmp6_proxy_t {
    struct port_t wan;
    struct port_t lan;
    struct fdb_t  fdb;
    uint32_t max_entrys;
    uint32_t aging_time;
    uint32_t timeout;
    volatile bool running;
};

static int join_multicast(struct port_t* port, const char* mc_group)
{
    struct ipv6_mreq mreq;

    mreq.ipv6mr_interface   = port->ifindex;
    inet_pton(AF_INET6, mc_group, &mreq.ipv6mr_multiaddr);

    if( 0 > setsockopt(port->rawsock, SOL_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) ){
        error(0, errno, "failed to join multicast group %s", mc_group);
        return -errno;
    }

    return 0;
}

static int leave_multicast(struct port_t* port, const char* mc_group)
{
    struct ipv6_mreq mreq;

    mreq.ipv6mr_interface   = port->ifindex;
    inet_pton(AF_INET6, mc_group, &mreq.ipv6mr_multiaddr);

    if( 0 > setsockopt(port->rawsock, SOL_IPV6, IPV6_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) ){
        error(0, errno, "failed to leave multicast group %s", mc_group);
        return -errno;
    }

    return 0;
}

static int create_icmpv6_sock(struct port_t* port)
{
    port->rawsock = socket(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if( port->rawsock < 0 ){
        error(0, errno, "failed to create ICMPV6 socket");
        return -errno;
    }

    if( 0 > setsockopt(port->rawsock, SOL_SOCKET, SO_BINDTODEVICE, port->ifname, sizeof(port->ifname)) ){
        error(0, errno, "failed to bind socket to device %s", port->ifname);
        close(port->rawsock);
        return -errno;
    }

    int pktinfo_on = 1;
    if( 0 > setsockopt(port->rawsock, SOL_IPV6, IPV6_RECVPKTINFO, &pktinfo_on, sizeof(pktinfo_on)) ){
        error(0, errno, "failed to set RECVPKTINFO flag");
        close(port->rawsock);
        return -errno;
    }

//    if( !port->join_node_router_group && 0 == join_multicast(port, "ff01::2") ){
//        port->join_node_router_group = true;
//    }

    if( !port->join_link_router_group && 0 == join_multicast(port, "ff02::2") ){
        port->join_link_router_group = true;
    }

    if( !port->join_site_router_group && 0 == join_multicast(port, "ff02::/16") ){
        port->join_site_router_group = true;
    }

    return 0;
}

static ssize_t recv_icmpv6_pkt(struct port_t* port, void* buf, size_t len,
                           struct in6_addr* from, struct in6_addr* to)
{
    ssize_t ret;
    uint8_t cbuf[sizeof(struct in6_pktinfo) + sizeof(struct cmsghdr)];
    struct msghdr msghdr;
    struct iovec iovec;
    struct sockaddr_in6 si6;

    memset(&cbuf, 0, sizeof(cbuf));
    iovec.iov_base          = buf;
    iovec.iov_len           = len;
    msghdr.msg_iovlen       = 1;
    msghdr.msg_iov          = &iovec;
    msghdr.msg_control      = cbuf;
    msghdr.msg_controllen   = sizeof(cbuf);
    msghdr.msg_flags        = 0;
    msghdr.msg_name         = &si6;
    msghdr.msg_namelen      = sizeof(si6);

    do{
        ret = recvmsg(port->rawsock, &msghdr, 0);
    }while( ret > 0 && errno == EINTR);

    if( ret > 0 ){
        struct in6_pktinfo* pktinfo = (struct in6_pktinfo*)CMSG_DATA((struct cmsghdr*)cbuf);
        memcpy(to, &pktinfo->ipi6_addr, sizeof(pktinfo->ipi6_addr));
        memcpy(from, &si6.sin6_addr, sizeof(si6.sin6_addr));
    }

    return ret;
}

static int forward_icmpv6_pkt(struct icmp6_proxy_t* proxy, struct icmp6_hdr* hdr,
                              size_t len, struct in6_addr* from, struct in6_addr* to)
{
    char saddr[INET6_ADDRSTRLEN] = "";
    char daddr[INET6_ADDRSTRLEN] = "";

    inet_ntop(PF_INET6, from, saddr, sizeof(saddr));
    inet_ntop(PF_INET6, to, daddr, sizeof(daddr));

//    for( size_t i = 0 ; i < len ; i ++ ){
//        if( i && (i%16) == 0 ){
//            printf("\n");
//        }
//        printf("%.2x ", ((unsigned char*)hdr)[i]);
//    }
//    printf("\n");

    switch( hdr->icmp6_type ){
    case ND_ROUTER_ADVERT:
        printf("from: %s, to: %s, type: RA\n", saddr, daddr);
        break;
    case ND_ROUTER_SOLICIT:
        printf("from: %s, to: %s, type: RS\n", saddr, daddr);
        break;
    case ND_NEIGHBOR_ADVERT:
        printf("from: %s, to: %s, type: NA\n", saddr, daddr);
        break;
    case ND_NEIGHBOR_SOLICIT:
        printf("from: %s, to: %s, type: NS\n", saddr, daddr);
        break;
    default:
        printf("from: %s, to: %s, type: Other(%u)\n", saddr, daddr, hdr->icmp6_type);
        break;
    }

    return 0;
}

static void clear_fdb(struct fdb_t* fdb)
{}

static void cleanup_icmp6proxy(struct icmp6_proxy_t* proxy) {
    if( proxy->wan.join_node_router_group ){
        leave_multicast(&proxy->wan, "ff01::2");
    }

    if( proxy->wan.join_link_router_group ){
        leave_multicast(&proxy->wan, "ff02::2");
    }

    if( proxy->wan.join_site_router_group ){
        leave_multicast(&proxy->wan, "ff05::2");
    }

    if( proxy->lan.join_node_router_group ){
        leave_multicast(&proxy->wan, "ff01::2");
    }

    if( proxy->lan.join_link_router_group ){
        leave_multicast(&proxy->wan, "ff02::2");
    }

    if( proxy->lan.join_site_router_group ){
        leave_multicast(&proxy->wan, "ff05::2");
    }

    close(proxy->wan.rawsock);
    close(proxy->lan.rawsock);

    clear_fdb(&proxy->fdb);
}

int main(int argc, char** argv)
{
    struct icmp6_proxy_t* icmp6proxy;

    struct proxy_args_t {
        char wan_ifname[IF_NAMESIZE];
        char lan_ifname[IF_NAMESIZE];
    }args;
    memset(&args, 0, sizeof(args));

    int op;
    while( -1 != (op = getopt(argc, argv, "l:w:")) ){
        switch (op) {
        case 'l':
            strcpy(args.lan_ifname,optarg);
            break;
        case 'w':
            strcpy(args.wan_ifname,optarg);
        case '?':
            error(1, EINVAL, "%s is not a valid option", argv[optind]);
            break;
        default:
            break;
        }
    }

    icmp6proxy = (struct icmp6_proxy_t*)calloc(1, sizeof(struct icmp6_proxy_t));
    if( !icmp6proxy ){
        error(1, errno, "failed to create icmp6proxy object");
    }

    strcpy(icmp6proxy->lan.ifname, args.lan_ifname);
    icmp6proxy->lan.ifindex = if_nametoindex(args.lan_ifname);

    strcpy(icmp6proxy->wan.ifname, args.wan_ifname);
    icmp6proxy->wan.ifindex = if_nametoindex(args.wan_ifname);

    if( create_icmpv6_sock(&icmp6proxy->lan) < 0 ){
        cleanup_icmp6proxy(icmp6proxy);
        return -errno;
    }

    if( create_icmpv6_sock(&icmp6proxy->wan) < 0 ){
        cleanup_icmp6proxy(icmp6proxy);
        return -errno;
    }

    int ret;
    int maxfd;
    ssize_t retlen;
    fd_set rfdset;
    fd_set rfdset_save;
    struct in6_addr to;
    struct in6_addr from;
    uint8_t pktbuf[1520] = "";
    //struct icmp6_hdr* icmp6pkt = (struct icmp6_hdr*)pktbuf;

    FD_ZERO(&rfdset_save);
    FD_SET(icmp6proxy->lan.rawsock, &rfdset_save);
    FD_SET(icmp6proxy->wan.rawsock, &rfdset_save);

    if( icmp6proxy->lan.rawsock > icmp6proxy->wan.rawsock ){
        maxfd = icmp6proxy->lan.rawsock;
    }else{
        maxfd = icmp6proxy->wan.rawsock;
    }

    icmp6proxy->running = true;
    while (icmp6proxy->running) {
        struct timeval tv = {
            .tv_sec     = icmp6proxy->timeout,
            .tv_usec    = 0
        };
        rfdset = rfdset_save;

        ret = select(maxfd + 1, &rfdset, NULL, NULL, &tv);
        if( ret == 0 ){
            continue;
        }else if( ret < 0 ){
            break;
        }

        if( FD_ISSET(icmp6proxy->lan.rawsock, &rfdset) ){
            retlen = recv_icmpv6_pkt(&icmp6proxy->lan, pktbuf, sizeof(pktbuf), &from, &to);
            if( 0 > retlen ){
                error(0, errno, "failed to read icmp6 packet from lan");
                break;
            }
            forward_icmpv6_pkt(icmp6proxy, (struct icmp6_hdr*)pktbuf, retlen, &from, &to);
        }

        if( FD_ISSET(icmp6proxy->wan.rawsock, &rfdset) ){
            retlen = recv_icmpv6_pkt(&icmp6proxy->wan, pktbuf, sizeof(pktbuf), &from, &to);
            if( 0 > retlen ){
                error(0, errno, "failed to read icmp6 packet from wan");
                break;
            }
            forward_icmpv6_pkt(icmp6proxy, (struct icmp6_hdr*)pktbuf, retlen, &from, &to);
        }
    }

    cleanup_icmp6proxy(icmp6proxy);

    return 0;
}
