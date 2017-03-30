/*
 *
 * Copyright (C) 2011, 2015 Cisco Systems, Inc.
 * Copyright (C) 2015 CBA research group, Technical University of Catalonia.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "iface_mgmt.h"
#include "defs.h"
#include "oor_external.h"
#include "control/oor_control.h"
#include "data-plane/data-plane.h"
#include "lib/oor_log.h"
#include "lib/prefixes.h"
#include "lib/sockets-util.h"


/************************* FUNCTION DECLARTAION ********************************/

void process_nl_add_address (struct nlmsghdr *nlh);
void process_nl_del_address (struct nlmsghdr *nlh);
void process_nl_new_link (struct nlmsghdr *nlh);
void process_nl_new_route (struct nlmsghdr *nlh);
void process_nl_new_unicast_route (struct rtmsg *rtm, int rt_length);
void process_nl_new_multicast_route (struct rtmsg *rtm, int rt_length);
void process_nl_del_route (struct nlmsghdr *nlh);
void process_nl_del_unicast_route(struct rtmsg *rtm, int rt_length);
void process_nl_del_multicast_route (struct rtmsg *rtm, int rt_length);
int process_nl_mcast_route_attributes(struct rtmsg *rtm, int rt_length,
        lisp_addr_t *src, lisp_addr_t *grp);

/* Change the address of the interface. If the address belongs to a not
 * initialized locator, activate it. Program SMR */
void process_address_change(iface_t *iface, lisp_addr_t *new_addr);




/*******************************************************************************/


int
process_netlink_msg(struct sock *sl)
{
    int len = 0;
    char buffer[4096];
    struct iovec iov;
    struct sockaddr_nl dst_addr;
    struct msghdr msgh;
    struct nlmsghdr *nlh;

    nlh = (struct nlmsghdr *) buffer;

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = (void *) nlh;
    iov.iov_len = sizeof(nlh);

    memset(&msgh, 0, sizeof(msgh));
    msgh.msg_name = (void *) &(dst_addr);
    msgh.msg_namelen = sizeof(dst_addr);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    while ((len = recv(sl->fd, nlh, 4096, MSG_DONTWAIT)) > 0) {
        for (; (NLMSG_OK(nlh, len)) && (nlh->nlmsg_type != NLMSG_DONE);
                nlh = NLMSG_NEXT(nlh, len)) {
            switch (nlh->nlmsg_type) {
            case RTM_NEWADDR:
                OOR_LOG(LDBG_2, "==>process_netlink_msg: Received new address "
                        "message");
                process_nl_add_address(nlh);
                break;
            case RTM_DELADDR:
                OOR_LOG(LDBG_2, "==>process_netlink_msg: Received del address "
                        "message");
                process_nl_del_address(nlh);
                break;
            case RTM_NEWLINK:
                OOR_LOG(LDBG_2, "==>process_netlink_msg: Received link "
                        "message");
                process_nl_new_link(nlh);
                break;
            case RTM_NEWROUTE:
                OOR_LOG(LDBG_2, "==>process_netlink_msg: Received new route "
                        "message");
                process_nl_new_route(nlh);
                break;
            case RTM_DELROUTE:
                OOR_LOG(LDBG_2, "==>process_netlink_msg: Received delete route "
                        "message");
                process_nl_del_route(nlh);
                break;
            default:
                break;

            }
        }
        nlh = (struct nlmsghdr *) buffer;
        memset(nlh, 0, 4096);
    }

    return (GOOD);
}


void
process_nl_add_address (struct nlmsghdr *nlh)
{
    struct ifaddrmsg *ifa;
    struct rtattr *rth;
    int iface_index;
    int rt_length;
    iface_t *iface;
    lisp_addr_t new_addr = { .lafi = LM_AFI_IP };
    char iface_name[IF_NAMESIZE];

    /*
     * Get the new address from the net link message
     */
    ifa = (struct ifaddrmsg *) NLMSG_DATA (nlh);
    iface_index = ifa->ifa_index;

    iface = get_interface_from_index(iface_index);

    if (iface == NULL) {
        if_indextoname(iface_index, iface_name);
        OOR_LOG(LDBG_2, "process_nl_add_address: netlink message not for an "
                "interface with associated RLOCs (%s / %d)", iface_name,
                iface_index);
        return;
    }
    rth = IFA_RTA (ifa);

    rt_length = IFA_PAYLOAD(nlh);
    for (;rt_length && RTA_OK (rth, rt_length);rth = RTA_NEXT (rth,rt_length))
    {
        if (ifa->ifa_family == AF_INET && rth->rta_type == IFA_LOCAL){
            lisp_addr_ip_init(&new_addr, RTA_DATA(rth), ifa->ifa_family);
            process_address_change (iface, &new_addr);
        }
        if (ifa->ifa_family == AF_INET6 && rth->rta_type == IFA_ADDRESS){
            lisp_addr_ip_init(&new_addr, RTA_DATA(rth), ifa->ifa_family);
            process_address_change (iface, &new_addr);
        }
    }
}

