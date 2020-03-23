#pragma once

/* avoid Symbol is not yet part of stable ABI compile-time warnings */
#define ALLOW_EXPERIMENTAL_API

#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_bus_pci.h>

#if IXGBE

#include <../../drivers/net/ixgbe/ixgbe_rxtx.h>
#include <../../drivers/net/ixgbe/ixgbe_ethdev.h>

#elif I40E

#define X722_SUPPORT
#define PF_DRIVER
#include <../../drivers/net/i40e/base/i40e_type.h>
#include <../../drivers/net/i40e/base/virtchnl.h>
#include <../../drivers/net/i40e/i40e_rxtx.h>
#include <../../drivers/net/i40e/i40e_ethdev.h>

#endif

#define SIZE 4 * 1024 * 1024
#define MAX_PKT (SIZE / PKT_SIZE)

struct pkt1 {
	struct ether_hdr ether_hdr;
	volatile long timestamp;
	char padding[42];
} __attribute__((packed));

struct pkt_rx {
	struct ether_hdr ether_hdr;
	char message_id;
	char version_ptp;
	char padding1[1024-14-2];
	volatile long timestamp;
	char padding2[1024-8];
} __attribute__((packed));

#if IXGBE

extern struct ixgbe_tx_queue *txq;
extern struct ixgbe_rx_queue *rxq;
extern struct ixgbe_hw *hw;

#elif I40E

extern struct i40e_tx_queue *txq;
extern struct i40e_rx_queue *rxq;
extern struct i40e_hw *hw;

#endif

extern struct pkt1 *pkts1;
extern uint64_t pmem;
extern volatile struct pkt_rx *pkts_rx;

void agent_rx_data(void *buf, int len);
void agent_rx_data_ignore(void);
void agent_rx_mac(void);
void agent_tx_cmd(int id, int count);
void alloc_mem(void);
void connect_to_agent(const char *agent_host);
void init_rxd(int idx);
void init_rxq(void);
void start(int core, int int_core);

#if IXGBE

static inline volatile union ixgbe_adv_tx_desc *__my_tx(uint64_t physaddr, int size, uint64_t extra_flags)
{
	uint64_t flags = DCMD_DTYP_FLAGS | extra_flags;
	uint16_t tx_id;
	volatile union ixgbe_adv_tx_desc *txdp;

	tx_id = txq->tx_tail;
	txdp = &txq->tx_ring[tx_id];
	__m128i descriptor = _mm_set_epi64x((uint64_t)size << 46 | flags | size, physaddr);
	// printf("%lx\n",(uint64_t)size << 46 | flags | size);
	_mm_store_si128((__m128i *)&txdp->read, descriptor);
	tx_id = (uint16_t)(tx_id + 1);
	txq->tx_tail = tx_id;
	if (txq->tx_tail >= txq->nb_tx_desc)
		txq->tx_tail = 0;

	return txdp;
}

static inline volatile union ixgbe_adv_tx_desc *my_tx(uint64_t physaddr, int size, uint64_t extra_flags)
{
	volatile union ixgbe_adv_tx_desc *txdp;
	txdp = __my_tx(physaddr, size, extra_flags);
	IXGBE_PCI_REG_WRITE(txq->tdt_reg_addr, txq->tx_tail);

	return txdp;
}

static inline volatile struct pkt_rx *my_rx(void *unused)
{
	volatile union ixgbe_adv_rx_desc *rxdp = &rxq->rx_ring[rxq->rx_tail];
	if (!(rxdp->wb.upper.status_error & IXGBE_RXDADV_STAT_DD))
		return NULL;
	volatile struct pkt_rx *ret = &pkts_rx[rxq->rx_tail];
	return ret;
}

static inline void my_rx_complete(void)
{
	rxq->rx_tail = (uint16_t)(rxq->rx_tail + 1);
	init_rxd(rxq->rx_tail - 1);
	IXGBE_PCI_REG_WRITE(rxq->rdt_reg_addr, rxq->rx_tail - 1);
	if (rxq->rx_tail >= rxq->nb_rx_desc)
		rxq->rx_tail = 0;
}

