/*
 * ca_core.c
 * Copyright (C) 2016 yubo@yubo.org
 * 2016-02-14
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <asm/paravirt.h>
#include "ca.h"


unsigned long **sys_call_table;
unsigned long original_cr0;
struct syscall_links sys;

static int
ca_use_count_inc(void)
{
	return try_module_get(THIS_MODULE);
}

static void
ca_use_count_dec(void)
{
	module_put(THIS_MODULE);
}


static void
ip_vs_ca_modify_uaddr(int fd, struct sockaddr *uaddr, int len, int dir)
{
	int err, ret = 0;
	struct socket *sock = NULL;
	struct sockaddr_in sin;
	union nf_inet_addr addr;
	struct ip_vs_ca_conn *cp;

	if (len != sizeof(struct sockaddr_in)){
		ret = -1;
		goto out;
	}

	err = copy_from_user(&sin, uaddr, len);
	if (err){
		ret = -2;
		goto out;
	}

	if (sin.sin_family != AF_INET){
		ret = -3;
		goto out;
	}

	sock = sockfd_lookup(fd, &err);
	if (!sock){
		ret = -4;
		goto out;
	}

	IP_VS_CA_DBG("%s called, sin{.family:%d, .port:%d, addr:%pI4} sock.type:%d\n",
			__func__, sin.sin_family, ntohs(sin.sin_port),
			&sin.sin_addr.s_addr, sock->type);

	addr.ip = sin.sin_addr.s_addr;

	if (sock->type == SOCK_STREAM){
		cp = ip_vs_ca_conn_get(sin.sin_family, IPPROTO_TCP, &addr,
				sin.sin_port, dir);
	}else if(sock->type == SOCK_DGRAM){
		cp = ip_vs_ca_conn_get(sin.sin_family, IPPROTO_UDP, &addr,
				sin.sin_port, dir);
	}else{
		ret = -5;
		goto out;
	}

	IP_VS_CA_DBG("lookup type:%d %pI4:%d %s\n",
				sock->type,
				&addr.ip, ntohs(sin.sin_port),
				cp ? "hit" : "not hit");

	if (!cp){
		ret = -6;
		goto out;
	}

	IP_VS_CA_DBG("%s called, %d %pI4:%d(%pI4:%d)->%pI4:%d\n",
			__func__, cp->protocol,
			&sin.sin_addr.s_addr, ntohs(sin.sin_port),
			&cp->c_addr.ip, ntohs(cp->c_port),
			&cp->d_addr.ip, ntohs(cp->d_port));

	if (dir == IP_VS_CA_IN) {
		sin.sin_addr.s_addr = cp->c_addr.ip;
		sin.sin_port = cp->c_port;
	} else {
		sin.sin_addr.s_addr = cp->s_addr.ip;
		sin.sin_port = cp->s_port;
	}
	ip_vs_ca_conn_put(cp);
	if(copy_to_user(uaddr, &sin, len)) {
		ret = -7;
		goto out;
	}

out:
	if (sock && sock->file)
		sockfd_put(sock);

	IP_VS_CA_DBG("ip_vs_ca_modify_uaddr err:%d\n", ret);

	return;
}

/*
 * ./net/socket.c:1624
 */
asmlinkage static long
getpeername(int fd, struct sockaddr __user *usockaddr, int __user *usockaddr_len)
{
	int ret, len;

	if (!ca_use_count_inc())
		return -1;
	IP_VS_CA_DBG("getpeername called\n");

	ret = sys.getpeername(fd, usockaddr, usockaddr_len);
	if (ret < 0)
		goto out;

	get_user(len, usockaddr_len);
	ip_vs_ca_modify_uaddr(fd, usockaddr, len, IP_VS_CA_IN);

out:
	ca_use_count_dec();
	return ret;
}

asmlinkage static long
accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		int __user *upeer_addrlen, int flags)
{
	int ret, len;

	if (!ca_use_count_inc())
		return -1;
	IP_VS_CA_DBG("accept4 called\n");

	ret = sys.accept4(fd, upeer_sockaddr, upeer_addrlen, flags);
	if (ret < 0){
		IP_VS_CA_DBG("accept4 (%d, %p, %d, %d) ret:%d\n", fd, upeer_sockaddr, *upeer_addrlen, flags, ret);
		goto out;
	}

	get_user(len, upeer_addrlen);
	ip_vs_ca_modify_uaddr(fd, upeer_sockaddr, len, IP_VS_CA_IN);

out:
	ca_use_count_dec();
	return ret;
}

