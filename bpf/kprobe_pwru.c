// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2020-2021 Martynas Pumputis */
/* Copyright (C) 2021 Authors of Cilium */

#include "vmlinux.h"
#include "bpf_helpers.h"
#include "bpf_core_read.h"

#define PT_REGS_PARM1(x) ((x)->di)
#define PT_REGS_PARM2(x) ((x)->si)
#define PT_REGS_PARM3(x) ((x)->dx)
#define PT_REGS_PARM4(x) ((x)->cx)
#define PT_REGS_PARM5(x) ((x)->r8)
#define PT_REGS_RET(x) ((x)->sp)
#define PT_REGS_FP(x) ((x)->bp)
#define PT_REGS_RC(x) ((x)->ax)
#define PT_REGS_SP(x) ((x)->sp)
#define PT_REGS_IP(x) ((x)->ip)

#define PRINT_SKB_STR_SIZE    2048

extern u32 LINUX_KERNEL_VERSION __kconfig;

struct skb_meta {
	u32 mark;
	u32 ifindex;
	u32 len;
	u32 mtu;
	u16 protocol;
	u16 pad;
} __attribute__((packed));

struct tuple {
	u32 saddr;
	u32 daddr;
	u16 sport;
	u16 dport;
	u8 proto;
	u8 pad[7];
} __attribute__((packed));

u64 print_skb_id = 0;

struct event_t {
	u32 pid;
	u32 type;
	u64 addr;
	u64 skb_addr;
	u64 ts;
	typeof(print_skb_id) print_skb_id;
	struct skb_meta meta;
	struct tuple tuple;
	s64 print_stack_id;
} __attribute__((packed));

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
} events SEC(".maps");

union addr {
	u32 v4addr;
	struct {
		u64 d1;
		u64 d2;
	} v6addr;
	u64 pad[2];
} __attribute__((packed));

struct config {
	u32 mark;
	u8 ipv6;
	union addr saddr;
	union addr daddr;
	u8 l4_proto;
	u16 sport;
	u16 dport;
	u8 output_timestamp;
	u8 output_meta;
	u8 output_tuple;
	u8 output_skb;
	u8 output_stack;
	u8 pad;
} __attribute__((packed));

#define MAX_STACK_DEPTH 50
struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(max_entries, 256);
	__uint(key_size, sizeof(u32));
	__uint(value_size, MAX_STACK_DEPTH * sizeof(u64));
} print_stack_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct config);
} cfg_map SEC(".maps");

#ifdef OUTPUT_SKB
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 256);
	__type(key, u32);
	__type(value, char[PRINT_SKB_STR_SIZE]);
} print_skb_map SEC(".maps");
#endif

static __always_inline bool
filter_mark(struct sk_buff *skb, struct config *cfg) {
	u32 mark;

	if (cfg->mark) {
#if (LINUX_KERNEL_VERSION >= KERNEL_VERSION(5, 5, 0))
		mark = BPF_CORE_READ(skb, mark);
#else
		bpf_probe_read(&mark, sizeof(mark), &skb->mark);
#endif
		return mark == cfg->mark;
	}

	return true;
}

/*
 * Filter by packet tuple, return true when the tuple is empty, return false
 * if one of the other fields does not match.
 */
