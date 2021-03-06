/*
 * IPv6 library code, needed by static components when full IPv6 support is
 * not configured or static.  These functions are needed by GSO/GRO implementation.
 */
#include <linux/export.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/ip6_fib.h>


static u32 __ipv6_select_ident(struct net *net,
			       struct in6_addr *dst, struct in6_addr *src)
{
	const struct {
		struct in6_addr dst;
		struct in6_addr src;
	} __aligned(SIPHASH_ALIGNMENT) combined = {
		.dst = *dst,
		.src = *src,
	};
	u32 hash, id;

	/* Note the following code is not safe, but this is okay. */
	if (unlikely(siphash_key_is_zero(&net->ipv4.ip_id_key)))
		get_random_bytes(&net->ipv4.ip_id_key,
				 sizeof(net->ipv4.ip_id_key));

	hash = siphash(&combined, sizeof(combined), &net->ipv4.ip_id_key);

	/* Treat id of 0 as unset and if we get 0 back from ip_idents_reserve,
	 * set the hight order instead thus minimizing possible future
	 * collisions.
	 */
	id = ip_idents_reserve(hash, 1);
	if (unlikely(!id))
		id = 1 << 31;

	return id;
}

/* This function exists only for tap drivers that must support broken
 * clients requesting UFO without specifying an IPv6 fragment ID.
 *
 * This is similar to ipv6_select_ident() but we use an independent hash
 * seed to limit information leakage.
 *
 * The network header must be set before calling this.
 */
void ipv6_proxy_select_ident(struct net *net, struct sk_buff *skb)
{
	struct in6_addr buf[2];
	struct in6_addr *addrs;
	u32 id;

	addrs = skb_header_pointer(skb,
				   skb_network_offset(skb) +
				   offsetof(struct ipv6hdr, saddr),
				   sizeof(buf), buf);
	if (!addrs)
		return;

	id = __ipv6_select_ident(net, &addrs[1], &addrs[0]);
	skb_shinfo(skb)->ip6_frag_id = htonl(id);
}
EXPORT_SYMBOL_GPL(ipv6_proxy_select_ident);

void ipv6_select_ident(struct net *net, struct frag_hdr *fhdr,
		       struct rt6_info *rt)
{
	u32 id;

	id = __ipv6_select_ident(net, &rt->rt6i_dst.addr, &rt->rt6i_src.addr);
	fhdr->identification = htonl(id);
}
EXPORT_SYMBOL(ipv6_select_ident);

int ip6_find_1stfragopt(struct sk_buff *skb, u8 **nexthdr)
{
	unsigned int offset = sizeof(struct ipv6hdr);
	unsigned int packet_len = skb_tail_pointer(skb) -
		skb_network_header(skb);
	int found_rhdr = 0;
	*nexthdr = &ipv6_hdr(skb)->nexthdr;

	while (offset <= packet_len) {
		struct ipv6_opt_hdr *exthdr;
		unsigned int len;

		switch (**nexthdr) {

		case NEXTHDR_HOP:
			break;
		case NEXTHDR_ROUTING:
			found_rhdr = 1;
			break;
		case NEXTHDR_DEST:
#if IS_ENABLED(CONFIG_IPV6_MIP6)
			if (ipv6_find_tlv(skb, offset, IPV6_TLV_HAO) >= 0)
				break;
#endif
			if (found_rhdr)
				return offset;
			break;
		default :
			return offset;
		}

		if (offset + sizeof(struct ipv6_opt_hdr) > packet_len)
			return -EINVAL;

		exthdr = (struct ipv6_opt_hdr *)(skb_network_header(skb) +
						 offset);
		len = ipv6_optlen(exthdr);
		if (len + offset >= IPV6_MAXPLEN)
			return -EINVAL;
		offset += len;
		*nexthdr = &exthdr->nexthdr;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(ip6_find_1stfragopt);

int __ip6_local_out(struct sk_buff *skb)
{
	int len;

	len = skb->len - sizeof(struct ipv6hdr);
	if (len > IPV6_MAXPLEN)
		len = 0;
	ipv6_hdr(skb)->payload_len = htons(len);
	IP6CB(skb)->nhoff = offsetof(struct ipv6hdr, nexthdr);

	skb->protocol = htons(ETH_P_IPV6);

	return nf_hook(NFPROTO_IPV6, NF_INET_LOCAL_OUT, skb, NULL,
		       skb_dst(skb)->dev, dst_output);
}
EXPORT_SYMBOL_GPL(__ip6_local_out);

int ip6_local_out(struct sk_buff *skb)
{
	int err;

	err = __ip6_local_out(skb);
	if (likely(err == 1))
		err = dst_output(skb);

	return err;
}
EXPORT_SYMBOL_GPL(ip6_local_out);
