#include <assert.h>
#include <unistd.h>

#include "bench.h"
#include "common.h"

#define REPEAT 1000

static void print_results(const char *msg, long *measurements, int count)
{
	puts(msg);
	for (int i = 0; i < count; i++)
		printf("%f ", measurements[i] / cycles_per_ns);
	puts("");
	qsort(measurements, count, sizeof(measurements[0]), cmp_measurement);
	fprintf(stderr, "%10.1f ns %s\n", measurements[count/2] / cycles_per_ns, msg);
}

// TODO: is this useful?
static long bench_pci_write(void)
{
	long start, stop;

	init_ether_hdr(&pkts1[0].ether_hdr);
	__my_tx(pmem, PKT_SIZE, 0);
	// TODO: fences?
	start = rdtsc();
#if IXGBE
	IXGBE_PCI_REG_WRITE(txq->tdt_reg_addr, txq->tx_tail);
#elif I40E
	I40E_PCI_REG_WRITE(txq->qtx_tail, txq->tx_tail);
#endif
	stop = rdtsc();

	agent_rx_data_ignore();
	return stop - start;
}

static long bench_nic_reads_txd(void)
{
	long start;
#if IXGBE
	volatile union ixgbe_adv_tx_desc *txdp;
#elif I40E
	volatile struct i40e_tx_desc *txdp;
#endif

	for (int i = 0; i < MAX_PKT; i++)
		init_ether_hdr(&pkts1[i].ether_hdr);

	start = rdtsc();
	txdp = my_tx(pmem, PKT_SIZE, 0);
	for (int i = 0; i < MAX_PKT; i++) {
		pkts1[i].timestamp = rdtsc();
#if IXGBE
		txdp->read.buffer_addr = pmem + i * PKT_SIZE;
#elif I40E
		txdp->buffer_addr = pmem + i * PKT_SIZE;
#endif
	}

	struct pkt1 rcv_pkt;
	agent_rx_data(&rcv_pkt, PKT_SIZE);
	for (int j = 0; j < MAX_PKT; j++)
		if (pkts1[j].timestamp == rcv_pkt.timestamp)
			return rcv_pkt.timestamp - start;
	assert(0);
}

static long bench_nic_reads_payload(void)
{
	long start;

	init_ether_hdr(&pkts1[0].ether_hdr);

	start = rdtsc();
	pkts1[0].timestamp = start;
	my_tx(pmem, PKT_SIZE, 0);
	for (int i = 1; i < 9999; i++)
		pkts1[0].timestamp = rdtsc();
	pkts1[0].timestamp = -1l;

	struct pkt1 rcv_pkt;
	agent_rx_data(&rcv_pkt, PKT_SIZE);
	return rcv_pkt.timestamp - start;
}

static long bench_nic_updates_rxd(void)
{
	long start, stop;
#if IXGBE
	volatile union ixgbe_adv_tx_desc *txdp;
#elif I40E
	volatile struct i40e_tx_desc *txdp;
#endif

	init_ether_hdr(&pkts1[0].ether_hdr);
	start = rdtsc();
#if IXGBE
	txdp = my_tx(pmem, PKT_SIZE, IXGBE_ADVTXD_DCMD_RS);
#elif I40E
	txdp = my_tx(pmem, PKT_SIZE, I40E_TX_DESC_CMD_RS);
#endif
	while (1) {
		// TODO: one of those are needed rdtsc(); cpu_relax();
#if IXGBE
		if (txdp->wb.status & 1)
#elif I40E
		/* TODO: why not 0x1f? pg 1029 */
		if (txdp->cmd_type_offset_bsz == 0xf)
#endif
			break;
	}
	stop = rdtsc();

	agent_rx_data_ignore();
	return stop - start;
}

static long bench_nic_updates_tx_head_reg(void)
{
	long start, stop;
	int tdh, prv_val;

#if IXGBE
	tdh = IXGBE_TDH(txq->reg_idx);
	prv_val = IXGBE_READ_REG(hw, tdh);
#elif I40E
	tdh = I40E_QTX_HEAD(txq->reg_idx);
	prv_val = I40E_READ_REG(hw, tdh);
#endif

	init_ether_hdr(&pkts1[0].ether_hdr);
	start = rdtsc();
	my_tx(pmem, PKT_SIZE, 0);
	while (1) {
		// TODO: one of those are needed rdtsc(); cpu_relax();
#if IXGBE
		if (IXGBE_READ_REG(hw, tdh) != prv_val)
#elif I40E
		if (I40E_READ_REG(hw, tdh) != prv_val)
#endif
			break;
	}
	stop = rdtsc();

	agent_rx_data_ignore();
	return stop - start;
}

