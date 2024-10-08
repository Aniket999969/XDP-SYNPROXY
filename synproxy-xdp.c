#include <stdbool.h>
#include <stddef.h>

#include <uapi/asm-generic/errno.h>
#include <uapi/linux/bpf.h>
#include <uapi/linux/pkt_cls.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/in.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/ipv6.h>
#include <uapi/linux/tcp.h>
#include <uapi/linux/netfilter/nf_conntrack_common.h>
#include <linux/minmax.h>
#include <vdso/time64.h>
#include <asm/unaligned.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define DEFAULT_MSS4 1460
#define DEFAULT_MSS6 1440
#define DEFAULT_WSCALE 7
#define DEFAULT_TTL 64
#define MAX_ALLOWED_PORTS 65535

struct bpf_map_def SEC("maps") values = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(__u32),
	.value_size = sizeof(__u64),
	.max_entries = 2,
};

struct bpf_map_def SEC("maps") allowed_ports = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(__u32),
	.value_size = sizeof(__u16),
	.max_entries = MAX_ALLOWED_PORTS,
};

#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFSET 0x1fff

#define NEXTHDR_TCP 6
#define TCPOPT_NOP 1
#define TCPOPT_EOL 0
#define TCPOPT_MSS 2
#define TCPOPT_WINDOW 3
#define TCPOPT_SACK_PERM 4
#define TCPOPT_TIMESTAMP 8

#define TCPOLEN_MSS 4
#define TCPOLEN_WINDOW 3
#define TCPOLEN_SACK_PERM 2
#define TCPOLEN_TIMESTAMP 10

#define TCP_MAX_WSCALE 14U

#define TS_OPT_WSCALE_MASK 0xf
#define TS_OPT_SACK BIT(4)
#define TS_OPT_ECN BIT(5)
#define TSBITS 6
#define TSMASK (((__u32)1 << TSBITS) - 1)

#define TCP_TS_HZ 1000

#define IPV4_MAXLEN 60
#define TCP_MAXLEN 60

static __always_inline void swap_eth_addr(__u8 *a, __u8 *b)
{
	__u8 tmp[ETH_ALEN];

	__builtin_memcpy(tmp, a, ETH_ALEN);
	__builtin_memcpy(a, b, ETH_ALEN);
	__builtin_memcpy(b, tmp, ETH_ALEN);
}

static __always_inline __u16 csum_fold(__u32 csum)
{
	csum = (csum & 0xffff) + (csum >> 16);
	csum = (csum & 0xffff) + (csum >> 16);
	return (__u16)~csum;
}

static __always_inline __u16 csum_tcpudp_magic(__be32 saddr, __be32 daddr,
					       __u32 len, __u8 proto,
					       __u32 csum)
{
	__u64 s = csum;

	s += (__u32)saddr;
	s += (__u32)daddr;
#if defined(__BIG_ENDIAN__)
	s += proto + len;
#elif defined(__LITTLE_ENDIAN__)
	s += (proto + len) << 8;
#else
#error Unknown endian
#endif
	s = (s & 0xffffffff) + (s >> 32);
	s = (s & 0xffffffff) + (s >> 32);

	return csum_fold((__u32)s);
}

static __always_inline __u16 csum_ipv6_magic(const struct in6_addr *saddr,
					     const struct in6_addr *daddr,
					     __u32 len, __u8 proto, __u32 csum)
{
	__u64 sum = csum;
	int i;

#pragma unroll
	for (i = 0; i < 4; i++)
		sum += (__u32)saddr->s6_addr32[i];

#pragma unroll
	for (i = 0; i < 4; i++)
		sum += (__u32)daddr->s6_addr32[i];

	// Don't combine additions to avoid 32-bit overflow.
	sum += bpf_htonl(len);
	sum += bpf_htonl(proto);

	sum = (sum & 0xffffffff) + (sum >> 32);
	sum = (sum & 0xffffffff) + (sum >> 32);

	return csum_fold((__u32)sum);
}

static __always_inline u64 tcp_clock_ns(void)
{
	return bpf_ktime_get_ns();
}

