# SM18635E：SFC1 / SPI1 内部 DMA 示例

以内核 `/home/a/hl/bu2_bulbcam/platform_t33a/kernel-4.4.94` 为准，
使用 `ingenic_sfc_spi.c` 的 SFC1 内部描述符 DMA，默认设备 `/dev/spidev1.0`。
本次沿用现有打包输入的 PRJ009 / T32P 产品配置。

## 编译和运行

```sh
make
make check
# 1 颗芯片，全零灰度；发送两帧后退出
./sm18635_spi
# R 通道低灰度，五路增益最低档
./sm18635_spi -g 1024,0,0,0,0 -a 0,0,0,0,0
# 3 颗级联
./sm18635_spi -n 3 -g 1024,512,256,0,0 -a 0,0,0,0,0
# 最大 R 灰度，电流增益仍为最低档
./sm18635_spi -g 65535,0,0,0,0 -a 0,0,0,0,0
# 只发送一次（两帧）全零灰度后退出，用于关灯
./sm18635_spi -g 0,0,0,0,0 -a 0,0,0,0,0
# 离线生成波形
./sm18635_spi -g 0x1234,0x5678,0,0,0 -o waveform.bin
```

默认交叉编译工具链 `mips-linux-uclibc-`，`sm18635_spi` 用于目标板，
`sm18635_spi_host` 仅用于本机测试。默认 TX-only；旧 `-t` 参数保留兼容，
无需再手动指定。8 bits/word、MSB-first、mode 0，请求时钟默认 2 MHz。
两帧放在一个 `SPI_IOC_MESSAGE(1)` 的单个 transfer 中，首帧唤醒、第二帧生效。
不提交 RX 缓冲区，适配 SFC1 半双工限制。1～32 颗，最长消息 3642 字节。
用户态只提交一次传输，等待两帧由 DMA 发送完成后关闭设备并退出，不循环发送。
芯片保持最后灰度；更新亮度时再次运行程序，关灯发送全零灰度。
传输失败或短传输会报错退出；`-o` 离线导出生成一个双帧文件后退出。
循环版本的 `-r`、`-i` 参数已移除。仅需替换用户态程序，无需重刷内核。

## 内核和接线

产品配置启用：

```text
CONFIG_SPI=y
CONFIG_INGENIC_SFC1_SSI=y
CONFIG_INGENIC_SFC1_SSI_DMA=y
CONFIG_SPI_SPIDEV=y
```

`SPI_MASTER`、`SPI_BITBANG` 由配置依赖生成。`SPI_BITBANG` 在这里是内核队列框架，
不表示 GPIO 软件模拟。SFC1 内部 DMA 不依赖 SSI1 的 PDMA 通道配置。
SFC0 继续服务启动 Flash，SFC1 Flash 节点保持关闭，`sfc1_spi` 注册 SPI bus 1。
板级仅复用 PC04（SFC1 DT）输出，保留 PC03 和 PC06/07 I2C1 的用途。
PC04 接 SM18635 DIN，共地；SCK/CS/DR 不接灯芯片。
该引脚配置服务单线灯控，普通四线 SPI 外设需另行配置完整引脚。

DMA 使用平台原生 CDT 模式、一致性缓冲区和单个 DMA 描述符，
单次最多 4096 字节，8 位 MSB-first，
等待 SFC END 后才返回成功。全双工、多 transfer 消息和 `cs_change` 被拒绝。
TX 首字节由 CDT 的 8 位命令阶段发送，余下字节由 DMA 发送；不会增删应用缓冲区字节，
不插入地址或 dummy clocks。SM18635 首字节是前导复位区的 `0x00`，有效编码全部在 DMA 阶段。
内部时钟门控设置、INCR16 burst 和 CDT 格式对齐平台 `ingenic_sfc_v2` 驱动。
发生超时或 FIFO 错误会在停止/清除硬件前记录 SR、FSM、CDT 和描述符，然后返回错误。
若 STOP 后控制器仍 busy，会禁用后续传输，避免复用仍被 DMA 访问的缓冲区。

## 重新打包

```sh
./build_boot.sh
```