static long bench_nic_tx_timestamp(void)
{
	long start, stop, offset_start, offset_stop;

#if IXGBE
	IXGBE_WRITE_REG(hw, IXGBE_TIMINCA, (2 << 24) | 1);
	IXGBE_WRITE_REG(hw, IXGBE_TSYNCTXCTL, IXGBE_TSYNCTXCTL_ENABLED);
#elif I40E
#define I40E_PRTTSYN_TSYNENA     0x80000000
#define I40E_PRTTSYN_TSYNTYPE    0x0e000000
#define I40E_PTP_40GB_INCVAL     0x0199999999ULL
	I40E_READ_REG(hw, I40E_PRTTSYN_STAT_0);
	I40E_READ_REG(hw, I40E_PRTTSYN_TXTIME_H);
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(0));
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(1));
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(2));
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(3));
	I40E_WRITE_REG(hw, I40E_PRTTSYN_INC_L, I40E_PTP_40GB_INCVAL & 0xFFFFFFFF);
	I40E_WRITE_REG(hw, I40E_PRTTSYN_INC_H, I40E_PTP_40GB_INCVAL >> 32);
	I40E_WRITE_REG(hw, I40E_PRTTSYN_CTL0, I40E_READ_REG(hw, I40E_PRTTSYN_CTL0) | I40E_PRTTSYN_TSYNENA);
	I40E_WRITE_REG(hw, I40E_PRTTSYN_CTL1, I40E_READ_REG(hw, I40E_PRTTSYN_CTL1) | I40E_PRTTSYN_TSYNENA | I40E_PRTTSYN_TSYNTYPE);
#endif
	init_ether_hdr(&pkts1[0].ether_hdr);
	offset_start = rdtsc();

#if IXGBE
	/* TODO: can i synchronize rdtsc with the nic timer? */
	start = (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIML);
	start |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIMH) << 32;
#elif I40E
	start = (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_TIME_L);
	start |= (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_TIME_H) << 32;
#endif
	offset_stop = rdtsc();
#if IXGBE
	my_tx(pmem, PKT_SIZE, IXGBE_ADVTXD_MAC_TSTAMP);
#elif I40E
	/* TODO: FIXME 42 */
	my_tx(pmem, PKT_SIZE, 42);
#endif

	struct pkt1 rcv_pkt;

	agent_rx_data(&rcv_pkt, PKT_SIZE);
#if IXGBE
	stop = (uint64_t)IXGBE_READ_REG(hw, IXGBE_TXSTMPL);
	stop |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_TXSTMPH) << 32;
#elif I40E
	stop = (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_TXTIME_L);
	stop |= (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_TXTIME_H) << 32;
#endif
	// TODO: don't multiply cycles_per_ns
#if IXGBE
	return 1.0 * (stop - start) * 2 / 156.25e-3 * cycles_per_ns - (offset_stop - offset_start);
#elif I40E
	// fprintf(stderr, "%ld %ld %ld %ld\n", start, stop, offset_start, offset_stop);
	return 1.0 * (stop - start) * cycles_per_ns - (offset_stop - offset_start);
#endif
}

static long bench_nic_rx_writes_payload(void)
{
	long start = 0, stop = 0;
	volatile struct pkt_rx *pkt, *expected = &pkts_rx[rxq->rx_tail];

	bzero((void *) pkts_rx, SIZE);
	init_rxq();

	tx_pkt();
	/* TODO: happens almost at the same time. maybe we should use 2 threads here. */
	while (!expected->ether_hdr.ether_type)
		;
	start = rdtsc();
	do {
		pkt = my_rx(NULL);
	} while (!pkt);
	stop = rdtsc();
	my_rx_complete();
	assert(pkt == expected);
	assert(expected->ether_hdr.ether_type == __builtin_bswap16(MY_ETHER_TYPE));

	return stop - start;
}

