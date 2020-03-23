#include <assert.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_timer.h>

#include "common.h"

#define RX_RING_SIZE 64
#define TX_RING_SIZE 64
#define NUM_MBUFS 256
#define MBUF_CACHE_SIZE 128

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.hw_ip_checksum = 1,
		.max_rx_pkt_len = ETHER_MAX_LEN
	},
	.intr_conf = {
		.rxq = 1,
	},
};

struct rte_mempool *mbuf_pool;

static void port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	int ret;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf tx_conf;
	eth_rx_burst_t non_vec_rx;

	assert(port < rte_eth_dev_count());
	assert(rte_eth_dev_socket_id(port) == rte_socket_id());

	rte_eth_dev_info_get(port, &dev_info);
	tx_conf = dev_info.default_txconf;
	tx_conf.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS;

	ret = rte_eth_dev_configure(port, 1, 1, &port_conf_default);
	assert(ret == 0);

	ret = rte_eth_rx_queue_setup(port, 0, RX_RING_SIZE, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
	assert(ret >= 0);

	ret = rte_eth_tx_queue_setup(port, 0, TX_RING_SIZE, rte_eth_dev_socket_id(port), &tx_conf);
	assert(ret >= 0);

	non_vec_rx = rte_eth_devices[port].rx_pkt_burst;

	ret = rte_eth_dev_start(port);
	assert(ret >= 0);

	rte_eth_devices[port].rx_pkt_burst = non_vec_rx;

	rte_eth_promiscuous_disable(port);

	// struct rte_eth_link link;
	// printf("Getting link status...\n");
	// rte_eth_link_get(port, &link);
	// printf("Link status: %s\n", (link.link_status) ? ("up") : ("down"));
	// printf("Link speed: %u Mbps\n", (unsigned) link.link_speed);
	// printf("Link duplex: %s\n", (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex") : ("half-duplex"));
}

void dpdk_init(int core)
{
	int ret;
	char corestr[8];
	char *argv[] = { "./a.out", "-l", corestr, "--log-level", "4" };

	sprintf(corestr, "%d", core);

	ret = rte_eal_init(sizeof(argv) / sizeof(argv[0]), argv);
	assert(ret >= 0);

	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	assert(mbuf_pool);

	port_init(0, mbuf_pool);
}
