/*
 * lispd_control.c
 *
 * This file is part of LISP Mobile Node Implementation.
 *
 * Copyright (C) 2014 Universitat Politècnica de Catalunya.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Please send any bug reports or fixes you make to the email address(es):
 *    LISP-MN developers <devel@lispmob.org>
 *
 * Written or modified by:
 *    Florin Coras <fcoras@ac.upc.edu>
 */

#include "lisp_control.h"
#include <lispd_external.h>
#include "lispd_info_nat.h"
#include <lbuf.h>
#include <cksum.h>
#include <lisp_ctrl_device.h>


struct lisp_ctrl {
    glist_t *devices;
    /* move ctrl interface here */

    int ipv4_control_input_fd;
    int ipv6_control_input_fd;

    glist_t *rlocs;
    glist_t *default_rlocs;
};

static void set_default_rlocs(lisp_ctrl_t *ctrl);

static void
set_default_rlocs(lisp_ctrl_t *ctrl)
{
    glist_remove_all(ctrl->default_rlocs);
    if (default_ctrl_iface_v4) {
        glist_add(default_ctrl_iface_v4->ipv4_address, ctrl->default_rlocs);
    }

    if (default_ctrl_iface_v6) {
        glist_add_tail(default_ctrl_iface_v6->ipv6_address,
                ctrl->default_rlocs);
    }
}

lisp_ctrl_t *
ctrl_create()
{
    lisp_ctrl_t *ctrl = xzalloc(sizeof(lisp_ctrl_t));
    ctrl->devices = glist_new_managed((glist_del_fct) ctrl_dev_destroy);
    ctrl->default_rlocs = glist_new_managed((glist_del_fct) lisp_addr_del);

    return (ctrl);
}

void
ctrl_destroy(lisp_ctrl_t *ctrl)
{
    glist_destroy(ctrl->devices);
    glist_destroy(ctrl->default_rlocs);
    close(ctrl->ipv4_control_input_fd);
    close(ctrl->ipv6_control_input_fd);
    free(ctrl);
}

void
ctrl_init(lisp_ctrl_t *ctrl)
{
    set_default_ctrl_ifaces();

    /* Generate receive sockets for control (4342) and data port (4341) */
    if (default_rloc_afi == -1 || default_rloc_afi == AF_INET) {
        ctrl->ipv4_control_input_fd = open_control_input_socket(AF_INET);
        sock_register_read_listener(smaster, ctrl_recv_msg, ctrl,
                ctrl->ipv4_control_input_fd);
    }

    if (default_rloc_afi == -1 || default_rloc_afi == AF_INET6) {
        ctrl->ipv6_control_input_fd = open_control_input_socket(AF_INET6);
        sock_register_read_listener(smaster, ctrl_recv_msg, ctrl,
                ctrl->ipv6_control_input_fd);
    }

    set_default_rlocs(ctrl);
}

/*  Process a LISP protocol message sitting on
 *  socket s with address family afi */
int
ctrl_recv_msg(struct sock *sl)
{
    uconn_t uc;
    struct lbuf *packet;
    lisp_msg_type_e type;
    lisp_ctrl_t *ctrl;
    lisp_ctrl_dev_t *dev;

    ctrl = sl->arg;
    /* Only one device supported for now */
    dev = glist_first_data(ctrl->devices);

    uc.rp = LISP_CONTROL_PORT;

    packet = lbuf_new(MAX_IP_PKT_LEN);
    if (sock_recv(sl->fd, packet, &uc) != GOOD) {
        lmlog(DBG_1, "Couldn't retrieve socket information"
                "for control message! Discarding packet!");
        return (BAD);
    }

    lisp_msg_parse_type(packet, &type);

    /* direct call of ctrl device
     * TODO: check type to decide where to send msg*/
    ctrl_dev_recv(dev, packet, &uc);

    lbuf_del(packet);

    return (GOOD);
}

int
ctrl_send_msg(lisp_ctrl_t *ctrl, lbuf_t *b, uconn_t *uc)
{
    int sk, ret;
    int dst_afi = lisp_addr_ip_afi(&uc->ra);
    iface_t *iface;

    if (lisp_addr_afi(&uc->ra) != LM_AFI_IP) {
        lmlog(DBG_2, "sock_send: dst % of UDP connection is not IP. "
                "Discarding!", lisp_addr_to_char(&uc->ra));
        return (BAD);
    }

    /* FIND the socket where to output the packet */
    if (lisp_addr_afi(&uc->la) == LM_AFI_NO_ADDR) {
        lisp_addr_copy(&uc->la, get_default_ctrl_address(dst_afi));
        sk = get_default_ctrl_socket(dst_afi);
    } else {
        iface = get_interface_with_address(&uc->la);
        if (iface) {
            sk = iface_socket(iface, dst_afi);
        } else {
            sk = get_default_ctrl_socket(dst_afi);
        }
    }

    ret = sock_send(sk, b, uc);

    if (ret != GOOD) {
        lmlog(DBG_1, "FAILED TO SEND \n %s "
                " RLOC: %s -> %s",
                lisp_addr_to_char(&uc->la), lisp_addr_to_char(&uc->ra));
        return(BAD);
    } else {
        lmlog(DBG_1, " RLOC: %s -> %s",
                lisp_addr_to_char(&uc->la), lisp_addr_to_char(&uc->ra));
        return(GOOD);
    }
}