asmlinkage static long
accept(int fd, struct sockaddr __user *upeer_sockaddr, int __user *upeer_addrlen)
{
	return accept4(fd, upeer_sockaddr, upeer_addrlen, 0);
}

asmlinkage static long
recvfrom(int fd, void __user *ubuf, size_t size, unsigned flags,
				struct sockaddr __user *addr, int __user *addr_len)
{
	int ret, len;

	if (!ca_use_count_inc())
		return -1;

	if(addr == NULL || addr_len == NULL){
		ret =  sys.recvfrom(fd, ubuf, size, flags, addr, addr_len);
		goto out;
	}


	ret = sys.recvfrom(fd, ubuf, size, flags, addr, addr_len);
	if (ret < 0)
		goto out;

	get_user(len, addr_len);
	ip_vs_ca_modify_uaddr(fd, addr, len, IP_VS_CA_IN);

out:
	ca_use_count_dec();
	return ret;
}

asmlinkage static long
connect(int fd, struct sockaddr __user *uservaddr, int addrlen)
{
	int ret;

	if (!ca_use_count_inc())
		return -1;

	ip_vs_ca_modify_uaddr(fd, uservaddr, addrlen, IP_VS_CA_OUT);
	ret = sys.connect(fd, uservaddr, addrlen);

	ca_use_count_dec();
	return ret;
}

asmlinkage static long
sendto(int fd, void __user *buff, size_t len, unsigned int flags,
			struct sockaddr __user *addr, int addr_len)
{
	int ret;

	if (!ca_use_count_inc())
		return -1;

	ip_vs_ca_modify_uaddr(fd, addr, addr_len, IP_VS_CA_OUT);
	ret = sys.sendto(fd, buff, len, flags, addr, addr_len);

	ca_use_count_dec();
	return ret;
}

const char *ip_vs_ca_proto_name(unsigned proto)
{
	static char buf[20];

	switch (proto) {
		case IPPROTO_IP:
			return "IP";
		case IPPROTO_UDP:
			return "UDP";
		case IPPROTO_TCP:
			return "TCP";
		case IPPROTO_ICMP:
			return "ICMP";
#ifdef CONFIG_IP_VS_IPV6
		case IPPROTO_ICMPV6:
			return "ICMPv6";
#endif
		default:
			sprintf(buf, "IP_%d", proto);
			return buf;
	}
}

static int ip_vs_ca_syscall_init(void)
{
	if (!(sys_call_table = find_sys_call_table())){
		IP_VS_CA_ERR("get sys call table failed.\n");
		return -1;
	}

	original_cr0 = read_cr0();
	write_cr0(original_cr0 & ~0x00010000);
	IP_VS_CA_DBG("Loading ip_vs_ca module, sys call table at %p\n", sys_call_table);

	sys.getpeername = (void *)(sys_call_table[__NR_getpeername]);
	sys.accept4	= (void *)(sys_call_table[__NR_accept4]);
	sys.recvfrom	= (void *)(sys_call_table[__NR_recvfrom]);
	sys.connect	= (void *)(sys_call_table[__NR_connect]);
	sys.accept	= (void *)(sys_call_table[__NR_accept]);
	sys.sendto	= (void *)(sys_call_table[__NR_sendto]);

	sys_call_table[__NR_getpeername]= (void *)getpeername;
	sys_call_table[__NR_accept4]	= (void *)accept4;
	sys_call_table[__NR_recvfrom]	= (void *)recvfrom;
	sys_call_table[__NR_connect]	= (void *)connect;
	sys_call_table[__NR_accept]	= (void *)accept;
	sys_call_table[__NR_sendto]	= (void *)sendto;

	write_cr0(original_cr0);

	return 0;
}

