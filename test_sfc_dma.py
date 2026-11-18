#!/usr/bin/env python3
"""Compile the real DMA transfer functions with fake MMIO; check wire bytes.
This tests software register/descriptor consistency, not real SFC hardware.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

kernel = Path('/home/a/hl/bu2_bulbcam/platform_t33a/kernel-4.4.94')
source = Path(sys.argv[1]) if len(sys.argv) > 1 else kernel / 'drivers/spi/ingenic_sfc_spi.c'
text = source.read_text()
header = (kernel / 'drivers/spi/ingenic_sfc_spi.h').read_text()
regs = header[header.index('#define SFC_GLB0'):header.index('struct ingenic_sfc_spi {')]
def part(start, end):
    return text[text.index(start):text.index(end, text.index(start))]
functions = part('static irqreturn_t ingenic_sfc_spi_dma_irq(', '/*\n * Setup transfer parameters')
constants = part('#define SFC_SPI_DMA_OFFSET', 'static int ingenic_sfc_spi_prepare_message')
shim = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef int irqreturn_t;
#define BIT(n) (1u << (n))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define SPI_CPOL 2
#define SPI_CPHA 1
#define SPI_CS_HIGH 4
struct spi_transfer { unsigned len; const void *tx_buf; void *rx_buf; };
struct spi_device { void *master; unsigned mode, chip_select; };
struct ingenic_sfc_spi {
 void *buffer; unsigned buffer_dma; void *iomem; unsigned dma_flag;
 int done, irq, lock; unsigned state; void *dev; unsigned long spi_clk;
};
static u32 registers[1024];
static unsigned failure, dumps, saw_pre_stop, starts;
static unsigned char expected[4096];
static unsigned expected_length;
#define spi_master_get_devdata(m) ((struct ingenic_sfc_spi *)(m))
#define synchronize_irq(irq) ((void)0)
#define spin_lock_irqsave(lock,flags) ((flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(flags))
#define reinit_completion(p) (*(p)=0)
#define complete(p) (*(p)=1)
#define wmb() ((void)0)
#define rmb() ((void)0)
#define msecs_to_jiffies(n) (n)
#define readl_poll_timeout(addr, v, cond, delay, limit) ((v)=registers[SFC_SR/4], (cond)?0:-ETIMEDOUT)
#define dev_err(dev, ...) do { dumps++; if(registers[SFC_SR/4]) saw_pre_stop++; } while(0)
static u32 sfc_readl(struct ingenic_sfc_spi *hw, unsigned reg) { return registers[reg/4]; }
static void sfc_writel(struct ingenic_sfc_spi *hw, unsigned reg, u32 val);
static unsigned fake_wait(struct ingenic_sfc_spi *hw, unsigned timeout);
#define wait_for_completion_timeout(done,timeout) fake_wait(hw,timeout)
'''
backend = r'''
static void sfc_writel(struct ingenic_sfc_spi *hw, unsigned reg, u32 val)
{
 registers[reg/4] = val;
 if(reg == SFC_SCR) registers[SFC_SR/4] &= ~val;
 if(reg != SFC_TRIG) return;
 if(val == TRIG_STOP) { registers[SFC_SR/4]=0; return; }
 if(val != TRIG_START) return;
 starts++;
 struct sfc_spi_dma_desc *d=hw->buffer;
 unsigned cmd=registers[(SFC_LUT+4)/4];
 assert(registers[SFC_CGE/4]==0);
 assert(registers[SFC_GLB0/4] & GLB_CDT_EN);
 assert((registers[SFC_GLB0/4]&GLB_BURST_MD_MSK)==2);
 assert(cmd & TRAN_CONF0_CMD_EN);
 assert((cmd & 255) == expected[0]);
 assert(d->length==expected_length-1);
 assert(registers[SFC_TRAN_LEN/4]==d->length);
 assert(d->buffer==hw->buffer_dma+64 && d->next==0 && d->link==0);
 assert(!memcmp((u8 *)hw->buffer+64,expected+1,d->length));
 assert(registers[SFC_CMD_IDX/4]&BIT(30));
 assert(!!(registers[SFC_CMD_IDX/4]&BIT(31))==!!d->length);
 assert(!!(cmd&TRAN_CONF0_DATEEN)==!!d->length);
 assert(!!(registers[SFC_GLB0/4]&GLB_OP_MODE)==!!d->length);
 registers[SFC_SR/4]=failure==1 ? BIT(7) : (failure==2 ? SFC_UNDER : SFC_END);
}
static unsigned fake_wait(struct ingenic_sfc_spi *hw, unsigned timeout)
{
 assert(timeout>=100);
 if(failure==1) return 0;
 assert(ingenic_sfc_spi_dma_irq(hw)==IRQ_HANDLED);
 assert(hw->done);
 return 1;
}
int main(void)
{
 unsigned char buffer[4160];
 struct ingenic_sfc_spi hw={.buffer=buffer,.buffer_dma=0x100000,
    .iomem=registers,.spi_clk=2000000};
 struct spi_device spi={.master=&hw};
 struct spi_transfer t={.tx_buf=expected};
 unsigned lengths[]={1,2,3,666,3642,4096};
 for(unsigned i=0;i<sizeof(expected);i++) expected[i]=(i*37+5)&255;
 for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
   expected_length=t.len=lengths[i];
   assert(ingenic_sfc_spi_dma_txrx(&spi,&t)==(int)t.len);
 }
 assert(starts==6);
 expected_length=t.len=666;
 failure=1;
 assert(ingenic_sfc_spi_dma_txrx(&spi,&t)==-ETIMEDOUT);
 assert(dumps && saw_pre_stop);
 failure=2;
 assert(ingenic_sfc_spi_dma_txrx(&spi,&t)==-EIO);
 failure=0;
 assert(ingenic_sfc_spi_dma_txrx(&spi,&t)==666);
 t.len=4097;
 assert(ingenic_sfc_spi_dma_txrx(&spi,&t)==-EINVAL);
 t.len=666;t.rx_buf=buffer;
 assert(ingenic_sfc_spi_dma_txrx(&spi,&t)==-EINVAL);
 puts("PASS: CDT/descriptor wire-byte preservation, length edges, timeout diagnostics, FIFO errors and retry");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    c.write_text(shim + regs + constants + functions + backend)
    exe = Path(tmp) / 'test'
    subprocess.run(['cc', '-std=gnu99', '-Werror=implicit-function-declaration', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