/* Change the address of the interface. If the address belongs to a not
 * initialized locator, activate it. Program SMR */
void
process_address_change(iface_t *iface, lisp_addr_t *new_addr)
{
    lisp_addr_t *iface_addr;
    lisp_addr_t *new_addr_cpy;
    lisp_addr_t *old_addr_cpy;
    lisp_addr_t *gw_addr, *net_addr;
    int new_addr_ip_afi;


    new_addr_ip_afi = lisp_addr_ip_afi(new_addr);
    iface_addr = iface_address(iface,new_addr_ip_afi);

    if (iface_addr == NULL){
        OOR_LOG(LDBG_2,"precess_address_change: OOR not configured to use %s address for the interface %s",
                (new_addr_ip_afi == AF_INET ? "IPv4" : "IPv6"),iface->iface_name);
        return;
    }

    /* Check if the addres is a global address*/
    if (ip_addr_is_link_local(lisp_addr_ip(new_addr)) == TRUE) {
        OOR_LOG(LDBG_2,"precess_address_change: the address in netlink messages "
                "is a local link address: %s discarded",
                lisp_addr_to_char(new_addr));
        return;
    }
    /* If default RLOC afi defined (-a 4 or 6), only accept addresses of the
     * specified afi */
    if (default_rloc_afi != AF_UNSPEC
        && default_rloc_afi != new_addr_ip_afi) {
        OOR_LOG(LDBG_2,"precess_address_change: Default RLOC afi defined (-a #): "
                "Skipped %s address in iface %s",
                (new_addr_ip_afi == AF_INET) ? "IPv4" : "IPv6",
                iface->iface_name);
        return;
    }

    /* Check if the affected address is the one used to reach the default of the interface */
    gw_addr = iface_gateway(iface, new_addr_ip_afi);
    if (gw_addr){
        net_addr = get_network_pref_of_host(gw_addr);
        if (pref_is_addr_part_of_prefix(new_addr,net_addr) == FALSE){
            OOR_LOG(LDBG_2,"precess_address_change: Change of not main address of the interface."
                    " Skipped %s address in iface %s",
                    lisp_addr_to_char(net_addr), iface->iface_name);
            return;
        }else{
            OOR_LOG(LDBG_2,"process_address_change: New address %s belongs to the prefix %s",
                    lisp_addr_to_char(new_addr),lisp_addr_to_char(net_addr));
        }
    }else{
        OOR_LOG(LDBG_2,"precess_address_change: No gateway address for the interface %s. Skipped %s address",
                iface->iface_name, lisp_addr_to_char(new_addr));
        return;
    }

    /* Detected a valid change of address  */
    OOR_LOG(LDBG_2,"process_address_change: New address detected for interface "
            "%s. Address changed from %s to %s. The gateway of the interface is: %s", iface->iface_name,
            lisp_addr_to_char(iface_addr), lisp_addr_to_char(new_addr), lisp_addr_to_char(gw_addr));


    old_addr_cpy = lisp_addr_clone(iface_addr);
    new_addr_cpy = lisp_addr_clone(new_addr);


    /* raise event to data plane */
    data_plane->datap_updated_addr(iface,iface_addr,new_addr);

    /* raise event in ctrl */
    ctrl_if_addr_update(lctrl, iface, old_addr_cpy, new_addr_cpy);
    lisp_addr_del(old_addr_cpy);
    lisp_addr_del(new_addr_cpy);

}


void
process_nl_del_address(struct nlmsghdr *nlh)
{
    struct ifaddrmsg *ifa;
    struct rtattr *rth;
    int iface_index;
    int rt_length;
    iface_t *iface;
    lisp_addr_t new_addr;
    char iface_name[IF_NAMESIZE];

    ifa = (struct ifaddrmsg *) NLMSG_DATA(nlh);
    iface_index = ifa->ifa_index;

    iface = get_interface_from_index(iface_index);

    if (iface == NULL) {
        if_indextoname(iface_index, iface_name);
        OOR_LOG(LDBG_2, "process_nl_add_address: the netlink message is not "
                "for any interface associated with RLOCs (%s)", iface_name);
        return;
    }

    rth = IFA_RTA(ifa);
    rt_length = IFA_PAYLOAD(nlh);
    for (; rt_length && RTA_OK(rth, rt_length);rth = RTA_NEXT(rth, rt_length)) {
        if ((ifa->ifa_family == AF_INET && rth->rta_type == IFA_LOCAL)
                        || (ifa->ifa_family == AF_INET6 && rth->rta_type == IFA_ADDRESS)){
            lisp_addr_ip_init(&new_addr, RTA_DATA(rth), ifa->ifa_family);
            break;
        }
    }
    /* Actions to be done when address is removed */
    OOR_LOG(LDBG_2, "   deleted address: %s\n", lisp_addr_to_char(&new_addr));
}