#elif I40E

static inline uint64_t i40e_build_ctob(uint32_t td_cmd, uint32_t td_offset, unsigned int size, uint32_t td_tag)
{
	return I40E_TX_DESC_DTYPE_DATA |
		((uint64_t)td_cmd  << I40E_TXD_QW1_CMD_SHIFT) |
		((uint64_t)td_offset << I40E_TXD_QW1_OFFSET_SHIFT) |
		((uint64_t)size  << I40E_TXD_QW1_TX_BUF_SZ_SHIFT) |
		((uint64_t)td_tag  << I40E_TXD_QW1_L2TAG1_SHIFT);
}

static inline volatile struct i40e_tx_desc *__my_tx(uint64_t physaddr, int size, uint64_t extra_flags)
{
	uint16_t tx_id;
	volatile struct i40e_tx_desc *txdp;
	uint64_t flags = I40E_TX_DESC_CMD_ICRC | I40E_TX_DESC_CMD_EOP | extra_flags;

	tx_id = txq->tx_tail;
	txdp = &txq->tx_ring[tx_id];
	uint64_t high_qw = i40e_build_ctob(flags, 0, size, 0);
	__m128i descriptor = _mm_set_epi64x(high_qw, physaddr);
	_mm_store_si128((__m128i *)txdp, descriptor);
	tx_id = (uint16_t)(tx_id + 1);
	txq->tx_tail = tx_id;
	if (txq->tx_tail >= txq->nb_tx_desc)
		txq->tx_tail = 0;

	return txdp;
}

static inline volatile struct i40e_tx_desc *my_tx(uint64_t physaddr, int size, uint64_t extra_flags)
{
	volatile struct i40e_tx_desc *txdp;

	if (extra_flags == 42) {
		volatile struct i40e_tx_context_desc *ctx_txd = (volatile struct i40e_tx_context_desc *)&txq->tx_ring[txq->tx_tail];
		ctx_txd->l2tag2 = 0;
		ctx_txd->tunneling_params = 0;
		ctx_txd->type_cmd_tso_mss = I40E_TX_DESC_DTYPE_CONTEXT | ((uint64_t)I40E_TX_CTX_DESC_TSYN << I40E_TXD_CTX_QW1_CMD_SHIFT);
		if (++txq->tx_tail >= txq->nb_tx_desc)
			txq->tx_tail = 0;
		extra_flags = 0;
	}

	txdp = __my_tx(physaddr, size, extra_flags);
	I40E_PCI_REG_WRITE(txq->qtx_tail, txq->tx_tail);

	return txdp;
}

static inline volatile struct pkt_rx *my_rx(uint64_t *ret_qword1)
{
	uint64_t qword1;
	uint32_t rx_status;
	volatile union i40e_rx_desc *rxdp = &rxq->rx_ring[rxq->rx_tail];
	qword1 = rxdp->wb.qword1.status_error_len;
	rx_status = (qword1 & I40E_RXD_QW1_STATUS_MASK) >> I40E_RXD_QW1_STATUS_SHIFT;
	if (!(rx_status & (1 << I40E_RX_DESC_STATUS_DD_SHIFT)))
		return NULL;
	volatile struct pkt_rx *ret = &pkts_rx[rxq->rx_tail];
	if (ret_qword1)
		*ret_qword1 = qword1;
	return ret;
}

static inline void my_rx_complete(void)
{
	rxq->rx_tail = (uint16_t)(rxq->rx_tail + 1);
	init_rxd(rxq->rx_tail - 1);
	I40E_PCI_REG_WRITE(rxq->qrx_tail, rxq->rx_tail - 1);
	if (rxq->rx_tail >= rxq->nb_rx_desc)
		rxq->rx_tail = 0;
}

#endif