static void ip_vs_ca_syscall_cleanup(void)
{	
	if (!sys_call_table){
		return;
	}

	write_cr0(original_cr0 & ~0x00010000);

	sys_call_table[__NR_getpeername] = (void *)sys.getpeername;
	sys_call_table[__NR_accept4]     = (void *)sys.accept4;
	sys_call_table[__NR_recvfrom]    = (void *)sys.recvfrom;
	sys_call_table[__NR_connect]     = (void *)sys.connect;
	sys_call_table[__NR_accept]      = (void *)sys.accept;
	sys_call_table[__NR_sendto]      = (void *)sys.sendto;

	write_cr0(original_cr0);
	//msleep(100);
	sys_call_table = NULL;
}

static unsigned int _ip_vs_ca_in_hook(struct sk_buff *skb);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
static unsigned int
ip_vs_ca_in_hook(void *priv, struct sk_buff *skb,
		      const struct nf_hook_state *state)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
static unsigned int
ip_vs_ca_in_hook(const struct nf_hook_ops *ops, struct sk_buff *skb,
			const struct net_device *in,
			const struct net_device *out,
			const void *ignore)
#else
ip_vs_ca_in_hook(unsigned int hooknum, struct sk_buff *skb,
		const struct net_device *in, const struct net_device *out,
		int (*okfn) (struct sk_buff *))
#endif
{
	return _ip_vs_ca_in_hook(skb);
}

static unsigned int _ip_vs_ca_in_hook(struct sk_buff *skb)
{
	struct ip_vs_ca_iphdr iph;
	struct ip_vs_ca_conn *cp;
	struct ip_vs_ca_protocol *pp;
	int af;

	//EnterFunction();

	af = (skb->protocol == htons(ETH_P_IP)) ? AF_INET : AF_INET6;

	if (af != AF_INET) {
		goto out;
	}

	ip_vs_ca_fill_iphdr(af, skb_network_header(skb), &iph);


	/*
	 *      Big tappo: only PACKET_HOST, including loopback for local client
	 *      Don't handle local packets on IPv6 for now
	 */
	if (unlikely(skb->pkt_type != PACKET_HOST)) {
		/*
		IP_VS_CA_DBG("packet type=%d proto=%d daddr=%pI4 ignored\n",
				skb->pkt_type,
				iph.protocol, &iph.daddr.ip);
		*/
		goto out;
	}

	if (iph.protocol == IPPROTO_ICMP) {
#ifndef IP_VS_CA_ICMP
		return NF_ACCEPT;
#else
		struct iphdr *ih;
		struct icmphdr _icmph, *icmph;
		struct ipvs_ca _ca, *ca;

		IP_VS_CA_DBG("icmp packet recv\n");

		ih = (struct iphdr *)skb_network_header(skb);

		icmph = skb_header_pointer(skb, iph.len,
				sizeof(_icmph), &_icmph);

		if (icmph == NULL){
			IP_VS_CA_DBG("icmphdr NULL\n");
			goto out;
		}

		if(ntohs(ih->tot_len) == sizeof(*ih)+sizeof(*icmph)+sizeof(*ca)
				&& icmph->type == ICMP_ECHO 
				&& icmph->code == 0 
				&& icmph->un.echo.id == 0x1234
				&& icmph->un.echo.sequence == 0){
			ca = skb_header_pointer(skb, iph.len + sizeof(*icmph),
				sizeof(_ca), &_ca);

			if (ca == NULL){
				IP_VS_CA_DBG("ca NULL\n");
				goto out;
			}

			if(ca->code != 123
					|| ca->toa.opcode != tcpopt_addr
					|| ca->toa.opsize != TCPOLEN_ADDR){
				IP_VS_CA_DBG("ca not hit. {.code:%d, .protocol:%d,"
						" .toa.opcode:%d, .toa.opsize:%d}\n",
						ca->code, ca->protocol, ca->toa.opcode, ca->toa.opsize);
				goto out;
			}

			pp = ip_vs_ca_proto_get(ca->protocol);
			if (unlikely(!pp))
				goto out;

			cp = pp->conn_get(af, skb, pp, &iph, iph.len);
			if(unlikely(cp)){
				ip_vs_ca_conn_put(cp);
				goto out;
			}else{
				int v;
				if(pp->icmp_process(af, skb, pp, &iph, icmph, ca,
							&v, &cp) == 0){
					return v;
				}else{
					goto out;
				}
			}
		}else{
			IP_VS_CA_DBG("icmphdr not hit tot_len:%d, "
					"icmp{.type:%d, .code:%d .echo.id:0x%04x,"
					" .echo.sequence:%d}\n"
					"want tot_len:%lu icmp.type:%d\n", 
					ntohs(ih->tot_len), icmph->type,
					icmph->code, icmph->un.echo.id,
					icmph->un.echo.sequence,
					sizeof(*ih)+sizeof(*icmph)+sizeof(*ca),
					ICMP_ECHO);
			goto out;
		}
#endif
	}else if (iph.protocol == IPPROTO_TCP) {
		/* Protocol supported? */
		pp = ip_vs_ca_proto_get(iph.protocol);
		if (unlikely(!pp))
			goto out;

		/*
		 * Check if the packet belongs to an existing connection entry
		 */
		cp = pp->conn_get(af, skb, pp, &iph, iph.len);

		if (likely(cp)) {
			ip_vs_ca_conn_put(cp);
			goto out;
		} else {
			int v;
			/* create a new connection */
			if(pp->skb_process(af, skb, pp, &iph, &v, &cp) == 0){
				//LeaveFunction();
				return v;
			}else{
				goto out;
			}
		}
	}

out:
	//LeaveFunction();
	return NF_ACCEPT;
}

