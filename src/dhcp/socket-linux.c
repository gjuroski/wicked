/*
 * DHCP socket handling code
 *
 * Copyright (C) 2010, Olaf Kirch <okir@suse.de>
 *
 * Heavily inspired by dhcpcd, which was written by Roy Marples <roy@marples.name>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#define __FAVOR_BSD
#include <netinet/udp.h>
#undef __FAVOR_BSD

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/filter.h>
#include <netpacket/packet.h>
#define bpf_insn sock_filter

#include <wicked/logging.h>
#include <wicked/socket.h>
#include "protocol.h"
#include "dhcp.h"

/*
 * Credit where credit is due :)
 * The below BPF filter is taken from ISC DHCP
 */
static struct bpf_insn dhcp_bpf_filter [] = {
	/* Make sure this is an IP packet... */
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 12),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ETHERTYPE_IP, 0, 8),

	/* Make sure it's a UDP packet... */
	BPF_STMT(BPF_LD + BPF_B + BPF_ABS, 23),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, IPPROTO_UDP, 0, 6),

	/* Make sure this isn't a fragment... */
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 20),
	BPF_JUMP(BPF_JMP + BPF_JSET + BPF_K, 0x1fff, 4, 0),

	/* Get the IP header length... */
	BPF_STMT(BPF_LDX + BPF_B + BPF_MSH, 14),

	/* Make sure it's to the right port... */
	BPF_STMT(BPF_LD + BPF_H + BPF_IND, 16),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, DHCP_CLIENT_PORT, 0, 1),

	/* If we passed all the tests, ask for the whole packet. */
	BPF_STMT(BPF_RET + BPF_K, ~0U),

	/* Otherwise, drop it. */
	BPF_STMT(BPF_RET + BPF_K, 0),
};

static struct bpf_insn arp_bpf_filter [] = {
	/* Make sure this is an ARP packet... */
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 12),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ETHERTYPE_ARP, 0, 3),

	/* Make sure this is an ARP REPLY... */
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 20),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REPLY, 0, 1),

	/* If we passed all the tests, ask for the whole packet. */
	BPF_STMT(BPF_RET + BPF_K, ~0U),

	/* Otherwise, drop it. */
	BPF_STMT(BPF_RET + BPF_K, 0),
};

/*
 * Platform specific
 */
struct ni_capture {
	ni_dhcp_device_t *dev;
	ni_socket_t *	sock;
	int		protocol;
	struct sockaddr_ll sll;

	void *		buffer;
	size_t		mtu;
};

static ni_capture_t *	ni_capture_open(const ni_dhcp_device_t *, int, void (*)(ni_socket_t *));

static uint32_t
checksum_partial(uint32_t sum, const void *data, uint16_t len)
{
	while (len > 1) {
		sum += *(uint16_t *) data;
		data += 2;
		len -= 2;
	}

	if (len == 1) {
		uint16_t a = *(unsigned char *) data;

		sum += htons(a << 8);
	}
	return sum;
}

static inline uint16_t
checksum_fold(uint32_t sum)
{
	sum = (sum >> 16) + (sum & 0xffff);
	sum +=(sum >> 16);

	return ~sum;
}

static uint16_t
checksum(const void *data, uint16_t length)
{
	uint32_t sum;

	sum = checksum_partial(0, data, length);
	return checksum_fold(sum);
}

static uint16_t
ipudp_checksum(const struct ip *iph, const struct udphdr *uh,
		const unsigned char *data, size_t length)
{
	struct {
		struct in_addr src, dst;
		uint8_t mbz, proto;
		uint16_t length;
	} fake_header;
	uint32_t csum;

	memset(&fake_header, 0, sizeof(fake_header));
	fake_header.src = iph->ip_src;
	fake_header.dst = iph->ip_dst;
	fake_header.proto = iph->ip_p;
	fake_header.length = uh->uh_ulen;
	fake_header.mbz = 0;

	csum = checksum_partial(0, &fake_header, sizeof(fake_header));
	csum = checksum_partial(csum, uh, sizeof(*uh));
	csum = checksum_partial(csum, data, length);

	return checksum_fold(csum);
}