/* TODO: should change to get_updated_interfaces */
int
ctrl_get_mappings_to_smr(lisp_ctrl_t *ctrl, mapping_t **mappings_to_smr,
        int *mcount)
{
    iface_list_elt *iface_list = NULL;
    mapping_t *m;
    iface_mappings_list *mlist;
    int mappings_ctr, ctr;

    iface_list = get_head_interface_list();

    while (iface_list) {
        if ((iface_list->iface->status_changed == TRUE)
                || (iface_list->iface->ipv4_changed == TRUE)
                || (iface_list->iface->ipv6_changed == TRUE)) {
            mlist = iface_list->iface->head_mappings_list;
            while (mlist != NULL) {
                if (iface_list->iface->status_changed == TRUE
                        || (iface_list->iface->ipv4_changed == TRUE
                                && mlist->use_ipv4_address == TRUE)
                        || (iface_list->iface->ipv6_changed == TRUE
                                && mlist->use_ipv6_address == TRUE)) {
                    m = mlist->mapping;
                    for (ctr = 0; ctr < mappings_ctr; ctr++) {
                        if (mappings_to_smr[ctr] == m) {
                            break;
                        }
                    }
                    if (mappings_to_smr[ctr] != m) {
                        mappings_to_smr[mappings_ctr] = m;
                        mappings_ctr++;
                    }
                }
                mlist = mlist->next;
            }
        }

        iface_list->iface->status_changed = FALSE;
        iface_list->iface->ipv4_changed = FALSE;
        iface_list->iface->ipv6_changed = FALSE;
        iface_list = iface_list->next;
    }
    *mcount = mappings_ctr;
    return (GOOD);
}

void
ctrl_if_addr_update(lisp_ctrl_t *ctrl, iface_t *iface, lisp_addr_t *old,
        lisp_addr_t *new)
{
    lisp_ctrl_dev_t *dev;

    dev = glist_first_data(ctrl->devices);

    /* Check if the new address is behind NAT */
    if (nat_aware == TRUE) {
        /* TODO : To be modified when implementing NAT per multiple
         * interfaces */
        nat_status = UNKNOWN;
        if (iface->status == UP) {
            /* TODO: fix nat
            initial_info_request_process(); */
        }
    }

    /* TODO: should store and pass updated rloc in the future
     * The old, and current solution is to keep a mapping between mapping_t
     * and iface to identify mapping_t(s) for which SMRs have to be sent. In
     * the future this should be decoupled and only the affected RLOC should
     * be passed to ctrl_dev */
    ctrl_if_event(dev);
    set_default_rlocs(ctrl);
}

void
ctrl_if_status_update(lisp_ctrl_t *ctrl, iface_t *iface)
{
    lisp_ctrl_dev_t *dev;
    dev = glist_first_data(ctrl->devices);
    ctrl_if_event(dev);
    set_default_rlocs(lctrl);
}

glist_t *
ctrl_default_rlocs(lisp_ctrl_t *c)
{
    return (c->default_rlocs);
}

lisp_addr_t *
ctrl_default_rloc(lisp_ctrl_t *c, int afi)
{
    lisp_addr_t *loc = NULL;
    if (lisp_addr_ip_afi(glist_first_data(c->default_rlocs)) == afi) {
        loc = glist_first_data(c->default_rlocs);
    } else if (lisp_addr_ip_afi(glist_last_data(c->default_rlocs)) == afi) {
        loc = glist_last_data(c->default_rlocs);
    }
    return (loc);
}

fwd_entry_t *
ctrl_get_forwarding_entry(packet_tuple_t *tuple)
{
    lisp_ctrl_dev_t *dev;
    dev = glist_first_data(lctrl->devices);
    return (ctrl_dev_get_fwd_entry(dev, tuple));
}


/*
 * Multicast Interface to end-hosts
 */

void
multicast_join_channel(lisp_addr_t *src, lisp_addr_t *grp)
{
    lisp_addr_t *mceid = lisp_addr_build_mc(src, grp);
    /* re_join_channel(mceid); */
    lisp_addr_del(mceid);
}

void
multicast_leave_channel(lisp_addr_t *src, lisp_addr_t *grp)
{
    lisp_addr_t *mceid = lisp_addr_build_mc(src, grp);
    /* re_leave_channel(mceid); */
    lisp_addr_del(mceid);
}