static __always_inline __u32 tcp_ns_to_ts(__u64 ns)
{
	return div_u64(ns, NSEC_PER_SEC / TCP_TS_HZ);
}

static __always_inline __u32 tcp_time_stamp_raw(void)
{
	return tcp_ns_to_ts(tcp_clock_ns());
}

static __always_inline bool cookie_init_timestamp_raw(struct tcphdr 
*tcp_header,
						      __u16 tcp_len,
						      __be32 *tsval,
						      __be32 *tsecr,
						      void *data_end)
{
	u8 wscale = TS_OPT_WSCALE_MASK;
	bool option_timestamp = false;
	bool option_sack = false;
	u8 *ptr, *end;
	u32 cookie;
	int i;

	ptr = (u8 *)(tcp_header + 1);
	end = (u8 *)tcp_header + tcp_len;

	for (i = 0; i < 10; i++) {
		u8 opcode, opsize;

		if (ptr >= end)
			break;
		if (ptr + 1 > data_end)
			return false;

		opcode = ptr[0];

		if (opcode == TCPOPT_EOL)
			break;
		if (opcode == TCPOPT_NOP) {
			++ptr;
			continue;
		}

		if (ptr + 1 >= end)
			break;
		if (ptr + 2 > data_end)
			return false;
		opsize = ptr[1];
		if (opsize < 2)
			break;

		if (ptr + opsize > end)
			break;

		switch (opcode) {
		case TCPOPT_WINDOW:
			if (opsize == TCPOLEN_WINDOW) {
				if (ptr + TCPOLEN_WINDOW > data_end)
					return false;
				wscale = min_t(u8, ptr[2], TCP_MAX_WSCALE);
			}
			break;
		case TCPOPT_TIMESTAMP:
			if (opsize == TCPOLEN_TIMESTAMP) {
				if (ptr + TCPOLEN_TIMESTAMP > data_end)
					return false;
				option_timestamp = true;
				/* Client's tsval becomes our tsecr. */
				*tsecr = get_unaligned((__be32 *)(ptr + 2));
			}
			break;
		case TCPOPT_SACK_PERM:
			if (opsize == TCPOLEN_SACK_PERM)
				option_sack = true;
			break;
		}

		ptr += opsize;
	}

	if (!option_timestamp)
		return false;

	cookie = tcp_time_stamp_raw() & ~TSMASK;
	cookie |= wscale & TS_OPT_WSCALE_MASK;
	if (option_sack)
		cookie |= TS_OPT_SACK;
	if (tcp_header->ece && tcp_header->cwr)
		cookie |= TS_OPT_ECN;
	*tsval = cpu_to_be32(cookie);

	return true;
}

static __always_inline void values_get_tcpipopts(__u16 *mss, __u8 *wscale,
						 __u8 *ttl, bool ipv6)
{
	__u32 key = 0;
	__u64 *value;

	value = bpf_map_lookup_elem(&values, &key);
	if (value && *value != 0) {
		if (ipv6)
			*mss = (*value >> 32) & 0xffff;
		else
			*mss = *value & 0xffff;
		*wscale = (*value >> 16) & 0xf;
		*ttl = (*value >> 24) & 0xff;
		return;
	}

	*mss = ipv6 ? DEFAULT_MSS6 : DEFAULT_MSS4;
	*wscale = DEFAULT_WSCALE;
	*ttl = DEFAULT_TTL;
}

static __always_inline void values_inc_synacks(void)
{
	__u32 key = 1;
	__u32 *value;

	value = bpf_map_lookup_elem(&values, &key);
	if (value)
		__sync_fetch_and_add(value, 1);
}

static __always_inline bool check_port_allowed(__u16 port)
{
	__u32 i;

	for (i = 0; i < MAX_ALLOWED_PORTS; i++) {
		__u32 key = i;
		__u16 *value;

		value = bpf_map_lookup_elem(&allowed_ports, &key);

		if (!value)
			break;
		// 0 is a terminator value. Check it first to avoid matching on
		// a forbidden port == 0 and returning true.
		if (*value == 0)
			break;

		if (*value == port)
			return true;
	}

	return false;
}