int
ni_dhcp_build_send_header(ni_buffer_t *bp, struct in_addr src, struct in_addr dst)
{
	const unsigned char *payload;
	unsigned int payload_len;
	unsigned int udp_len;
	struct ip *ip;
	struct udphdr *udp;

	payload = ni_buffer_head(bp);
	payload_len = ni_buffer_count(bp);

	/* Build the UDP header */
	udp = ni_buffer_push_head(bp, sizeof(struct udphdr));
	udp_len = ni_buffer_count(bp);
	udp->uh_sport = htons(DHCP_CLIENT_PORT);
	udp->uh_dport = htons(DHCP_SERVER_PORT);
	udp->uh_ulen = htons(udp_len);
	udp->uh_sum = 0;

	/* Build the IP header */
	ip = ni_buffer_push_head(bp, sizeof(struct ip));
	ip->ip_v = 4;
	ip->ip_hl = 5;
	ip->ip_id = 0;
	ip->ip_tos = IPTOS_LOWDELAY;
	ip->ip_len = htons(sizeof(*ip) + udp_len);
	ip->ip_id = 0;
	ip->ip_off = htons(IP_DF);
	ip->ip_ttl = IPDEFTTL;
	ip->ip_p = IPPROTO_UDP;
	ip->ip_src = src;
	ip->ip_dst = dst;
	if (ip->ip_dst.s_addr == 0)
		ip->ip_dst.s_addr = INADDR_BROADCAST;
	ip->ip_sum = 0;

	/* Finally, do the checksums */
	ip->ip_sum = checksum((unsigned char *) ip, sizeof(*ip));
	udp->uh_sum = ipudp_checksum(ip, udp, payload, payload_len);

	return 0;
}

static void *
check_packet_header(unsigned char *data, size_t bytes, size_t *payload_len)
{
	struct ip *iph = (struct ip *) data;
	struct udphdr *uh;
	unsigned int ihl;

	ihl = iph->ip_hl << 2;
	if (iph->ip_v != 4 || ihl < 20) {
		ni_debug_dhcp("bad IP header, ignoring");
		return NULL;
	}

	if (bytes < ihl) {
		ni_debug_dhcp("truncated IP header, ignoring");
		return NULL;
	}

	if (checksum(iph, ihl) != 0) {
		ni_debug_dhcp("bad IP header checksum, ignoring");
		return NULL;
	}

	if (bytes < ntohs(iph->ip_len)) {
		ni_debug_dhcp("truncated IP packet, ignoring");
		return NULL;
	}

	data += ihl;
	bytes -= ihl;

	if (iph->ip_p != IPPROTO_UDP) {
		ni_debug_dhcp("unexpected IP protocol, ignoring");
		return NULL;
	}

	if (bytes < sizeof(*uh)) {
		ni_debug_dhcp("truncated IP packet, ignoring");
		return NULL;
	}

	uh = (struct udphdr *) data;
	data += sizeof(*uh);
	bytes -= sizeof(*uh);

	if (ipudp_checksum(iph, uh, data, bytes) != 0) {
		ni_debug_dhcp("bad UDP checksum, ignoring");
		return NULL;
	}

	*payload_len = ntohs(iph->ip_len);
	return data;
}

/*
 * Common functions for handling timeouts
 * (Common as in: working for DHCP and ARP)
 * These are a bit of a layering violation, but I don't like too many
 * callbacks nested in callbacks...
 */
static int
__ni_dhcp_socket_get_timeout(const ni_socket_t *sock, struct timeval *tv)
{
	ni_capture_t *capture;
	ni_dhcp_device_t *dev;

	if (!(capture = sock->user_data)) {
		ni_error("dhcp socket without capture?!");
		return -1;
	}
	if (!(dev = capture->dev)) {
		ni_error("dhcp socket without device?!");
		return -1;
	}

	timerclear(tv);
	if (timerisset(&dev->retrans.deadline))
		*tv = dev->retrans.deadline;
	return timerisset(tv)? 0 : -1;
}

static void
__ni_dhcp_socket_check_timeout(ni_socket_t *sock, const struct timeval *now)
{
	ni_capture_t *capture;
	ni_dhcp_device_t *dev;

	if (!(capture = sock->user_data)) {
		ni_error("dhcp socket without capture?!");
		return;
	}
	if (!(dev = capture->dev)) {
		ni_error("dhcp socket without device?!");
		return;
	}

	if (timerisset(&dev->retrans.deadline) && timercmp(&dev->retrans.deadline, now, <))
		ni_dhcp_device_retransmit(dev);
}