static __always_inline bool
filter_l3_and_l4(struct sk_buff *skb, struct config *cfg) {
	unsigned char *skb_head = 0;
	u16 l3_off, l4_off;
	u16 dport;
	u16 sport;
	u8 iphdr_first_byte;
	u8 ip_vsn;

	if (!cfg->l4_proto && \
        cfg->saddr.pad[0] == 0 && cfg->saddr.pad[1] == 0 && \
        cfg->daddr.pad[0] == 0 && cfg->daddr.pad[1] == 0 && \
        !cfg->sport && !cfg->dport)
		return true;

#if (LINUX_KERNEL_VERSION >= KERNEL_VERSION(5, 5, 0))
	skb_head = BPF_CORE_READ(skb, head);
	l3_off = BPF_CORE_READ(skb, network_header);
	l4_off = BPF_CORE_READ(skb, transport_header);
#else
	bpf_probe_read(&skb_head, sizeof(skb_head), &skb->head);
	bpf_probe_read(&l3_off, sizeof(l3_off), &skb->network_header);
	bpf_probe_read(&l4_off, sizeof(l4_off), &skb->transport_header);
#endif

	struct iphdr *tmp = (struct iphdr *) (skb_head + l3_off);
	bpf_probe_read(&iphdr_first_byte, 1, tmp);
	ip_vsn = iphdr_first_byte >> 4;

	//TODO: support ipv6
	if (ip_vsn != 4) {
		return false;
	}

	struct iphdr ip4;
	bpf_probe_read(&ip4, sizeof(ip4), tmp);

	if (cfg->saddr.v4addr != 0 && ip4.saddr != cfg->saddr.v4addr)
		return false;

	if (cfg->daddr.v4addr != 0 && ip4.daddr != cfg->daddr.v4addr)
		return false;

	if (cfg->l4_proto && ip4.protocol != cfg->l4_proto)
		return false;

	if (cfg->dport || cfg->sport) {
		if (ip4.protocol == IPPROTO_TCP) {
			struct tcphdr *tmp = (struct tcphdr *) (skb_head + l4_off);
			struct tcphdr tcp;

			bpf_probe_read(&tcp, sizeof(tcp), tmp);
			sport = tcp.source;
			dport = tcp.dest;
		} else if (ip4.protocol == IPPROTO_UDP) {
			struct udphdr *tmp = (struct udphdr *) (skb_head + l4_off);
			struct udphdr udp;

			bpf_probe_read(&udp, sizeof(udp), tmp);
			sport = udp.source;
			dport = udp.dest;
		} else {
			return false;
		}

		if (cfg->sport && sport != cfg->sport)
			return false;

		if (cfg->dport && dport != cfg->dport)
			return false;
	}

	return true;
}

static __always_inline bool
filter(struct sk_buff *skb, struct config *cfg) {
	return filter_mark(skb, cfg) && filter_l3_and_l4(skb, cfg);
}

static __always_inline void
set_meta(struct sk_buff *skb, struct skb_meta *meta) {
#if (LINUX_KERNEL_VERSION >= KERNEL_VERSION(5, 5, 0))
	meta->mark = BPF_CORE_READ(skb, mark);
	meta->len = BPF_CORE_READ(skb, len);
	meta->protocol = BPF_CORE_READ(skb, protocol);
	meta->ifindex = BPF_CORE_READ(skb, dev, ifindex);
	meta->mtu = BPF_CORE_READ(skb, dev, mtu);
#else
	struct net_device *dev = 0;

	bpf_probe_read(&meta->mark, sizeof(meta->mark), &skb->mark);
	bpf_probe_read(&meta->len, sizeof(meta->len), &skb->len);
	bpf_probe_read(&meta->protocol, sizeof(meta->protocol), &skb->protocol);

	if (!bpf_probe_read(&dev, sizeof(dev), &skb->dev)) {
		bpf_probe_read(&meta->ifindex, sizeof(dev->ifindex),
			       &dev->ifindex);
		bpf_probe_read(&meta->mtu, sizeof(dev->mtu), &dev->mtu);
	}
#endif
}

static __always_inline void
set_tuple(struct sk_buff *skb, struct tuple *tpl) {
	unsigned char *skb_head = 0;
	u16 l3_off;
	u16 l4_off;
	struct iphdr *ip;
	u8 iphdr_first_byte;
	u8 ip_vsn;

#if (LINUX_KERNEL_VERSION >= KERNEL_VERSION(5, 5, 0))
	skb_head = BPF_CORE_READ(skb, head);
	l3_off = BPF_CORE_READ(skb, network_header);
	l4_off = BPF_CORE_READ(skb, transport_header);
#else
	bpf_probe_read(&skb_head, sizeof(skb_head), &skb->head);
	bpf_probe_read(&l3_off, sizeof(l3_off), &skb->network_header);
	bpf_probe_read(&l4_off, sizeof(l4_off), &skb->transport_header);

#endif
	ip = (struct iphdr *) (skb_head + l3_off);
	bpf_probe_read(&tpl->proto, 1, &ip->protocol);

	bpf_probe_read(&iphdr_first_byte, 1, ip);
	ip_vsn = iphdr_first_byte >> 4;
	if (ip_vsn == 4) {
		bpf_probe_read(&tpl->saddr, sizeof(tpl->saddr), &ip->saddr);
		bpf_probe_read(&tpl->daddr, sizeof(tpl->daddr), &ip->daddr);
		bpf_probe_read(tpl->pad, sizeof(u32), &ip->daddr);
	}

	if (tpl->proto == IPPROTO_TCP) {
		struct tcphdr *tcp = (struct tcphdr *) (skb_head + l4_off);
		bpf_probe_read(&tpl->sport, sizeof(tpl->sport), &tcp->source);
		bpf_probe_read(&tpl->dport, sizeof(tpl->dport), &tcp->dest);
	} else if (tpl->proto == IPPROTO_UDP) {
		struct udphdr *udp = (struct udphdr *) (skb_head + l4_off);
		bpf_probe_read(&tpl->sport, sizeof(tpl->sport), &udp->source);
		bpf_probe_read(&tpl->dport, sizeof(tpl->dport), &udp->dest);
	}
}