static long bench_nic_rx_updates_rx_head_reg(void)
{
#if IXGBE

	long start, stop;
	volatile struct pkt_rx *pkt;
	int rdh, prv_val;

	rdh = IXGBE_RDH(rxq->reg_idx);
	prv_val = IXGBE_READ_REG(hw, rdh);

	bzero((void *) pkts_rx, SIZE);
	init_rxq();

	tx_pkt();
	/* TODO: happens almost at the same time. maybe we should use 2 threads here. */
	while (IXGBE_READ_REG(hw, rdh) == prv_val)
		// TODO: one of those are needed rdtsc(); cpu_relax();
		;
	start = rdtsc();
	do {
		pkt = my_rx(NULL);
	} while (!pkt);
	stop = rdtsc();
	my_rx_complete();
	assert(pkt->ether_hdr.ether_type == __builtin_bswap16(MY_ETHER_TYPE));

	return stop - start;

#elif I40E

	/* TODO: not supported */
	return 0;

#endif
}

static long bench_nic_rx_reads_rxd(void)
{
// TODO: check nested #if
#if IXGBE
	int i;
	long stop;
#if IXGBE
	volatile union ixgbe_adv_rx_desc *rxdp = &rxq->rx_ring[rxq->rx_tail];
#elif I40E
	volatile union i40e_rx_desc *rxdp = &rxq->rx_ring[rxq->rx_tail];
	uint64_t qword1;
	uint32_t rx_status;
#endif

	bzero((void *) pkts_rx, SIZE);
	init_rxq();

	tx_pkt();
	for (i = 0; i < SIZE / 2048; i++) {
		pkts_rx[i].timestamp = rdtsc();
		rxdp->read.pkt_addr = pmem + i * 2048;
#if IXGBE
		if (rxdp->wb.upper.status_error & IXGBE_RXDADV_STAT_DD)
#elif I40E
		qword1 = rxdp->wb.qword1.status_error_len;
		rx_status = (qword1 & I40E_RXD_QW1_STATUS_MASK) >> I40E_RXD_QW1_STATUS_SHIFT;
		if (rx_status & (1 << I40E_RX_DESC_STATUS_DD_SHIFT))
#endif
			break;
	}
	stop = rdtsc();
	assert(i < SIZE / 2048);
	my_rx(NULL);
	my_rx_complete();
	for (i = 0; i < (SIZE / 2048); i++)
		if (pkts_rx[i].ether_hdr.ether_type == __builtin_bswap16(MY_ETHER_TYPE))
			return stop - pkts_rx[i].timestamp;
	assert(0);
#elif I40E
	return 0;
#endif
}

static long bench_nic_rx_timestamp(void)
{
	long start, stop;
	volatile struct pkt_rx *pkt;
	uint64_t qword1;

	bzero((void *) pkts_rx, SIZE);
	init_rxq();

#if IXGBE
	IXGBE_WRITE_REG(hw, IXGBE_TIMINCA, (2 << 24) | 1);
	IXGBE_WRITE_REG(hw, IXGBE_ETQF(IXGBE_ETQF_FILTER_1588), (ETHER_TYPE_1588 | IXGBE_ETQF_FILTER_EN | IXGBE_ETQF_1588));
	IXGBE_WRITE_REG(hw, IXGBE_TSYNCRXCTL, IXGBE_TSYNCRXCTL_ENABLED);
	IXGBE_WRITE_REG(hw, IXGBE_RXMTRL, 2 << 8);
#elif I40E
#define I40E_PRTTSYN_TSYNENA     0x80000000
#define I40E_PRTTSYN_TSYNTYPE    0x0e000000
#define I40E_PTP_40GB_INCVAL     0x0199999999ULL
	I40E_READ_REG(hw, I40E_PRTTSYN_STAT_0);
	I40E_READ_REG(hw, I40E_PRTTSYN_TXTIME_H);
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(0));
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(1));
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(2));
	I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(3));
	I40E_WRITE_REG(hw, I40E_PRTTSYN_INC_L, I40E_PTP_40GB_INCVAL & 0xFFFFFFFF);
	I40E_WRITE_REG(hw, I40E_PRTTSYN_INC_H, I40E_PTP_40GB_INCVAL >> 32);
	I40E_WRITE_REG(hw, I40E_PRTTSYN_CTL0, I40E_READ_REG(hw, I40E_PRTTSYN_CTL0) | I40E_PRTTSYN_TSYNENA);
	I40E_WRITE_REG(hw, I40E_PRTTSYN_CTL1, I40E_READ_REG(hw, I40E_PRTTSYN_CTL1) | I40E_PRTTSYN_TSYNENA | I40E_PRTTSYN_TSYNTYPE);