static int
__ni_dhcp_common_open(ni_dhcp_device_t *dev, int protocol, void (*data_ready)(ni_socket_t *))
{

	ni_capture_t *capture;

	if ((capture = dev->capture) != NULL) {
		ni_socket_t *sock = capture->sock;

		if (sock && !sock->error && capture->protocol == protocol)
			return 0;

		ni_capture_free(dev->capture);
		dev->capture = NULL;
	}

	capture = ni_capture_open(dev, protocol, data_ready);
	if (!capture)
		return -1;
	capture->sock->get_timeout = __ni_dhcp_socket_get_timeout;
	capture->sock->check_timeout = __ni_dhcp_socket_check_timeout;

	dev->capture = capture;
	capture->dev = dev;

	return 0;
}

/*
 * This callback is invoked from the socket code when we
 * detect an incoming DHCP packet on the raw socket.
 */
static void
ni_dhcp_socket_recv(ni_socket_t *sock)
{
	ni_capture_t *capture = sock->user_data;
	void *payload;
	size_t payload_len;
	ni_buffer_t buf;
	ssize_t bytes;

	ni_debug_dhcp("%s: incoming DHCP packet", capture->dev->ifname);
	bytes = read(sock->__fd, capture->buffer, capture->mtu);
	if (bytes < 0) {
		ni_error("ni_dhcp_socket_recv: cannot read from socket: %m");
		return;
	}

	/* Make sure IP and UDP header are sane */
	payload = check_packet_header(capture->buffer, bytes, &payload_len);
	if (payload == NULL) {
		ni_debug_dhcp("bad IP/UDP packet header");
		return;
	}

	ni_buffer_init_reader(&buf, payload, payload_len);
	ni_dhcp_fsm_process_dhcp_packet(capture->dev, &buf);
}

/*
 * Open a DHCP socket for send and receive
 */
int
ni_dhcp_socket_open(ni_dhcp_device_t *dev)
{
	/* We need to bind to a port, otherwise Linux will generate
	 * ICMP_UNREACHABLE messages telling the server that there's
	 * no DHCP client listening at all.
	 *
	 * We don't actually use this fd at all, instead using our packet
	 * filter socket.
	 *
	 * (It would be nice if we did, at least in BOUND/RENEWING state
	 * where good manners would dictate unicast requests anyway).
	 */
	if (dev->listen_fd == -1) {
		struct sockaddr_in sin;
		struct ifreq ifr;
		int on = 1;
		int fd;

		if ((fd = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
			ni_error("socket: %m");
			return -1;
		}

		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
			ni_error("SO_REUSEADDR: %m");
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &on, sizeof(on)) == -1)
			ni_error("SO_RCVBUF: %m");

		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, dev->ifname, sizeof(ifr.ifr_name));
		if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) == -1)
			ni_error("SO_SOBINDTODEVICE: %m");

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = htons(DHCP_CLIENT_PORT);
		if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) == -1) {
			ni_error("bind: %m");
			close(fd);
		} else {
			dev->listen_fd = fd;
			fcntl(fd, F_SETFD, FD_CLOEXEC);
		}
	}

	return __ni_dhcp_common_open(dev, ETHERTYPE_IP, ni_dhcp_socket_recv);
}

/*
 * This callback is invoked from the socket code when we
 * detect an incoming ARP packet on the raw socket.
 */
static void
ni_arp_socket_recv(ni_socket_t *sock)
{
	ni_capture_t *capture = sock->user_data;
	ni_buffer_t buf;
	ssize_t bytes;

	ni_debug_dhcp("%s: incoming ARP packet", capture->dev->ifname);
	bytes = read(sock->__fd, capture->buffer, capture->mtu);
	if (bytes < 0) {
		ni_error("ni_arp_socket_recv: cannot read from socket: %m");
		return;
	}

	ni_buffer_init_reader(&buf, capture->buffer, capture->mtu);
	ni_dhcp_fsm_process_arp_packet(capture->dev, &buf);
}


int
ni_arp_socket_open(ni_dhcp_device_t *dev)
{
	return __ni_dhcp_common_open(dev, ETHERTYPE_ARP, ni_arp_socket_recv);
}

/*
 * Platform specific code starts here
 */
static int	ni_capture_set_filter(ni_capture_t *, int);