void
process_nl_new_link(struct nlmsghdr *nlh)
{
    struct ifinfomsg *ifi;
    iface_t *iface;
    int iface_index;
    int old_iface_index;
    uint8_t new_status;
    char iface_name[IF_NAMESIZE];


    ifi = (struct ifinfomsg *) NLMSG_DATA (nlh);
    iface_index = ifi->ifi_index;


    iface = get_interface_from_index(iface_index);

    if (iface == NULL) {
        /*
         * In some OS when a virtual interface is removed and added again,
         * the index of the interface change. Search iface_t by the interface
         * name and update the index. */
        if (if_indextoname(iface_index, iface_name) != NULL) {
            iface = get_interface(iface_name);
        }
        if (iface == NULL) {
            OOR_LOG(LDBG_2, "process_nl_new_link: the netlink message is not for "
                    "any interface associated with RLOCs  (%s)", iface_name);
            return;
        } else {
            old_iface_index = iface->iface_index;
        }
    }else{
        old_iface_index = iface_index;
    }

    /* Get the new status */
    if ((ifi->ifi_flags & IFF_RUNNING) != 0) {
        OOR_LOG(LDBG_1, "process_nl_new_link: Interface %s changes its status to "
                "UP", iface->iface_name);
        new_status = UP;
    } else {
        OOR_LOG(LDBG_1, "process_nl_new_link: Interface %s changes its status to "
                "DOWN", iface->iface_name);
        new_status = DOWN;
    }
    /* Check if status has changed */
    if (iface->status == new_status){
        OOR_LOG(LDBG_2,"process_nl_new_link: The detected change of status"
                " doesn't affect");
        return;
    }
    /* raise event to data plane */
    data_plane->datap_update_link(iface, old_iface_index, iface_index, new_status);
    /* raise event in ctrl */
    ctrl_if_link_update(lctrl, iface, old_iface_index, iface_index, new_status);
}


void
process_nl_new_route (struct nlmsghdr *nlh)
{
    struct rtmsg *rtm;
    int rt_length;

    rtm = (struct rtmsg *) NLMSG_DATA (nlh);
    rt_length = RTM_PAYLOAD(nlh);

    /* Interested only in unicast or multicast updates */
    if (rtm->rtm_type == RTN_UNICAST) {
        process_nl_new_unicast_route(rtm, rt_length);
    } else if (rtm->rtm_type == RTN_MULTICAST) {
        process_nl_new_multicast_route(rtm, rt_length);
    }

}

