/*
 * EIGRP Sending and Receiving EIGRP Query Packets.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "thread.h"
#include "memory.h"
#include "linklist.h"
#include "prefix.h"
#include "if.h"
#include "table.h"
#include "sockunion.h"
#include "stream.h"
#include "log.h"
#include "sockopt.h"
#include "checksum.h"
#include "md5.h"
#include "vty.h"

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_macros.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_memory.h"

uint32_t eigrp_query_send_all(eigrp_t *eigrp)
{
	eigrp_interface_t *iface;
	struct listnode *node, *node2, *nnode2;
	eigrp_prefix_descriptor_t *pe;
	uint32_t counter;

	if (eigrp == NULL) {
		zlog_debug("EIGRP Routing Process not enabled");
		return 0;
	}

	counter = 0;
	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, iface)) {
		eigrp_send_query(iface);
		counter++;
	}

	for (ALL_LIST_ELEMENTS(eigrp->topology_changes_internalIPV4, node2,
			       nnode2, pe)) {
		if (pe->req_action & EIGRP_FSM_NEED_QUERY) {
			pe->req_action &= ~EIGRP_FSM_NEED_QUERY;
			listnode_delete(eigrp->topology_changes_internalIPV4,
					pe);
		}
	}

	return counter;
}

/*EIGRP QUERY read function*/
void eigrp_query_receive(eigrp_t *eigrp, eigrp_neighbor_t *nbr,
			 struct eigrp_header *eigrph, struct stream *pkt,
			 eigrp_interface_t *ei, int length)
{
    struct eigrp_fsm_action_message msg;
    eigrp_route_descriptor_t *route;

    /* increment statistics. */
    ei->stats.rcvd.query++;

    // record neighbor seq were processing
    nbr->recv_sequence_number = ntohl(eigrph->sequence);

    // process all TLVs in the packet
    while (pkt->endp > pkt->getp) {
	route = (nbr->tlv_decoder)(eigrp, nbr, pkt, length);

	// should hsve got route off the packet, but one never knowd
	if (route) {
	    msg.packet_type = EIGRP_OPC_QUERY;
	    msg.eigrp = eigrp;
	    msg.data_type = EIGRP_INT;
	    msg.adv_router = nbr;
	    msg.route = route;
	    msg.metrics = route->vmetric;
	    msg.prefix = route->prefix;
	    eigrp_fsm_event(&msg);
	} else {
	    // neighbor sent corrupted packet - flush remaining packet
	    break;
	}
    }

    eigrp_hello_send_ack(eigrp, nbr);
    eigrp_query_send_all(eigrp);
    eigrp_update_send_all(eigrp, nbr->ei);
}

void eigrp_send_query(eigrp_interface_t *ei)
{
    eigrp_t	*eigrp = ei->eigrp;
    eigrp_neighbor_t *nbr;
    eigrp_prefix_descriptor_t *prefix;
    eigrp_packet_t *ep = NULL;
    uint16_t length = EIGRP_HEADER_LEN;
    bool has_tlv = false;
    bool new_packet = true;
    uint16_t mtu = EIGRP_PACKET_MTU(ei->ifp->mtu);
    struct listnode *node, *nnode, *node2, *nnode2;

    for (ALL_LIST_ELEMENTS(eigrp->topology_changes_internalIPV4, node, nnode, prefix)) {
	if (!(prefix->req_action & EIGRP_FSM_NEED_QUERY))
	    continue;

	if (new_packet) {
	    ep = eigrp_packet_new(mtu, NULL);

	    /* Prepare EIGRP INIT UPDATE header */
	    eigrp_packet_header_init(EIGRP_OPC_QUERY, eigrp, ep->s, 0,
				     eigrp->sequence_number, 0);

	    // encode Authentication TLV, if needed
	    if ((ei->params.auth_type == EIGRP_AUTH_TYPE_MD5)
		&& (ei->params.auth_keychain != NULL)) {
		length += eigrp_add_authTLV_MD5_encode(ep->s, ei);
	    }
	    new_packet = false;
	}

	length += (nbr->tlv_encoder)(eigrp, nbr, ep->s, prefix);
	has_tlv = true;
	for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
	    if (nbr->state == EIGRP_NEIGHBOR_UP)
		listnode_add(prefix->rij, nbr);
	}

	if (length + EIGRP_TLV_MAX_IPV4_BYTE > mtu) {
	    if ((ei->params.auth_type == EIGRP_AUTH_TYPE_MD5)
		&& ei->params.auth_keychain != NULL) {
		eigrp_make_md5_digest(ei, ep->s, EIGRP_AUTH_UPDATE_FLAG);
	    }

	    eigrp_packet_checksum(ei, ep->s, length);
	    ep->length = length;

	    ep->dst.s_addr = htonl(EIGRP_MULTICAST_ADDRESS);

	    ep->sequence_number = eigrp->sequence_number;
	    eigrp->sequence_number++;

	    for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
		struct eigrp_packet *dup;

		if (nbr->state != EIGRP_NEIGHBOR_UP)
		    continue;

		dup = eigrp_packet_duplicate(ep, nbr);

		/*Put packet to retransmission queue*/
		eigrp_fifo_push(nbr->retrans_queue, dup);

		if (nbr->retrans_queue->count == 1)
		    eigrp_send_packet_reliably(nbr);
	    }

	    has_tlv = false;
	    length = 0;
	    eigrp_packet_free(ep);
	    ep = NULL;
	    new_packet = true;
	}
    }

    if (!has_tlv) {
	if (ep)
	    eigrp_packet_free(ep);
	return;
    }

    if ((ei->params.auth_type == EIGRP_AUTH_TYPE_MD5)
	&& ei->params.auth_keychain != NULL)
	eigrp_make_md5_digest(ei, ep->s, EIGRP_AUTH_UPDATE_FLAG);

    /* EIGRP Checksum */
    eigrp_packet_checksum(ei, ep->s, length);

    ep->length = length;
    ep->dst.s_addr = htonl(EIGRP_MULTICAST_ADDRESS);

    /* This ack number we await from neighbor */
    ep->sequence_number = eigrp->sequence_number;
    eigrp->sequence_number++;

    for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
	struct eigrp_packet *dup;

	if (nbr->state != EIGRP_NEIGHBOR_UP)
	    continue;

	dup = eigrp_packet_duplicate(ep, nbr);
	/* Put packet to retransmission queue */
	eigrp_fifo_push(nbr->retrans_queue, dup);

	if (nbr->retrans_queue->count == 1)
	    eigrp_send_packet_reliably(nbr);
    }

    eigrp_packet_free(ep);
}