static struct nf_hook_ops ip_vs_ca_ops[] __read_mostly = { 
	{
		.hook     = (nf_hookfn *)ip_vs_ca_in_hook,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0)
		.owner    = THIS_MODULE,
#endif
		.pf       = NFPROTO_IPV4,
		.hooknum  = NF_INET_LOCAL_IN,
		.priority = NF_IP_PRI_CONNTRACK_CONFIRM,
	},
};

static int __init ip_vs_ca_init(void)
{
	int ret;

	ret = ip_vs_ca_syscall_init();
	if (ret < 0){
		IP_VS_CA_ERR("can't modify syscall table.\n");
		goto out_err;
	}
	IP_VS_CA_DBG("modify syscall table done.\n");

	ip_vs_ca_protocol_init();
	IP_VS_CA_DBG("ip_vs_ca_protocol_init done.\n");

	ret = ip_vs_ca_control_init();
	if (ret < 0){
		IP_VS_CA_ERR("can't modify syscall table.\n");
		goto cleanup_syscall;
	}
	IP_VS_CA_DBG("ip_vs_ca_control_init done.\n");

	ret = ip_vs_ca_conn_init();
	if (ret < 0){
		IP_VS_CA_ERR("can't setup connection table.\n");
		goto cleanup_control;
	}
	IP_VS_CA_DBG("ip_vs_ca_conn_init done.\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0) 
	ret = nf_register_net_hooks(NULL, ip_vs_ca_ops, ARRAY_SIZE(ip_vs_ca_ops));
#else
	ret = nf_register_hooks(ip_vs_ca_ops, ARRAY_SIZE(ip_vs_ca_ops));
#endif
	if (ret < 0){
		IP_VS_CA_ERR("can't register hooks.\n");
		goto cleanup_conn;
	}
	IP_VS_CA_DBG("nf_register_hooks done.\n");

	IP_VS_CA_INFO("ip_vs_ca loaded.");
	return ret;

cleanup_conn:
	ip_vs_ca_conn_cleanup();
cleanup_control:
	ip_vs_ca_control_cleanup();
cleanup_syscall:
	ip_vs_ca_syscall_cleanup();
out_err:
	return ret;
}

static void __exit ip_vs_ca_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0) 
	nf_unregister_net_hooks(NULL, ip_vs_ca_ops, ARRAY_SIZE(ip_vs_ca_ops));
#else
	nf_unregister_hooks(ip_vs_ca_ops, ARRAY_SIZE(ip_vs_ca_ops));
#endif
	ip_vs_ca_conn_cleanup();
	ip_vs_ca_protocol_cleanup();
	ip_vs_ca_control_cleanup();
	ip_vs_ca_syscall_cleanup();
	IP_VS_CA_INFO("ip_vs_ca unloaded.");
}

module_init(ip_vs_ca_init);
module_exit(ip_vs_ca_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yu Bo<yubo@yubo.org>");