void
process_nl_new_unicast_route(struct rtmsg *rtm, int rt_length)
{
    struct rtattr *rt_attr;
    iface_t *iface = NULL;
    int iface_index;
    char iface_name[IF_NAMESIZE];
    lisp_addr_t gateway = { .lafi = LM_AFI_IP };
    lisp_addr_t src = { .lafi = LM_AFI_IP };
    lisp_addr_t dst = { .lafi = LM_AFI_IP };
    lisp_addr_t *iface_addr, *new_iface_addr, *old_addr_cpy;
    int src_len;
    int dst_len;
    int afi;

    /* Interested only in main table updates for unicast */
    if (rtm->rtm_table != RT_TABLE_MAIN)
        return;

    if ( rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6 ) {
        OOR_LOG(LDBG_3,"process_nl_new_unicast_route: New unicast route of "
                "unknown address family %d", rtm->rtm_family);
        return;
    }
    afi = rtm->rtm_family;
    src_len = rtm->rtm_src_len;
    dst_len = rtm->rtm_dst_len;

    rt_attr = (struct rtattr *)RTM_RTA(rtm);

    for (; RTA_OK(rt_attr, rt_length);
            rt_attr = RTA_NEXT(rt_attr, rt_length)) {
        switch (rt_attr->rta_type) {
        case RTA_OIF:
            iface_index = *(int *)RTA_DATA(rt_attr);
            iface = get_interface_from_index(iface_index);
            if_indextoname(iface_index, iface_name);
            if (iface == NULL){
                OOR_LOG(LDBG_3, "process_nl_new_unicast_route: the netlink "
                        "message is not for any interface associated with "
                        "RLOCs (%s)", iface_name);
                return;
            }
            break;
        case RTA_GATEWAY:
            lisp_addr_ip_init(&gateway, RTA_DATA(rt_attr), rtm->rtm_family);
            break;
        case RTA_SRC:
            lisp_addr_ip_init(&src, RTA_DATA(rt_attr), rtm->rtm_family);
            lisp_addr_set_plen(&src,src_len);
            break;
        case RTA_DST:
            lisp_addr_ip_init(&dst, RTA_DATA(rt_attr), rtm->rtm_family);
            lisp_addr_set_plen(&dst,dst_len);
            break;
        default:
            break;
        }
    }

    /* Check default afi*/

    if (lisp_addr_ip_afi(&src) != LM_AFI_NO_ADDR &&
            default_rloc_afi != AF_UNSPEC &&
            default_rloc_afi != lisp_addr_ip_afi(&src)) {
        OOR_LOG(LDBG_1, "process_nl_new_unicast_route: Default RLOC afi "
                "defined (-a #): Skipped route with source address %s in iface %s",
                (lisp_addr_ip_afi(&src)== AF_INET) ? "IPv4" : "IPv6",
                        iface->iface_name);
        return;
    }

    if (lisp_addr_ip_afi(&dst) != LM_AFI_NO_ADDR &&
            default_rloc_afi != AF_UNSPEC &&
            default_rloc_afi != lisp_addr_ip_afi(&dst)) {
        OOR_LOG(LDBG_1, "process_nl_new_unicast_route: Default RLOC afi "
                "defined (-a #): Skipped route with destination address %s in iface %s",
                (lisp_addr_ip_afi(&dst)== AF_INET) ? "IPv4" : "IPv6",
                        iface->iface_name);
        return;
    }

    if (lisp_addr_ip_afi(&gateway) != LM_AFI_NO_ADDR &&
            default_rloc_afi != AF_UNSPEC &&
            default_rloc_afi != lisp_addr_ip_afi(&gateway)) {
        OOR_LOG(LDBG_1, "process_nl_new_unicast_route: Default RLOC afi "
                "defined (-a #): Skipped route with gateway address %s in iface %s",
                (lisp_addr_ip_afi(&dst)== AF_INET) ? "IPv4" : "IPv6",
                        iface->iface_name);
        return;
    }

    iface_addr = iface_address(iface,afi);

    if (!iface_addr){
        OOR_LOG(LDBG_1, "==>process_nl_new_unicast_route: %s address familiy not used for interface %s. "
                "Skipped route",(afi== AF_INET) ? "IPv4" : "IPv6", iface->iface_name);
        return;
    }

    OOR_LOG(LDBG_1, "process_nl_new_unicast_route: New route detected-> SRC: %s ,DST: %s ,GW: %s, IFACE: %s",
            lisp_addr_to_char(&src),lisp_addr_to_char(&dst),lisp_addr_to_char(&gateway), iface->iface_name);

    /* raise event to data plane */
    data_plane->datap_updated_route(RTM_NEWROUTE, iface, &src, &dst, &gateway);
    /* raise event to control plane */
    ctrl_route_update(lctrl, RTM_NEWROUTE, iface, &src, &dst, &gateway);

    /* If default route added, check if the used address for the interface is valid */
    if (lisp_addr_ip_afi(&gateway) == LM_AFI_NO_ADDR){
        return;
    }

    new_iface_addr = iface_find_address(iface,afi);
    if (lisp_addr_is_no_addr(new_iface_addr)){
        OOR_LOG(LDBG_1, "process_nl_new_unicast_route: It should never happen");
    }
    if (lisp_addr_cmp(iface_addr,new_iface_addr)!=0){
        // We have selected a wrong address to the interface. Replace and notify it
        OOR_LOG(LDBG_2, "process_nl_new_unicast_route: Used iface address not match "
                "with the gateway. Replacing %s with %s", lisp_addr_to_char(iface_addr),
                lisp_addr_to_char(new_iface_addr));
        old_addr_cpy = lisp_addr_clone(iface_addr);
        /* raise event to data plane */
        data_plane->datap_updated_addr(iface,iface_addr,new_iface_addr);

        /* raise event in ctrl */
        ctrl_if_addr_update(lctrl, iface, old_addr_cpy, new_iface_addr);
    }
    lisp_addr_del(new_iface_addr);
}

void
process_nl_new_multicast_route(struct rtmsg *rtm, int rt_length)
{
//    lisp_addr_t srcaddr = {.lafi = LM_AFI_IP};
//    lisp_addr_t grpaddr = {.lafi = LM_AFI_IP};

    OOR_LOG(LDBG_1, "process_nl_new_multicast_route: Not yet implented, ignored!");

//    /* IPv4 multicast routes are part of the default table and have
//     * family 128, while IPv6 multicast routes are part of the main table
//     * and have family 129 ... */
//    if ( !((rtm->rtm_table == RT_TABLE_DEFAULT && rtm->rtm_family == 128) ||
//           (rtm->rtm_table == RT_TABLE_MAIN && rtm->rtm_family == 129) ) )
//        return;
//
//    if (process_nl_mcast_route_attributes(rtm, rt_length, &srcaddr, &grpaddr)
//            == BAD) {
//        return;
//    }
//
//    multicast_join_channel(&srcaddr, &grpaddr);
}

