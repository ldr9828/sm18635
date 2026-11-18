CROSS_COMPILE ?= mips-linux-uclibc-
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror

.PHONY: all check check-kernel clean
all: sm18635_spi

sm18635_spi: sm18635_spi.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

sm18635_spi_host: sm18635_spi.c
	$(HOSTCC) $(CPPFLAGS) $(CFLAGS) $< -o $@

HOSTCC ?= cc
test_spidev_mock.so: test_spidev_mock.c
	$(HOSTCC) -std=c99 -Wall -Wextra -Werror -fPIC -shared $< -o $@

check: sm18635_spi_host test_spidev_mock.so
	python3 test_encoding.py ./sm18635_spi_host
	python3 test_transfer.py ./sm18635_spi_host ./test_spidev_mock.so

check-kernel:
	python3 test_sfc_dma.py

clean:
	$(RM) sm18635_spi sm18635_spi_host test_spidev_mock.so