脚本在目标内核目录重新生成 PRJ009 配置和 zImage，复用当前 release 的 ramdisk，
使用其开发签名密钥生成 FIT，并用已有 U-Boot 公钥验证，输出
`/home/a/hl/bu2_bulbcam/output/boot.img` 和配置、SHA256 文件。
需要目标目录写权限；不会重新构建整个应用或文件系统。

## 板端验证

```sh
ls -l /dev/spidev1.0
dmesg | grep -E 'SFC1|SFC1-SSI|DMA transfer'
./sm18635_spi -g 1024,0,0,0,0 -a 0,0,0,0,0
```

本次修正版 probe 应打印 `SFC1 SPI1 internal descriptor DMA enabled (CDT v2)`。
若仍超时，请保留 `SFC1 DMA error=`、`GLB0=`、`CDT0=` 三行日志和
`cat /proc/interrupts | grep -Ei 'sfc|spi'` 输出。
旧版 `status=0x0` 只是未收到完成中断的缓存结果，不是完整硬件状态。
时钟由内核时钟框架分频，实际频率可能低于请求值；需示波器检查 PC04 的
实际周期、帧头、字节间连续性和末尾低电平，并在视频/网络负载下检查断流。
编译和离线编码测试不替代目标板 DMA、中断及电气波形验证。

## 编码和原厂逻辑对应

SPI mode 0、8 bits/word、MSB first，`0 -> 1000`，`1 -> 1110`。
默认 SPI 时钟 2 MHz，对应芯片 500 kbps：

| 内容 | SPI 数据 | 2 MHz 时序 |
|---|---|---|
| 前置复位 | 128 个 0x00 | 512 us 低电平 |
| 自适应高电平 | 4 个 0xFF | 16 us = 8T |
| 自适应低电平 | 1 个 0x00 | 4 us = 2T |
| 数据 0 | 半字节 0x8 | 高 0.5 us、低 1.5 us |
| 数据 1 | 半字节 0xE | 高 1.5 us、低 0.5 us |
| 尾部复位 | 128 个 0x00 | 512 us 低电平 |

允许请求 SPI 时钟 1.6～3.2 MHz；复位字节数固定，在上限仍提供 320 us。
自适应低电平选择 2T，满足 200 ns～20 us，并方便字节对齐。
原厂例子依赖缺失头文件中的 RESET 等宏，本例不猜测宏值，按 PDF 确保复位至少 200 us。

每颗芯片按 `0x96 + R/G/B/W/Y 各16位 + control 8位` 排列，
整条级联末尾只发送一份 48 位寄存器：

```
0xE1 (8)
R_gain / G_gain / B_gain / W_gain / Y_gain (各6)
din_flip (1) + reserved1=0 (1)
reg_control (8)
```

所有多位字段高位先发，不使用依赖编译器布局的 C 位域。

- `-c` 为每颗芯片的控制字节，默认 0；原厂逻辑低4位为 width_comp。
- `-f` 为 DIN flip 位，默认 0。
- `-p` 为最后8位寄存器原始值；按原厂代码打包：
  `[7:5] pwm_freq, [4] iout_response, [3] pwm_phase, [2] reserved2, [1:0] standby_ctrl`。
  原代码没有提供 standby_ctrl 的结构体定义，这里保留整个原始字节，不推断待机枚举。
- 默认 gain 为 `25,25,25,63,63`，默认最后字节 `0xC0`，逐位保持原例初始化值。
  gain=25 对应手册约150.5 mA，gain=63约350 mA；亮灯示例使用最低 gain=0（约6 mA）。
- 原例将 `pwm_freq=6` 注释为32 kHz，而 PDF 写4/8/12 kHz；本例保留值6，
  **不声称它对应32 kHz**。DIN flip 的注释也与 PDF 不一致，保持原始位值。


## 内核 DMA 软件测试

```sh
make check-kernel
```

从实际内核驱动提取 DMA 传输函数，用模拟 MMIO 验证 CDT＋描述符的字节保持、
1/2/3/666/3642/4096 字节边界、超时清理前诊断、FIFO 错误和重试。
该测试不模拟芯片内部电路，不能证明硬件 DMA 已正常运行。