int
process_nl_mcast_route_attributes(struct rtmsg *rtm, int rt_length,
        lisp_addr_t *rt_srcaddr, lisp_addr_t *rt_grpaddr)
{
    struct rtattr *rt_attr;
    iface_t *iface;
    int iface_index;
    char iface_name[IF_NAMESIZE];
    struct rtnexthop *rt_nh;
    int nb_oifs = 0;
    int rtnh_length = 0;
    char ifnames[1024] = { 0 };

    rt_attr = (struct rtattr *)RTM_RTA(rtm);

    for (; RTA_OK(rt_attr, rt_length);
            rt_attr = RTA_NEXT(rt_attr, rt_length)) {
        switch (rt_attr->rta_type) {
            case RTA_DST:
                switch(rtm->rtm_family) {
                    case 128:
                        lisp_addr_ip_init(rt_grpaddr, RTA_DATA(rt_attr),
                                AF_INET);
                        break;
                    case 129:
                        lisp_addr_ip_init(rt_grpaddr, RTA_DATA(rt_attr),
                                AF_INET6);
                        break;
                    default:
                        break;
                }
                break;
            case RTA_SRC:
                switch (rtm->rtm_family) {
                    case 128:
                        lisp_addr_ip_init(rt_srcaddr, RTA_DATA(rt_attr),
                                AF_INET);
                        break;
                    case 129:
                        lisp_addr_ip_init(rt_srcaddr, RTA_DATA(rt_attr),
                                AF_INET6);
                        break;
                    default:
                        break;
                }
                break;
            case RTA_MULTIPATH:
                rt_nh = (struct rtnexthop *)RTA_DATA(rt_attr);
                rtnh_length = RTA_PAYLOAD(rt_attr);
                for (; RTNH_OK(rt_nh, rtnh_length); rt_nh = RTNH_NEXT(rt_nh)) {
                    /* Check if one of the interfaces is the gateway */
                    iface = get_interface_from_index(rt_nh->rtnh_ifindex);
                    iface_index = *(int *)RTA_DATA(rt_attr);
                    iface = get_interface_from_index(iface_index);
                    if (iface != NULL){
                        if_indextoname(iface_index, iface_name);
                        OOR_LOG(LINF, "process_nl_new_multicast_route: the "
                                "multicast route message is for an interface "
                                "that has RLOCs associated (%s). Ignoring!",
                                iface_name);
                        return BAD;
                    }

                    /* Prepare output for debug */
                    if_indextoname(rt_nh->rtnh_ifindex, iface_name);
                    sprintf(ifnames, "%s ", iface_name);
                    nb_oifs++;
                }
                break;
            default:
                break;
        }
    }

    if (nb_oifs == 0){
        OOR_LOG(LDBG_1, "process_nl_new_multicast_route: New multicast route has "
                "no output interface list, ignored!");
        return BAD;
    }

    OOR_LOG(LINF, "New multicast route with source %s, group %s for interfaces "
            "%s", lisp_addr_to_char(rt_srcaddr), lisp_addr_to_char(rt_grpaddr),
            ifnames);

    return GOOD;
}

void
process_nl_del_route(struct nlmsghdr *nlh)
{

    struct rtmsg *rtm;
    int rt_length;

    rtm = (struct rtmsg *) NLMSG_DATA (nlh);
    rt_length = RTM_PAYLOAD(nlh);

    /* Interested only in unicast or multicast updates */
    if (rtm->rtm_type == RTN_UNICAST) {
        process_nl_del_unicast_route(rtm, rt_length);
    } else if (rtm->rtm_type == RTN_MULTICAST) {
        process_nl_del_multicast_route(rtm, rt_length);
    }
}

