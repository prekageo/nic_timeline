#pragma once

#define PORT 5555
#define MY_ETHER_TYPE ETHER_TYPE_1588 /* Precise Time Protocol */
#define PKT_SIZE 64

extern struct rte_mempool *mbuf_pool;

void dpdk_init(int core);

extern struct ether_addr my_mac, remote_mac;
extern double cycles_per_ns;
extern int agent_fd;

struct agent_cmd {
	int id;
	int count;
};

enum agent_cmd_ids {
	AGENT_CMD_INVALID,
	AGENT_CMD_NOP,
	AGENT_CMD_ECHO,
	AGENT_CMD_TX_1PKT,
	AGENT_CMD_TX_2PKTS,
};

static inline void init_ether_hdr(struct ether_hdr *ether_hdr)
{
	memcpy(&ether_hdr->s_addr, &my_mac, sizeof(my_mac));
	memcpy(&ether_hdr->d_addr, &remote_mac, sizeof(my_mac));
	// memset(&ether_hdr->d_addr, 0xff, 6);
	ether_hdr->ether_type = __builtin_bswap16(MY_ETHER_TYPE);
}

static inline struct rte_mbuf *rx_pkt(void)
{
	struct rte_mbuf *mbufs[1];
	struct ether_hdr *ether_hdr;

	while (1) {
		// printf("waiting for pkt...\n");

		while (!rte_eth_rx_burst(0, 0, mbufs, 1))
			;

		// printf("pkt rcvd\n");
		ether_hdr = rte_pktmbuf_mtod(mbufs[0], struct ether_hdr *);
		if (ether_hdr->ether_type == __builtin_bswap16(MY_ETHER_TYPE)) {
			// remote_mac = ether_hdr->s_addr;
			return mbufs[0];
		}

		rte_pktmbuf_free(mbufs[0]);
	}
}

static inline void tx_pkt(void)
{
	struct rte_mbuf *mbufs[1];
	struct ether_hdr *ether_hdr;

	mbufs[0] = rte_pktmbuf_alloc(mbuf_pool);
	assert(mbufs[0]);
	mbufs[0]->pkt_len = mbufs[0]->data_len = PKT_SIZE;
	ether_hdr = rte_pktmbuf_mtod(mbufs[0], struct ether_hdr *);
	init_ether_hdr(ether_hdr);
	short *ptp_data = (short *)(ether_hdr + 1);
	*ptp_data = 0x0202;

        int count = rte_eth_tx_burst(0, 0, mbufs, 1);
        assert(count == 1);
}

static inline unsigned long rdtsc(void)
{
	unsigned long hi, lo;
	asm volatile("mfence; rdtsc; mfence" : "=a"(lo), "=d"(hi)::"memory");
	// asm volatile("rdtsc" : "=a"(lo), "=d"(hi)::"memory");
	return lo|(hi<<32);
}

static inline void delay_ns(long ns)
{
	long stop = rdtsc() + ns * cycles_per_ns;
	while (rdtsc() < stop)
		;
}

static inline void delay_us(long us)
{
	delay_ns(us * 1000);
}

static inline void delay_ms(long ms)
{
	delay_us(ms * 1000);
}

static inline void agent_tx_rx_mac(void)
{
	ssize_t ret;

	rte_eth_macaddr_get(0, &my_mac);
	ret = write(agent_fd, &my_mac, sizeof(my_mac));
	assert(ret == sizeof(my_mac));
	ret = read(agent_fd, &remote_mac, sizeof(remote_mac));
	assert(ret == sizeof(remote_mac));
}
