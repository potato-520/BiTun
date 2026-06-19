# Forwarding Makefile for BiTun project root

.PHONY: all clean test

all:
	$(MAKE) -C src/linux

clean:
	$(MAKE) -C src/linux clean

test: all
	bash run_integration_test.sh
