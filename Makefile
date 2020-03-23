DPDK=$(HOME)/dpdk/v18.02/build

CFLAGS=-Wall -march=native -O2 -g -I$(DPDK)/include $(DEFINES) -MD
LDFLAGS=-L$(DPDK)/lib
LDLIBS=-pthread -Wl,--whole-archive -Wl,-ldpdk -Wl,--no-whole-archive -Wl,-lnuma -Wl,-ldl -Wl,-lm

all: nic_timeline_ixgbe nic_timeline_i40e agent

%_ixgbe.o:: DEFINES=-DIXGBE=1
%_ixgbe.o: %.c
	$(COMPILE.c) $(OUTPUT_OPTION) $<

%_i40e.o:: DEFINES=-DI40E=1
%_i40e.o: %.c
	$(COMPILE.c) $(OUTPUT_OPTION) $<

%_ixgbe: %_ixgbe.o common.o bench_ixgbe.o
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

%_i40e: %_i40e.o common.o bench_i40e.o
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

agent: agent.o common.o

clean:
	rm -f nic_timeline_ixgbe nic_timeline_i40e agent *.o *.d

-include *.d