struct header_pointers {
	struct ethhdr *eth;
	struct iphdr *ipv4;
	struct ipv6hdr *ipv6;
	struct tcphdr *tcp;
	__u16 tcp_len;
};

static __always_inline int tcp_dissect(void *data, void *data_end,
				       struct header_pointers *hdr)
{
	hdr->eth = data;
	if (hdr->eth + 1 > data_end)
		return XDP_DROP;

	switch (bpf_ntohs(hdr->eth->h_proto)) {
	case ETH_P_IP:
		hdr->ipv6 = NULL;

		hdr->ipv4 = (void *)hdr->eth + sizeof(*hdr->eth);
		if (hdr->ipv4 + 1 > data_end)
			return XDP_DROP;
		if (hdr->ipv4->ihl * 4 < sizeof(*hdr->ipv4))
			return XDP_DROP;
		if (hdr->ipv4->version != 4)
			return XDP_DROP;

		if (hdr->ipv4->protocol != IPPROTO_TCP)
			return XDP_PASS;

		hdr->tcp = (void *)hdr->ipv4 + hdr->ipv4->ihl * 4;
		break;
	case ETH_P_IPV6:
		hdr->ipv4 = NULL;

		hdr->ipv6 = (void *)hdr->eth + sizeof(*hdr->eth);
		if (hdr->ipv6 + 1 > data_end)
			return XDP_DROP;
		if (hdr->ipv6->version != 6)
			return XDP_DROP;

		// XXX: Extension headers are not supported and could circumvent
		// XDP SYN flood protection.
		if (hdr->ipv6->nexthdr != NEXTHDR_TCP)
			return XDP_PASS;

		hdr->tcp = (void *)hdr->ipv6 + sizeof(*hdr->ipv6);
		break;
	default:
		// XXX: VLANs will circumvent XDP SYN flood protection.
		return XDP_PASS;
	}

	if (hdr->tcp + 1 > data_end)
		return XDP_DROP;
	hdr->tcp_len = hdr->tcp->doff * 4;
	if (hdr->tcp_len < sizeof(*hdr->tcp))
		return XDP_DROP;

	return XDP_TX;
}

static __always_inline __u8 tcp_mkoptions(__be32 *buf, __be32 *tsopt, 
__u16 mss,
					  __u8 wscale)
{
	__be32 *start = buf;

	*buf++ = bpf_htonl((TCPOPT_MSS << 24) | (TCPOLEN_MSS << 16) | mss);

	if (!tsopt)
		return buf - start;

	if (tsopt[0] & bpf_htonl(1 << 4))
		*buf++ = bpf_htonl((TCPOPT_SACK_PERM << 24) |
				   (TCPOLEN_SACK_PERM << 16) |
				   (TCPOPT_TIMESTAMP << 8) |
				   TCPOLEN_TIMESTAMP);
	else
		*buf++ = bpf_htonl((TCPOPT_NOP << 24) |
				   (TCPOPT_NOP << 16) |
				   (TCPOPT_TIMESTAMP << 8) |
				   TCPOLEN_TIMESTAMP);
	*buf++ = tsopt[0];
	*buf++ = tsopt[1];

	if ((tsopt[0] & bpf_htonl(0xf)) != bpf_htonl(0xf))
		*buf++ = bpf_htonl((TCPOPT_NOP << 24) |
				   (TCPOPT_WINDOW << 16) |
				   (TCPOLEN_WINDOW << 8) |
				   wscale);

	return buf - start;
}

static __always_inline void tcp_gen_synack(struct tcphdr *tcp_header,
					   __u32 cookie, __be32 *tsopt,
					   __u16 mss, __u8 wscale)
{
	void *tcp_options;

	tcp_flag_word(tcp_header) = TCP_FLAG_SYN | TCP_FLAG_ACK;
	if (tsopt && (tsopt[0] & bpf_htonl(1 << 5)))
		tcp_flag_word(tcp_header) |= TCP_FLAG_ECE;
	tcp_header->doff = 5; // doff is part of tcp_flag_word.
	swap(tcp_header->source, tcp_header->dest);
	tcp_header->ack_seq = bpf_htonl(bpf_ntohl(tcp_header->seq) + 1);
	tcp_header->seq = bpf_htonl(cookie);
	tcp_header->window = 0;
	tcp_header->urg_ptr = 0;
	tcp_header->check = 0; // Rely on hardware checksum offload.

	tcp_options = (void *)(tcp_header + 1);
	tcp_header->doff += tcp_mkoptions(tcp_options, tsopt, mss, wscale);
}

