#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>

#include "common.h"

int agent_fd;
struct ether_addr my_mac, remote_mac;
double cycles_per_ns;

static void wait_for_connection(void)
{
	struct sockaddr_in sin;
	int sock;
	int one;
	int ret;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	assert(sock);
	one = 1;
	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *) &one, sizeof(one));
	assert(!ret);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0);
	sin.sin_port = htons(PORT);
	ret = bind(sock, (struct sockaddr*)&sin, sizeof(sin));
	assert(!ret);
	ret = listen(sock, 1);
	assert(!ret);
	agent_fd = accept(sock, NULL, NULL);
}

static void agent_tx_data(const char *data, int len)
{
	ssize_t ret;

	ret = write(agent_fd, data, len);
	assert(ret == len);
}

static void agent_rx_cmd(struct agent_cmd *cmd)
{
	ssize_t ret;

	ret = read(agent_fd, cmd, sizeof(*cmd));
	if (ret == 0) {
		cmd->id = AGENT_CMD_INVALID;
		return;
	}
	assert(ret == sizeof(*cmd));
}

int main(int argc, char **argv)
{
	struct rte_mbuf *mbuf;
	struct agent_cmd cmd;

	assert(argc >= 2);
	int numa_node = atoi(argv[1]);
	wait_for_connection();
	dpdk_init(numa_node);
	cycles_per_ns = rte_get_tsc_hz() / 1e9;

	// TODO: dpdk doesn't transmit the first packet
	sleep(2);

	agent_tx_rx_mac();
	while (1) {
		agent_rx_cmd(&cmd);
		if (!cmd.id)
			break;
		switch (cmd.id) {
		case AGENT_CMD_NOP:
			for (int i = 0; i < cmd.count; i++) {
				mbuf = rx_pkt();
				rte_pktmbuf_free(mbuf);
			}
			break;
		case AGENT_CMD_ECHO:
			// printf("phase1. waiting for packets...\n");
			for (int i = 0; i < cmd.count; i++) {
				mbuf = rx_pkt();
				// printf("phase1. received packet %d/%d\n", i, cmd.count);
				agent_tx_data(rte_pktmbuf_mtod(mbuf, void *), mbuf->data_len);
				rte_pktmbuf_free(mbuf);
			}
			break;
		case AGENT_CMD_TX_1PKT:
			// printf("phase2. waiting for packets %d\n", cmd.count);
			for (int i = 0; i < cmd.count; i++) {
				mbuf = rx_pkt();
				// printf("phase2. received packet %d/%d\n", i, cmd.count);
				tx_pkt();
				rte_pktmbuf_free(mbuf);
			}
			break;
		case AGENT_CMD_TX_2PKTS:
			// printf("start %d\n", cmd.count);
			for (int i = 0; i < cmd.count; i++) {
				mbuf = rx_pkt();
				rte_pktmbuf_free(mbuf);
				// printf("send packet %d/%d\n", i, cmd.count);
				tx_pkt();
				delay_us(10);
				tx_pkt();
			}
			break;
		}
		continue;
	}
}