static __always_inline void
set_skb_btf(struct sk_buff *skb, typeof(print_skb_id) *event_id) {
#ifdef OUTPUT_SKB
	static struct btf_ptr p = {};
	typeof(print_skb_id) id;
	char *str;

	p.type_id = bpf_core_type_id_kernel(struct sk_buff);
	p.ptr = skb;
	id = __sync_fetch_and_add(&print_skb_id, 1) % 256;

	str = bpf_map_lookup_elem(&print_skb_map, (u32 *) &id);
	if (!str)
		return;

	if (bpf_snprintf_btf(str, PRINT_SKB_STR_SIZE, &p, sizeof(p), 0) < 0)
		return;

	*event_id = id;
#endif
}

static __always_inline void
set_output(struct pt_regs *ctx, struct sk_buff *skb, struct event_t *event, struct config *cfg) {
	if (cfg->output_meta)
		set_meta(skb, &event->meta);

	if (cfg->output_tuple)
		set_tuple(skb, &event->tuple);

	if (cfg->output_skb)
		set_skb_btf(skb, &event->print_skb_id);

	if (cfg->output_stack) {
		event->print_stack_id = bpf_get_stackid(ctx, &print_stack_map, BPF_F_FAST_STACK_CMP);
	}
}

static __always_inline int
handle_everything(struct sk_buff *skb, struct pt_regs *ctx) {
	struct event_t event = {};

	u32 index = 0;
	struct config *cfg = bpf_map_lookup_elem(&cfg_map, &index);

	if (cfg) {
		if (!filter(skb, cfg))
			return 0;

		set_output(ctx, skb, &event, cfg);
	}

	event.pid = bpf_get_current_pid_tgid();
	event.addr = PT_REGS_IP(ctx);
	event.skb_addr = (u64) skb;
	event.ts = bpf_ktime_get_ns();
	bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &event, sizeof(event));

	return 0;
}

SEC("kprobe/skb-1")
int kprobe_skb_1(struct pt_regs *ctx) {
	struct sk_buff *skb = (struct sk_buff *) PT_REGS_PARM1(ctx);

	return handle_everything(skb, ctx);
}

SEC("kprobe/skb-2")
int kprobe_skb_2(struct pt_regs *ctx) {
	struct sk_buff *skb = (struct sk_buff *) PT_REGS_PARM2(ctx);

	return handle_everything(skb, ctx);
}

SEC("kprobe/skb-3")
int kprobe_skb_3(struct pt_regs *ctx) {
	struct sk_buff *skb = (struct sk_buff *) PT_REGS_PARM3(ctx);

	return handle_everything(skb, ctx);
}

SEC("kprobe/skb-4")
int kprobe_skb_4(struct pt_regs *ctx) {
	struct sk_buff *skb = (struct sk_buff *) PT_REGS_PARM4(ctx);

	return handle_everything(skb, ctx);
}

SEC("kprobe/skb-5")
int kprobe_skb_5(struct pt_regs *ctx) {
	struct sk_buff *skb = (struct sk_buff *) PT_REGS_PARM5(ctx);

	return handle_everything(skb, ctx);
}

char __license[] SEC("license") = "GPL";