static __always_inline void tcpv4_gen_synack(struct header_pointers *hdr,
					     __u32 cookie, __be32 *tsopt)
{
	__u8 wscale;
	__u16 mss;
	__u8 ttl;

	values_get_tcpipopts(&mss, &wscale, &ttl, false);

	swap_eth_addr(hdr->eth->h_source, hdr->eth->h_dest);

	swap(hdr->ipv4->saddr, hdr->ipv4->daddr);
	hdr->ipv4->check = 0; // Rely on hardware checksum offload.
	hdr->ipv4->tos = 0;
	hdr->ipv4->id = 0;
	hdr->ipv4->ttl = ttl;

	tcp_gen_synack(hdr->tcp, cookie, tsopt, mss, wscale);

	hdr->tcp_len = hdr->tcp->doff * 4;
	hdr->ipv4->tot_len = bpf_htons(sizeof(*hdr->ipv4) + hdr->tcp_len);
}

static __always_inline void tcpv6_gen_synack(struct header_pointers *hdr,
					     __u32 cookie, __be32 *tsopt)
{
	__u8 wscale;
	__u16 mss;
	__u8 ttl;

	values_get_tcpipopts(&mss, &wscale, &ttl, true);

	swap_eth_addr(hdr->eth->h_source, hdr->eth->h_dest);

	swap(hdr->ipv6->saddr, hdr->ipv6->daddr);
	*(__be32 *)hdr->ipv6 = bpf_htonl(0x60000000);
	hdr->ipv6->hop_limit = ttl;

	tcp_gen_synack(hdr->tcp, cookie, tsopt, mss, wscale);

	hdr->tcp_len = hdr->tcp->doff * 4;
	hdr->ipv6->payload_len = bpf_htons(hdr->tcp_len);
}