ni_capture_t *
ni_capture_open(const ni_dhcp_device_t *dev, int protocol, void (*data_ready)(ni_socket_t *))
{
	struct sockaddr_ll sll;
	ni_capture_t *capture = NULL;
	ni_hwaddr_t brdaddr;
	int fd = -1;

	if (dev->system.ifindex == 0) {
		ni_error("no ifindex for interface `%s'", dev->ifname);
		return NULL;
	}

	if (ni_link_address_get_broadcast(dev->system.iftype, &brdaddr) < 0) {
		ni_error("cannot get broadcast address for %s (bad iftype)", dev->ifname);
		return NULL;
	}

	if ((fd = socket (PF_PACKET, SOCK_DGRAM, htons(protocol))) < 0) {
		ni_error("socket: %m");
		return NULL;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	capture = calloc(1, sizeof(*capture));
	capture->sock = ni_socket_wrap(fd, SOCK_DGRAM);
	capture->protocol = protocol;

	capture->sll.sll_family = AF_PACKET;
	capture->sll.sll_protocol = htons(protocol); // 0; /* will be filled in on xmit */
	capture->sll.sll_ifindex = dev->system.ifindex;
	capture->sll.sll_hatype = htons(dev->system.arp_type);
	capture->sll.sll_halen = brdaddr.len;
	memcpy(&capture->sll.sll_addr, brdaddr.data, brdaddr.len);

	if (ni_capture_set_filter(capture, protocol) < 0)
		goto failed;

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = PF_PACKET;
	sll.sll_protocol = htons(protocol);
	sll.sll_ifindex = dev->system.ifindex;

	if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) == -1) {
		ni_error("bind: %m");
		goto failed;
	}

	capture->mtu = dev->system.mtu;
	if (capture->mtu == 0)
		capture->mtu = MTU_MAX;
	capture->buffer = malloc(capture->mtu);

	capture->sock->data_ready = data_ready;
	capture->sock->user_data = capture;
	ni_socket_activate(capture->sock);
	return capture;

failed:
	if (capture)
		ni_capture_free(capture);
	else if (fd >= 0)
		close(fd);
	return NULL;
}

static int
ni_capture_set_filter(ni_capture_t *cap, int protocol)
{
	struct sock_fprog pf;
	static int done = 0;

	/* Initialize packet filters if we haven't done so */
	if (!done) {
		/* We need to massage the filters for Linux cooked packets */
		dhcp_bpf_filter[1].jf = 0; /* skip the IP packet type check */
		dhcp_bpf_filter[2].k -= ETH_HLEN;
		dhcp_bpf_filter[4].k -= ETH_HLEN;
		dhcp_bpf_filter[6].k -= ETH_HLEN;
		dhcp_bpf_filter[7].k -= ETH_HLEN;

		arp_bpf_filter[1].jf = 0; /* skip the IP packet type check */
		arp_bpf_filter[2].k -= ETH_HLEN;

		done = 1;
	}

	/* Install the DHCP filter */
	memset(&pf, 0, sizeof(pf));
	if (protocol == ETHERTYPE_ARP) {
		pf.filter = arp_bpf_filter;
		pf.len = sizeof(arp_bpf_filter) / sizeof(arp_bpf_filter[0]);
	} else {
		pf.filter = dhcp_bpf_filter;
		pf.len = sizeof(dhcp_bpf_filter) / sizeof(dhcp_bpf_filter[0]);
	}

	if (setsockopt(cap->sock->__fd, SOL_SOCKET, SO_ATTACH_FILTER, &pf, sizeof(pf)) < 0) {
		ni_error("SO_ATTACH_FILTER: %m");
		return -1;
	}

	return 0;
}

ssize_t
ni_capture_broadcast(const ni_capture_t *capture, const void *data, size_t len)
{
	ssize_t rv;

	if (capture == NULL) {
		ni_error("%s: no capture handle", __FUNCTION__);
		return -1;
	}

	rv = sendto(capture->sock->__fd, data, len, 0,
			(struct sockaddr *) &capture->sll,
			sizeof(capture->sll));
	if (rv < 0)
		ni_error("unable to send dhcp packet: %m");

	return rv;
}

void
ni_capture_free(ni_capture_t *capture)
{
	if (capture->sock)
		ni_socket_close(capture->sock);
	if (capture->buffer)
		free(capture->buffer);
	free(capture);
}