void
process_nl_del_unicast_route(struct rtmsg *rtm, int rt_length)
{
    struct rtattr *rt_attr;
    iface_t *iface = NULL;
    int iface_index;
    char iface_name[IF_NAMESIZE];
    lisp_addr_t gateway = { .lafi = LM_AFI_IP };
    lisp_addr_t src = { .lafi = LM_AFI_IP };
    lisp_addr_t dst = { .lafi = LM_AFI_IP };
    lisp_addr_t *iface_addr;
    int src_len;
    int dst_len;
    int afi;

    /* Interested only in main table updates for unicast */
    if (rtm->rtm_table != RT_TABLE_MAIN)
        return;

    if ( rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6 ) {
        OOR_LOG(LDBG_3,"process_nl_del_unicast_route: New unicast route of "
                "unknown address family %d", rtm->rtm_family);
        return;
    }

    src_len = rtm->rtm_src_len;
    dst_len = rtm->rtm_dst_len;
    afi = rtm->rtm_family;

    rt_attr = (struct rtattr *)RTM_RTA(rtm);

    for (; RTA_OK(rt_attr, rt_length);
            rt_attr = RTA_NEXT(rt_attr, rt_length)) {
        switch (rt_attr->rta_type) {
        case RTA_OIF:
            iface_index = *(int *)RTA_DATA(rt_attr);
            iface = get_interface_from_index(iface_index);
            if_indextoname(iface_index, iface_name);
            if (iface == NULL){
                OOR_LOG(LDBG_3, "process_nl_del_unicast_route: the netlink "
                        "message is not for any interface associated with "
                        "RLOCs (%s)", iface_name);
                return;
            }
            break;
        case RTA_GATEWAY:
            lisp_addr_ip_init(&gateway, RTA_DATA(rt_attr), rtm->rtm_family);
            break;
        case RTA_SRC:
            lisp_addr_ip_init(&src, RTA_DATA(rt_attr), rtm->rtm_family);
            lisp_addr_set_plen(&src,src_len);
            break;
        case RTA_DST:
            lisp_addr_ip_init(&dst, RTA_DATA(rt_attr), rtm->rtm_family);
            lisp_addr_set_plen(&dst,dst_len);
            break;
        default:
            break;
        }
    }

    /* Check default afi*/

    if (lisp_addr_ip_afi(&src) != LM_AFI_NO_ADDR &&
            default_rloc_afi != AF_UNSPEC &&
            default_rloc_afi != lisp_addr_ip_afi(&src)) {
        OOR_LOG(LDBG_1, "process_nl_del_unicast_route: Default RLOC afi "
                "defined (-a #): Skipped route with source address %s in iface %s",
                (lisp_addr_ip_afi(&src)== AF_INET) ? "IPv4" : "IPv6",
                        iface->iface_name);
        return;
    }

    if (lisp_addr_ip_afi(&dst) != LM_AFI_NO_ADDR &&
            default_rloc_afi != AF_UNSPEC &&
            default_rloc_afi != lisp_addr_ip_afi(&dst)) {
        OOR_LOG(LDBG_1, "process_nl_del_unicast_route: Default RLOC afi "
                "defined (-a #): Skipped route with destination address %s in iface %s",
                (lisp_addr_ip_afi(&dst)== AF_INET) ? "IPv4" : "IPv6",
                        iface->iface_name);
        return;
    }

    if (lisp_addr_ip_afi(&gateway) != LM_AFI_NO_ADDR &&
            default_rloc_afi != AF_UNSPEC &&
            default_rloc_afi != lisp_addr_ip_afi(&gateway)) {
        OOR_LOG(LDBG_1, "process_nl_del_unicast_route: Default RLOC afi "
                        "defined (-a #): Skipped route with gateway address %s in iface %s",
                        (lisp_addr_ip_afi(&gateway)== AF_INET) ? "IPv4" : "IPv6",
                                iface->iface_name);
        return;
    }

    iface_addr = iface_address(iface,afi);

    if (!iface_addr){
        OOR_LOG(LDBG_1, "==>process_nl_del_unicast_route: %s address familiy not used for interface %s. "
                "Skipped del route",(afi== AF_INET) ? "IPv4" : "IPv6", iface->iface_name);
        return;
    }

    /* raise event to data plane */
    data_plane->datap_updated_route(RTM_DELROUTE, iface, &src, &dst, &gateway);
    /* raise event to control plane */
    ctrl_route_update(lctrl, RTM_DELROUTE, iface, &src, &dst, &gateway);
}

void
process_nl_del_multicast_route (struct rtmsg *rtm, int rt_length)
{

    lisp_addr_t  rt_groupaddr = {.lafi=LM_AFI_IP};
    lisp_addr_t  rt_srcaddr = {.lafi=LM_AFI_IP};

    if ( !((rtm->rtm_table == RT_TABLE_DEFAULT && rtm->rtm_family == 128) ||
           (rtm->rtm_table == RT_TABLE_MAIN && rtm->rtm_family == 129) ) )
        return;


    if (process_nl_mcast_route_attributes(rtm, rt_length, &rt_srcaddr, &rt_groupaddr) == BAD)
        return;

    multicast_leave_channel(&rt_srcaddr, &rt_groupaddr);
}