static __always_inline int syncookie_handle_syn(struct header_pointers *hdr,
						struct xdp_md *ctx,
						void *data, void *data_end)
{
	__u32 old_pkt_size, new_pkt_size;
	// Unlike clang 10, clang 11 and 12 generate code that doesn't pass the
	// BPF verifier if tsopt is not volatile. Volatile forces it to store
	// the pointer value and use it directly, otherwise tcp_mkoptions is
	// (mis)compiled like this:
	//   if (!tsopt)
	//       return buf - start;
	//   reg = stored_return_value_of_bpf_tcp_raw_gen_tscookie;
	//   if (reg == 0)
	//       tsopt = tsopt_buf;
	//   else
	//       tsopt = NULL;
	//   ...
	//   *buf++ = tsopt[1];
	// It creates a dead branch where tsopt is assigned NULL, but the
	// verifier can't prove it's dead and blocks the program.
	__be32 * volatile tsopt = NULL;
	__be32 tsopt_buf[2];
	void *ip_header;
	__u16 ip_len;
	__u32 cookie;
	__s64 value;

	if (hdr->ipv4) {
		// Check the IPv4 and TCP checksums before creating a SYNACK.
		value = bpf_csum_diff(0, 0, (void *)hdr->ipv4, hdr->ipv4->ihl * 4, 0);
		if (value < 0)
			return XDP_ABORTED;
		if (csum_fold(value) != 0)
			return XDP_DROP; // Bad IPv4 checksum.

		value = bpf_csum_diff(0, 0, (void *)hdr->tcp, hdr->tcp_len, 0);
		if (value < 0)
			return XDP_ABORTED;
		if (csum_tcpudp_magic(hdr->ipv4->saddr, hdr->ipv4->daddr,
				      hdr->tcp_len, IPPROTO_TCP, value) != 0)
			return XDP_DROP; // Bad TCP checksum.

		ip_header = hdr->ipv4;
		ip_len = sizeof(*hdr->ipv4);
	} else if (hdr->ipv6) {
		// Check the TCP checksum before creating a SYNACK.
		value = bpf_csum_diff(0, 0, (void *)hdr->tcp, hdr->tcp_len, 0);
		if (value < 0)
			return XDP_ABORTED;
		if (csum_ipv6_magic(&hdr->ipv6->saddr, &hdr->ipv6->daddr,
				    hdr->tcp_len, IPPROTO_TCP, value) != 0)
			return XDP_DROP; // Bad TCP checksum.

		ip_header = hdr->ipv6;
		ip_len = sizeof(*hdr->ipv6);
	} else {
		return XDP_ABORTED;
	}

	// Issue SYN cookies on allowed ports, drop SYN packets on
	// blocked ports.
	if (!check_port_allowed(bpf_ntohs(hdr->tcp->dest)))
		return XDP_DROP;

	value = bpf_tcp_raw_gen_syncookie_ipv4(ip_header, ip_len,
					  (void *)hdr->tcp, hdr->tcp_len);
	if (value < 0)
		return XDP_ABORTED;
	cookie = (__u32)value;

	if (cookie_init_timestamp_raw((void *)hdr->tcp, hdr->tcp_len,
				      &tsopt_buf[0], &tsopt_buf[1], data_end))
		tsopt = tsopt_buf;

	// Check that there is enough space for a SYNACK. It also covers
	// the check that the destination of the __builtin_memmove below
	// doesn't overflow.
	if (data + sizeof(*hdr->eth) + ip_len + TCP_MAXLEN > data_end)
		return XDP_ABORTED;

	if (hdr->ipv4) {
		if (hdr->ipv4->ihl * 4 > sizeof(*hdr->ipv4)) {
			struct tcphdr *new_tcp_header;

			new_tcp_header = data + sizeof(*hdr->eth) + sizeof(*hdr->ipv4);
			__builtin_memmove(new_tcp_header, hdr->tcp, sizeof(*hdr->tcp));
			hdr->tcp = new_tcp_header;

			hdr->ipv4->ihl = sizeof(*hdr->ipv4) / 4;
		}

		tcpv4_gen_synack(hdr, cookie, tsopt);
	} else if (hdr->ipv6) {
		tcpv6_gen_synack(hdr, cookie, tsopt);
	} else {
		return XDP_ABORTED;
	}

	// Recalculate checksums.
	hdr->tcp->check = 0;
	value = bpf_csum_diff(0, 0, (void *)hdr->tcp, hdr->tcp_len, 0);
	if (value < 0)
		return XDP_ABORTED;
	if (hdr->ipv4) {
		hdr->tcp->check = csum_tcpudp_magic(hdr->ipv4->saddr,
						    hdr->ipv4->daddr,
						    hdr->tcp_len,
						    IPPROTO_TCP,
						    value);

		hdr->ipv4->check = 0;
		value = bpf_csum_diff(0, 0, (void *)hdr->ipv4, sizeof(*hdr->ipv4), 0);
		if (value < 0)
			return XDP_ABORTED;
		hdr->ipv4->check = csum_fold(value);
	} else if (hdr->ipv6) {
		hdr->tcp->check = csum_ipv6_magic(&hdr->ipv6->saddr,
						  &hdr->ipv6->daddr,
						  hdr->tcp_len,
						  IPPROTO_TCP,
						  value);
	} else {
		return XDP_ABORTED;
	}

	// Set the new packet size.
	old_pkt_size = data_end - data;
	new_pkt_size = sizeof(*hdr->eth) + ip_len + hdr->tcp->doff * 4;
	if (bpf_xdp_adjust_tail(ctx, new_pkt_size - old_pkt_size))
		return XDP_ABORTED;

	values_inc_synacks();

	return XDP_TX;
}

static __always_inline int syncookie_handle_ack(struct header_pointers *hdr)
{
	int err;

	if (hdr->ipv4)
		err = bpf_tcp_raw_check_syncookie_ipv4(hdr->ipv4, sizeof(*hdr->ipv4),
						  (void *)hdr->tcp, hdr->tcp_len);
	else
		return XDP_ABORTED;
	if (err)
		return XDP_DROP;

	return XDP_PASS;
}

