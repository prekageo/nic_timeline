#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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

#include "bench.h"
#include "common.h"

struct ether_addr my_mac, remote_mac;
struct pkt1 *pkts1;
volatile struct pkt_rx *pkts_rx;
uint64_t pmem;
double cycles_per_ns;
int agent_fd;

static struct rte_eth_dev *dev;

#if IXGBE

struct ixgbe_tx_queue *txq;
struct ixgbe_rx_queue *rxq;
struct ixgbe_hw *hw;

#elif I40E

struct i40e_tx_queue *txq;
struct i40e_rx_queue *rxq;
struct i40e_hw *hw;

#endif

static int int_core;
static sem_t int_sem_start;
static sem_t int_sem_done;
static struct args_read int_read;

void connect_to_agent(const char *agent_host)
{
	int ret;
	struct sockaddr_in addr;
	struct hostent *host;

	host = gethostbyname(agent_host);
	assert(host);
	assert(host->h_length == 4);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	memcpy(&addr.sin_addr, host->h_addr_list[0], 4);

	agent_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(agent_fd > 0);
	ret = connect(agent_fd, (struct sockaddr *) &addr, sizeof(addr));
	assert(ret == 0);
}

void agent_rx_data(void *buf, int len)
{
	ssize_t ret;

	ret = read(agent_fd, buf, len);
	assert(ret == len);
}

void agent_rx_data_ignore(void)
{
	struct pkt1 rcv_pkt;
	agent_rx_data(&rcv_pkt, PKT_SIZE);
}

void agent_tx_cmd(int id, int count)
{
	struct agent_cmd cmd;

	cmd.id = id;
	cmd.count = count;

	ssize_t ret = write(agent_fd, &cmd, sizeof(cmd));
	assert(ret == sizeof(cmd));
}

#if IXGBE

void init_rxd(int idx)
{
	volatile union ixgbe_adv_rx_desc *rxdp;
	rxdp = &rxq->rx_ring[idx];
	rxdp->read.hdr_addr = 0;
	rxdp->read.pkt_addr = pmem + idx * 2048;
}

#elif I40E

void init_rxd(int idx)
{
	volatile union i40e_rx_desc *rxdp;
	rxdp = &rxq->rx_ring[idx];
	rxdp->read.hdr_addr = 0;
	rxdp->read.pkt_addr = pmem + idx * 2048;
}

#endif

static uint64_t virt2phy(const void *virtaddr)
{
	uint64_t page, physaddr;

	int fd = open("/proc/self/pagemap", O_RDONLY);
	assert(fd >= 0);
	int ret = pread(fd, &page, sizeof(uint64_t), (unsigned long)virtaddr / 4096 * sizeof(uint64_t));
	assert(ret == sizeof(uint64_t));
	close(fd);
	physaddr = ((page & 0x7fffffffffffffULL) * 4096) + ((unsigned long)virtaddr % 4096);
	return physaddr;

}

void alloc_mem(void)
{
	pkts1 = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	assert(pkts1 != MAP_FAILED);
	pkts_rx = (void *) pkts1;
	int ret = mlock(pkts1, SIZE);
	assert(ret == 0);
	pmem = virt2phy(pkts1);
}

// static void cpu_relax(void)
// {
	// asm volatile("pause":::"memory");
// }

void init_rxq(void)
{
	assert(rxq->nb_rx_desc <= SIZE / 2048);
	for (int i = 0; i < rxq->nb_rx_desc; i++)
		init_rxd(i);
}

#if IXGBE
static void ixgbe_set_ivar(struct ixgbe_hw *hw, int8_t direction, uint8_t queue, uint8_t msix_vector, int enable)
{
	uint32_t tmp, idx;

	if (enable)
		msix_vector |= IXGBE_IVAR_ALLOC_VAL;
	idx = ((16 * (queue & 1)) + (8 * direction));
	tmp = IXGBE_READ_REG(hw, IXGBE_IVAR(queue >> 1));
	tmp &= ~(0xFF << idx);
	tmp |= (msix_vector << idx);
	IXGBE_WRITE_REG(hw, IXGBE_IVAR(queue >> 1), tmp);
	// printf("IXGBE_IVAR = %x\n", tmp);
}
#elif I40E
static void i40e_bind_intr(struct i40e_hw *hw, int8_t direction, uint8_t queue, uint8_t msix_vector, int enable)
{
	int reg, val;

	if (direction)
		reg = I40E_QINT_TQCTL(queue + 1);
	else
		reg = I40E_QINT_RQCTL(queue + 1);

	val = (msix_vector << I40E_QINT_RQCTL_MSIX_INDX_SHIFT) | 0 << I40E_QINT_RQCTL_ITR_INDX_SHIFT | (0x7ff << I40E_QINT_RQCTL_NEXTQ_INDX_SHIFT) | (0 << I40E_QINT_RQCTL_NEXTQ_TYPE_SHIFT);

	if (enable)
		val |= I40E_QINT_RQCTL_CAUSE_ENA_MASK;

	// fprintf(stderr, "%x\n", val);
	I40E_WRITE_REG(hw, reg, val);
}

#endif

void start(int core, int int_core)
{
#ifndef RTE_LIBRTE_IEEE1588
	fprintf(stderr, "Compile DPDK with RTE_LIBRTE_IEEE1588\n");
	exit(1);
#endif

	assert(sizeof(struct pkt1) == PKT_SIZE);
	assert(sizeof(struct pkt_rx) == 2048);
	alloc_mem();
	dpdk_init(core);

	dev = &rte_eth_devices[0];
#if IXGBE
	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
#elif I40E
	hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
#endif
	txq = dev->data->tx_queues[0];
	rxq = dev->data->rx_queues[0];

	init_rxq();
	rte_eth_macaddr_get(0, &my_mac);
	cycles_per_ns = rte_get_tsc_hz() / 1e9;
}