int
get_all_ifaces_name_list(char ***ifaces,int *count)
{
    struct nlmsghdr *nlh, *rcvhdr;
    struct ifinfomsg *ifm, *if_msg;
    char sndbuf[4096],rcvbuf[4096],name[IF_NAMESIZE];
    int ifa_len, retval,readlen;
    int netlk_fd;

    netlk_fd = opent_netlink_socket();
    if (netlk_fd == ERR_SOCKET){
        OOR_LOG(LERR, "get_all_ifaces_name_list: Error opening netlink socket");
        return (BAD);
    }

    *ifaces = calloc(100,sizeof(char *));

    /*
     * Build the command
     */
    memset(sndbuf, 0, 4096);

    nlh = (struct nlmsghdr *)sndbuf;
    ifm = (struct ifinfomsg *)(CO(sndbuf,sizeof(struct nlmsghdr)));

    ifa_len = sizeof(struct ifinfomsg);

    nlh->nlmsg_len   = NLMSG_LENGTH(ifa_len);
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_type  = RTM_GETLINK;


    ifm->ifi_family    = AF_PACKET;

    retval = send(netlk_fd, sndbuf, NLMSG_LENGTH(ifa_len), 0);

    if (retval < 0) {
        OOR_LOG(LERR, "get_all_ifaces_name_list: send netlink command failed: %s", strerror(errno));
        close(netlk_fd);
        return(BAD);
    }

    /*
     * Receive the responses from the kernel
     */
    while ((readlen = recv(netlk_fd,rcvbuf,4096,MSG_DONTWAIT)) > 0){
        rcvhdr = (struct nlmsghdr *)rcvbuf;
        /*
         * Walk through everything it sent us
         */
        for (; NLMSG_OK(rcvhdr, (unsigned int)readlen); rcvhdr = NLMSG_NEXT(rcvhdr, readlen)) {
            if (rcvhdr->nlmsg_type == RTM_NEWLINK) {
                if_msg = (struct ifinfomsg *)NLMSG_DATA(rcvhdr);
                if (if_indextoname(if_msg->ifi_index, name) != NULL){
                    (*ifaces)[*count] = strdup(name);
                    (*count)++;
                }
            }
        }
    }
    close(netlk_fd);
    return (GOOD);
}

void
iface_mac_address(char *iface_name, uint8_t *mac)
{
     int fd;
     struct ifreq ifr;
     int i = 0;

     fd = socket(AF_INET, SOCK_DGRAM, 0);

     memset(&ifr, 0, sizeof(ifr));
     ifr.ifr_addr.sa_family = AF_INET;
     strncpy(ifr.ifr_name, iface_name, IFNAMSIZ-1);

     ioctl(fd, SIOCGIFHWADDR, &ifr);

     close(fd);
     for (i = 0 ; i < 6 ; i++){
         mac[i] = (unsigned char)ifr.ifr_hwaddr.sa_data[i];
     }

     return;
}


