/*
 * dhcpcd - DHCP client daemon
 * Copyright (c) 2006-2013 Roy Marples <roy@marples.name>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __linux__
# include <asm/types.h> /* needed for 2.4 kernels for the below header */
# include <linux/filter.h>
# include <linux/if_packet.h>
# define bpf_insn		sock_filter
# define BPF_SKIPTYPE
# define BPF_ETHCOOK		-ETH_HLEN
# define BPF_WHOLEPACKET	0x0fffffff /* work around buggy LPF filters */
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "common.h"
#include "dhcp.h"
#include "ipv4.h"
#include "bpf-filter.h"

/* Broadcast address for IPoIB */
static const uint8_t ipv4_bcast_addr[] = {
	0x00, 0xff, 0xff, 0xff,
	0xff, 0x12, 0x40, 0x1b, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

#ifdef INET
int
ipv4_opensocket(struct interface *ifp, int protocol)
{
	int s;
	union sockunion {
		struct sockaddr sa;
		struct sockaddr_in sin;
		struct sockaddr_ll sll;
		struct sockaddr_storage ss;
	} su;
	struct sock_fprog pf;
	int *fd;
	struct dhcp_state *state;
#ifdef PACKET_AUXDATA
	int n;
#endif

	if ((s = socket(PF_PACKET, SOCK_DGRAM, htons(protocol))) == -1)
		return -1;

	memset(&su, 0, sizeof(su));
	su.sll.sll_family = PF_PACKET;
	su.sll.sll_protocol = htons(protocol);
	su.sll.sll_ifindex = ifp->index;
	/* Install the DHCP filter */
	memset(&pf, 0, sizeof(pf));
	if (protocol == ETHERTYPE_ARP) {
		pf.filter = UNCONST(arp_bpf_filter);
		pf.len = arp_bpf_filter_len;
	} else {
		pf.filter = UNCONST(dhcp_bpf_filter);
		pf.len = dhcp_bpf_filter_len;
	}
	if (setsockopt(s, SOL_SOCKET, SO_ATTACH_FILTER, &pf, sizeof(pf)) != 0)
		goto eexit;
#ifdef PACKET_AUXDATA
	n = 1;
	if (setsockopt(s, SOL_PACKET, PACKET_AUXDATA, &n, sizeof(n)) != 0) {
		if (errno != ENOPROTOOPT)
			goto eexit;
	}
#endif
	if (set_cloexec(s) == -1)
		goto eexit;
	if (set_nonblock(s) == -1)
		goto eexit;
	if (bind(s, &su.sa, sizeof(su)) == -1)
		goto eexit;
	state = D_STATE(ifp);
	if (protocol == ETHERTYPE_ARP)
		fd = &state->arp_fd;
	else
		fd = &state->raw_fd;
	if (*fd != -1)
		close(*fd);
	*fd = s;
	return s;

eexit:
	close(s);
	return -1;
}

ssize_t
ipv4_sendrawpacket(const struct interface *ifp, int protocol,
    const void *data, ssize_t len)
{
	const struct dhcp_state *state;
	union sockunion {
		struct sockaddr sa;
		struct sockaddr_ll sll;
		struct sockaddr_storage ss;
	} su;
	int fd;

	memset(&su, 0, sizeof(su));
	su.sll.sll_family = AF_PACKET;
	su.sll.sll_protocol = htons(protocol);
	su.sll.sll_ifindex = ifp->index;
	su.sll.sll_hatype = htons(ifp->family);
	su.sll.sll_halen = ifp->hwlen;
	if (ifp->family == ARPHRD_INFINIBAND)
		memcpy(&su.sll.sll_addr,
		    &ipv4_bcast_addr, sizeof(ipv4_bcast_addr));
	else
		memset(&su.sll.sll_addr, 0xff, ifp->hwlen);
	state = D_CSTATE(ifp);
	if (protocol == ETHERTYPE_ARP)
		fd = state->arp_fd;
	else
		fd = state->raw_fd;

	return sendto(fd, data, len, 0, &su.sa, sizeof(su));
}

ssize_t
ipv4_getrawpacket(struct interface *ifp, int protocol,
    void *data, ssize_t len, int *partialcsum)
{
	struct iovec iov = {
		.iov_base = data,
		.iov_len = len,
	};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	struct dhcp_state *state;
#ifdef PACKET_AUXDATA
	unsigned char cmsgbuf[CMSG_LEN(sizeof(struct tpacket_auxdata))];
	struct cmsghdr *cmsg;
	struct tpacket_auxdata *aux;
#endif

	ssize_t bytes;
	int fd = -1;

#ifdef PACKET_AUXDATA
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
#endif

	state = D_STATE(ifp);
	if (protocol == ETHERTYPE_ARP)
		fd = state->arp_fd;
	else
		fd = state->raw_fd;
	bytes = recvmsg(fd, &msg, 0);
	if (bytes == -1)
		return errno == EAGAIN ? 0 : -1;
	if (partialcsum != NULL) {
		*partialcsum = 0;
#ifdef PACKET_AUXDATA
		for (cmsg = CMSG_FIRSTHDR(&msg);
		     cmsg;
		     cmsg = CMSG_NXTHDR(&msg, cmsg))
		{
			if (cmsg->cmsg_level == SOL_PACKET &&
			    cmsg->cmsg_type == PACKET_AUXDATA) {
				aux = (void *)CMSG_DATA(cmsg);
				*partialcsum = aux->tp_status &
				    TP_STATUS_CSUMNOTREADY;
			}
		}
#endif
	}
	return bytes;
}
#endif