#endif

	tx_pkt();

	do {
		pkt = my_rx(&qword1);
	} while (!pkt);
#if IXGBE
	stop = (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIML);
	stop |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIMH) << 32;
#elif I40E
	stop = (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_TIME_L);
	stop |= (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_TIME_H) << 32;
#endif
	my_rx_complete();
	assert(pkt->ether_hdr.ether_type == __builtin_bswap16(MY_ETHER_TYPE));

#if IXGBE
	start = (uint64_t)IXGBE_READ_REG(hw, IXGBE_RXSTMPL);
	start |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_RXSTMPH) << 32;
#elif I40E
	uint16_t tsyn = (qword1 & (I40E_RXD_QW1_STATUS_TSYNVALID_MASK | I40E_RXD_QW1_STATUS_TSYNINDX_MASK)) >> I40E_RX_DESC_STATUS_TSYNINDX_SHIFT;
	assert(tsyn & 0x04);
	int index = tsyn & 0x3;
	start = (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_L(index));
	start |= (uint64_t)I40E_READ_REG(hw, I40E_PRTTSYN_RXTIME_H(index)) << 32;
#endif
	// TODO: don't multiply
#if IXGBE
	return 1.0 * (stop - start) * 2 / 156.25e-3 * cycles_per_ns;
#elif I40E
	return 1.0 * (stop - start) * cycles_per_ns;
#endif
}

static long bench_nic_roundtrip(void)
{
	long start, stop;

	bzero((void *) pkts_rx, SIZE);
	init_rxq();

	tx_pkt();

	start = rdtsc();
	while (!my_rx(NULL))
		;
	stop = rdtsc();
	my_rx_complete();

	return stop - start;
}

static void run(enum agent_cmd_ids cmd_id, const char *msg, long (*bench_func)(void))
{
	long measurements[REPEAT], v;
	int warmup = REPEAT / 10;
	int j = 0;

	agent_tx_cmd(cmd_id, REPEAT + warmup);

	for (int i = 0; i < warmup; i++) {
		bench_func();
	}

	for (int i = 0; i < REPEAT; i++) {
		v = bench_func();
		if (!v) {
			fprintf(stderr, "%s - failed\n", msg);
			continue;
		}
		measurements[j++] = v;
	}

	print_results(msg, measurements, j);
}

int main(int argc, char **argv)
{
	int i, core, int_core;
	char msg[64];

	assert(argc >= 4);
	core = atoi(argv[1]);
	int_core = atoi(argv[2]);
	start(core, int_core);

	char *agent_host = argv[3];
	connect_to_agent(agent_host);
	agent_tx_rx_mac();

	// TODO: dpdk doesn't transmit the first packet
	sleep(2);

	run(AGENT_CMD_ECHO, "PCI write", bench_pci_write);
	run(AGENT_CMD_ECHO, "NIC fetches TX descriptor", bench_nic_reads_txd);
	run(AGENT_CMD_ECHO, "NIC fetches payload", bench_nic_reads_payload);
	run(AGENT_CMD_ECHO, "NIC updates TX descriptor", bench_nic_updates_rxd);
	run(AGENT_CMD_ECHO, "NIC updates TDH register", bench_nic_updates_tx_head_reg);
	run(AGENT_CMD_ECHO, "TX timestamp", bench_nic_tx_timestamp);

	/* phase 2 */
	// puts("");
	rte_eth_dev_stop(0);
	assert(rte_eth_dev_start(0) >= 0);

	// TODO: dpdk doesn't transmit the first packet
	sleep(2);

	run(AGENT_CMD_TX_1PKT, "NIC writes payload", bench_nic_rx_writes_payload);
#if IXGBE
	run(AGENT_CMD_TX_1PKT, "NIC updates RDH register", bench_nic_rx_updates_rx_head_reg);
	run(AGENT_CMD_TX_1PKT, "NIC fetches DMA RX descriptor", bench_nic_rx_reads_rxd);
#elif I40E
/* not supported */
#endif
	run(AGENT_CMD_TX_1PKT, "RX timestamp", bench_nic_rx_timestamp);

	// TODO: restart because i40e doesn't receive packets here
	rte_eth_dev_stop(0);
	assert(rte_eth_dev_start(0) >= 0);

	// TODO: dpdk doesn't transmit the first packet
	sleep(2);

	run(AGENT_CMD_TX_1PKT, "[TX/RX] round trip", bench_nic_roundtrip);

	return 0;
}