lisp_addr_t *
get_network_pref_of_host(lisp_addr_t *address)
{
    lisp_addr_t net_prefix = { .lafi = LM_AFI_IP };
    int netlink_fd;
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh, *rcvhdr;
    struct rtmsg *rtm, *recv_rtm;
    struct rtattr *rt_attr;
    char sndbuf[4096],rcvbuf[4096];
    int rta_len = 0, retval, readlen, recv_pyload_len, afi;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    if (lisp_addr_ip_afi(address) == AF_INET){
        afi = AF_INET;
        addr.nl_groups = RTMGRP_IPV4_ROUTE;
    }else{
        afi = AF_INET6;
        addr.nl_groups = RTMGRP_IPV6_ROUTE;
    }

    netlink_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

    if (netlink_fd < 0) {
        OOR_LOG(LERR, "get_network_associated_addrress: Failed to connect to "
                "netlink socket");
        return (NULL);
    }

    bind(netlink_fd, (struct sockaddr *) &addr, sizeof(addr));

    memset(sndbuf, 0, 4096);
    nlh = (struct nlmsghdr *)sndbuf;
    rtm = (struct rtmsg *)(CO(sndbuf,sizeof(struct nlmsghdr)));

    rta_len = sizeof(struct rtmsg);

    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_len = NLMSG_LENGTH(rta_len);


    rtm->rtm_family = afi;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = RTPROT_STATIC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_src_len = 0;
    rtm->rtm_tos = 0;
    rtm->rtm_dst_len = 0;

    retval = send(netlink_fd, sndbuf, nlh->nlmsg_len, 0);

    if (retval < 0) {
        OOR_LOG(LCRIT, "get_network_associated_addrress: send netlink command failed %s", strerror(errno));
        return (NULL);
    }
    /*
     * Receive the responses from the kernel
     */

    while ((readlen = recv(netlink_fd,rcvbuf,4096,MSG_DONTWAIT)) > 0){
        rcvhdr = (struct nlmsghdr *)rcvbuf;
        /*
         * Walk through everything it sent us
         */
        for (; NLMSG_OK(rcvhdr, (unsigned int)readlen); rcvhdr = NLMSG_NEXT(rcvhdr, readlen)) {
            recv_pyload_len = RTM_PAYLOAD(rcvhdr);
            if (rcvhdr->nlmsg_type == RTM_NEWROUTE) {
                recv_rtm = (struct rtmsg *)NLMSG_DATA(rcvhdr);
                rt_attr = (struct rtattr *)RTM_RTA(recv_rtm);
                for (; RTA_OK(rt_attr, recv_pyload_len); rt_attr = RTA_NEXT(rt_attr, recv_pyload_len)) {
                    switch (rt_attr->rta_type) {
                    case RTA_DST:
                        if ((rtm->rtm_family == AF_INET && recv_rtm->rtm_dst_len == 32) ||
                                (rtm->rtm_family == AF_INET6 && recv_rtm->rtm_dst_len == 128) ){
                            break;
                        }
                        lisp_addr_ip_init(&net_prefix, RTA_DATA(rt_attr), rtm->rtm_family);
                        lisp_addr_set_plen(&net_prefix,recv_rtm->rtm_dst_len);
                        if (pref_is_addr_part_of_prefix(address,&net_prefix) == TRUE){
                            goto find;
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }
    close(netlink_fd);
    OOR_LOG(LDBG_3, "get_network_pref_of_host: No network prefix found for host %s", lisp_addr_to_char(address));
    return (NULL);

    find:
    close(netlink_fd);
    OOR_LOG(LDBG_3, "get_network_pref_of_host: Network prefix for host %s is %s",
            lisp_addr_to_char(address), lisp_addr_to_char(&net_prefix));
    return (lisp_addr_clone(&net_prefix));

}



lisp_addr_t *
iface_get_getway(int iface_index, int afi)
{
    lisp_addr_t gateway = { .lafi = LM_AFI_IP };
    int netlink_fd;
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh, *rcvhdr;
    struct rtmsg *rtm, *recv_rtm;
    struct rtattr *rt_attr;
    char sndbuf[4096],rcvbuf[4096];
    int rta_len = 0, retval, readlen, recv_pyload_len, iface_id, attrs;
    char iface_name[IF_NAMESIZE];

    if_indextoname(iface_index, iface_name);


    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    if (afi == AF_INET){
        addr.nl_groups = RTMGRP_IPV4_ROUTE;
    }else{
        addr.nl_groups = RTMGRP_IPV6_ROUTE;
    }



    netlink_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

    if (netlink_fd < 0) {
        OOR_LOG(LERR, "iface_get_getway: Failed to connect to "
                "netlink socket");
        return (NULL);
    }

    bind(netlink_fd, (struct sockaddr *) &addr, sizeof(addr));

    memset(sndbuf, 0, 4096);
    nlh = (struct nlmsghdr *)sndbuf;
    rtm = (struct rtmsg *)(CO(sndbuf,sizeof(struct nlmsghdr)));

    rta_len = sizeof(struct rtmsg);

    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_len = NLMSG_LENGTH(rta_len);


    rtm->rtm_family = afi;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = RTPROT_STATIC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_src_len = 0;
    rtm->rtm_tos = 0;
    rtm->rtm_dst_len = 0;


    retval = send(netlink_fd, sndbuf, nlh->nlmsg_len, 0);

    if (retval < 0) {
        OOR_LOG(LCRIT, "iface_get_getway: send netlink command failed %s", strerror(errno));
        return (NULL);
    }
    /*
     * Receive the responses from the kernel
     */


    while ((readlen = recv(netlink_fd,rcvbuf,4096,MSG_DONTWAIT)) > 0){
        rcvhdr = (struct nlmsghdr *)rcvbuf;
        /*
         * Walk through everything it sent us
         */
        for (; NLMSG_OK(rcvhdr, (unsigned int)readlen); rcvhdr = NLMSG_NEXT(rcvhdr, readlen)) {
            recv_pyload_len = RTM_PAYLOAD(rcvhdr);
            iface_id = 0;
            if (rcvhdr->nlmsg_type == RTM_NEWROUTE) {
                recv_rtm = (struct rtmsg *)NLMSG_DATA(rcvhdr);
                rt_attr = (struct rtattr *)RTM_RTA(recv_rtm);
                attrs = 0;
                for (; RTA_OK(rt_attr, recv_pyload_len); rt_attr = RTA_NEXT(rt_attr, recv_pyload_len)) {
                    switch (rt_attr->rta_type) {
                    case RTA_OIF:
                        iface_id = *(int *)RTA_DATA(rt_attr);
                        attrs++;
                        break;
                    case RTA_GATEWAY:
                        lisp_addr_ip_init(&gateway, RTA_DATA(rt_attr), rtm->rtm_family);
                        attrs++;
                        break;
                    default:
                        break;
                    }
                }
                if (iface_id == iface_index && attrs == 2 ){
                    goto find;
                }
            }
        }
    }
    OOR_LOG(LDBG_3, "iface_get_getway: No gateway detected for interface %s",iface_name);
    close(netlink_fd);
    return (NULL);

    find:
    close(netlink_fd);
    OOR_LOG(LDBG_3, "iface_get_getway: The gateway for interface %s is %s", iface_name, lisp_addr_to_char(&gateway));
    return (lisp_addr_clone(&gateway));
}