SEC("xdp/syncookie")
int syncookie_xdp(struct xdp_md *ctx) {
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct header_pointers hdr;
	struct bpf_sock_tuple tup;
	struct bpf_nf_conn *ct;
	__u32 tup_size;
	__s64 value;
	int ret;

	ret = tcp_dissect(data, data_end, &hdr);
	if (ret != XDP_TX)
		return ret;

	if (hdr.ipv4) {
		// TCP doesn't normally use fragments, and XDP can't reassemble them.
		if ((hdr.ipv4->frag_off & bpf_htons(IP_DF | IP_MF | IP_OFFSET)) != bpf_htons(IP_DF))
		 return XDP_DROP;

		tup.ipv4.saddr = hdr.ipv4->saddr;
		tup.ipv4.daddr = hdr.ipv4->daddr;
		tup.ipv4.sport = hdr.tcp->source;
		tup.ipv4.dport = hdr.tcp->dest;
		tup_size = sizeof(tup.ipv4);
	} else if (hdr.ipv6) {
		__builtin_memcpy(tup.ipv6.saddr, &hdr.ipv6->saddr, 
sizeof(tup.ipv6.saddr));
		__builtin_memcpy(tup.ipv6.daddr, &hdr.ipv6->daddr, 
sizeof(tup.ipv6.daddr));
		tup.ipv6.sport = hdr.tcp->source;
		tup.ipv6.dport = hdr.tcp->dest;
		tup_size = sizeof(tup.ipv6);
	} else {
		// The verifier can't track that either ipv4 or ipv6 is not NULL.
		return XDP_ABORTED;
	}

	value = 0; // Flags.
	ct = bpf_ct_lookup_tcp(ctx, &tup, tup_size, BPF_F_CURRENT_NETNS, &value);
	if (ct) {
		unsigned long status = ct->status;

		bpf_ct_release(ct);
		if (status & IPS_CONFIRMED_BIT)
			return XDP_PASS;
	} else if (value != -ENOENT) {
		return XDP_ABORTED;
	}

	// value == -ENOENT || !(status & IPS_CONFIRMED_BIT)

	if ((hdr.tcp->syn ^ hdr.tcp->ack) != 1)
		return XDP_DROP;

	// Grow the TCP header to TCP_MAXLEN to be able to pass any hdr.tcp_len
	// to bpf_tcp_raw_gen_syncookie and pass the verifier.
	if (bpf_xdp_adjust_tail(ctx, TCP_MAXLEN - hdr.tcp_len))
		return XDP_ABORTED;

	data_end = (void *)(long)ctx->data_end;
	data = (void *)(long)ctx->data;

	if (hdr.ipv4) {
		hdr.eth = data;
		hdr.ipv4 = (void *)hdr.eth + sizeof(*hdr.eth);
		// IPV4_MAXLEN is needed when calculating checksum.
		// At least sizeof(struct iphdr) is needed here to access ihl.
		if ((void *)hdr.ipv4 + IPV4_MAXLEN > data_end)
			return XDP_ABORTED;
		hdr.tcp = (void *)hdr.ipv4 + hdr.ipv4->ihl * 4;
	} else if (hdr.ipv6) {
		hdr.eth = data;
		hdr.ipv6 = (void *)hdr.eth + sizeof(*hdr.eth);
		hdr.tcp = (void *)hdr.ipv6 + sizeof(*hdr.ipv6);
	} else {
		return XDP_ABORTED;
	}

	if ((void *)hdr.tcp + TCP_MAXLEN > data_end)
		return XDP_ABORTED;

	// We run out of registers, tcp_len gets spilled to the stack, and the
	// verifier forgets its min and max values checked above in tcp_dissect.
	hdr.tcp_len = hdr.tcp->doff * 4;
	if (hdr.tcp_len < sizeof(*hdr.tcp))
		return XDP_ABORTED;

	return hdr.tcp->syn ? syncookie_handle_syn(&hdr, ctx, data, data_end) :
			      syncookie_handle_ack(&hdr);
}