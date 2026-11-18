# SM18635：可选 GPIO 正脉冲验证项目

这个版本不使用 UART 控制器，也不打开串口。独立内核模块接管一个未占用的 GPIO，
默认 **GPIO72 = PC08**，空闲保持物理低电平，逐位输出正脉冲；不需要外部反相器。
用户态只提交一次请求，由内核发两帧（唤醒帧＋应用帧），无 DIN 接收路径。

不再固定检查 UART0。真正空闲的引脚直接申请；若被复用占用，默认尝试一次
**有安全限制的自动解绑**，成功后复查，再申请 GPIO，失败才报告原因并退出。

自动解绑目前仅允许：能确认状态的 `ingenic-uart`、不是 console/boot console、
没有打开者或等待打开者。SPI/SFC、Flash、存储、I2C、PWM、未知驱动均不在自动解绑
允许列表，需要手动评估依赖后释放。GPIO 消费者/sysfs 导出也不自动强抢。

解绑会停掉整路 UART（TX 和 RX），不是只释放一根脚。模块加载前仍应停止相关
应用/getty，加载过程中不能有程序重新打开串口、改变 console 或并发 bind/unbind。
这是检查时刻的安全判断，不是跨所有驱动/用户进程的原子抢占协议。
如不希望自动解绑，可在加载时指定 `auto_unbind=0`，保留仅检查、拒绝占用的行为。

## 换 GPIO：只改一个地方

编辑 `include/sm18635_protocol.h` 的唯一输出定义：

```c
#define SM18635_GPIO 72u
```

编号：PA=n，PB=32+n，PC=64+n；例如 PC08=72、PC09=73。本版仅支持 PRJ009
片上 PA/PB/PC，且需 pinctrl/GPIO 控制器确实注册该引脚。编号有效不代表封装已引出，
软件无占用也不代表板上可用；先确认原理图，不能盲试 Flash、电源使能、复位等关键脚。

随后可用一键脚本从当前源码全新构建模块和应用：

```sh
./build.sh
```

产物为 `build/latest/sm18635_gpio_drv.ko` 和 `build/latest/sm18635_gpio`。
如果只想增量重编模块，原脚本仍保留：

```sh
sh build_module.sh build
```

不调用脚本时，当前已准备好的构建目录可直接使用：

```sh
make -C /srv/build/users/a/al_charged/bu2_bulbcam/platform_t33a/kernel-4.4.94 \
  O="$PWD/build/kernel" ARCH=mips CROSS_COMPILE=mips-linux-gnu- \
  M="$PWD/kernel" modules
```

用户态从模块查询真实 GPIO 号，日志中的 GPIO 和 PA/PB/PC 名称自动更新，后续换脚
不必重编用户态。**本次升级增加查询功能、ABI 升为 2，首次须同时替换新 `.ko` 和
新用户态程序**；旧程序会被明确拒绝，避免显示 GPIO72 却实际驱动另一根脚。

## 隔离范围和当前验证结果

所有新增源码、生成文件都在本目录：

```text
sm18635_gpio/
  kernel/sm18635_gpio_drv.c      外置内核模块源码
  kernel/pin_guard.h            实时 pinctrl 占用检查
  kernel/unbind_policy.h        自动解绑的保守允许列表和条件
  kernel/sm18635_gpio_drv.ko     编译产物
  include/sm18635_protocol.h    28 字节 ABI、协议字段与脉冲计划
  src/sm18635_gpio.c            用户态发送程序
  build/user-mips/sm18635_gpio  MIPS/uClibc 编译产物
  build/kernel/                内核 O= 构建输出（不写平台仓）
  build.sh                     一键全新编译模块和应用
  build/runs/clean.XXXXXXXX/    每次独立的源码快照、构建目录、日志和产物
  build/latest/                指向最近一次成功全新构建的产物目录
  tests/                       本机协议与用户态模拟测试
```

未修改 `bu2_bulbcam`、`platform_t33a`、原 SPI/UART demo，未打包或刷写固件。
模块引用外部平台内核源码，只读；`build_module.sh` 强制构建输出位于本项目
`build/` 中。板端加载/解绑是运行时操作，不会修改设备树文件或 Flash。

已经通过 MIPS 用户态和外置 `.ko` 编译、本机协议、占用判断和用户态请求测试。
旧版 GPIO72 曾由用户反馈板端控灯成功；**本次自动解绑版本尚未板测**。
本次沿用 ABI 2，已经使用 ABI 2 用户态程序时只需替换模块。
当前模块基于 `PRJ009_bulbcam_defconfig` 构建，vermagic 为：

```text
4.4.94 preempt mod_unload MIPS32_R2 32BIT
```

当前独立构建没有实际设备对应的 `Module.symvers`，且此配置未启用 MODVERSIONS。
因此编译通过不等于与板上内核 ABI 一致；必须核对运行内核版本、实际 `.config`
和导出符号。不要使用 `insmod -f` 或强制卸载来绕过不匹配。

该 4.4 平台没有导出的实时 pinmux-owner 查询 API，因此本模块只读引用对应内核的
`drivers/pinctrl/pinctrl-ingenic.h` / `core.h` 私有结构，不修改平台仓。自动解绑还依赖
同源 `ingenic_uart.c` 中首成员为 `uart_port` 的布局，读取串口打开计数。这进一步要求
**目标内核 pinctrl 源码及配置完全匹配**，不能只看 vermagic 相同。控制器或描述符
无法确认时失败，不回退到强占。

## 构建

### 推荐：一键全新构建，不复用旧缓存

```sh
cd /srv/build/users/a/al_charged/demo/sm18635_gpio
./build.sh
```

这里的“内核程序”是外置 `sm18635_gpio_drv.ko` 模块，不是完整 zImage/boot.img。
脚本使用 Bash 和常规 Linux 工具（包括 find、grep），不依赖 rg/ripgrep。
脚本每次新建 `build/runs/clean.XXXXXXXX`，复制当前工作区的源文件和头文件，
不使用 git 提交快照、不复制旧模块对象/自动生成的 `.mod.c`，也不读取旧 CMake 缓存
或 `build/kernel/.config`。内核从新目录执行配置和 `modules_prepare`，应用从新的
CMake 构建目录编译；禁用 ccache、要求 sccache 重新编译。

默认内核工具链 `mips-linux-gnu-`、应用工具链 `mips-linux-uclibc-`，并行数 4。
支持 `KERNEL_SRC/KERNEL_CONFIG/KERNEL_SYMVERS/KERNEL_CROSS_COMPILE`，以及
`APP_CROSS_COMPILE`、`JOBS` 环境变量；明确指定的板端配置/符号表是构建输入，不是
复用旧编译缓存。未指定配置时，每次重新使用平台源码中的 `PRJ009_bulbcam_defconfig`。

只有模块和应用都编译成功、且输入文件校验未变化，才原子更新 `build/latest` 链接。
构建时不要编辑源码。失败会保留本次日志且退出非零，不更新 latest；若 latest 已存在，
它仍是上次成功的文件，不能误认为本次成功。旧构建目录保留，不自动删除。

**一键脚本不更新旧的 `kernel/sm18635_gpio_drv.ko` 或 `build/user-mips/sm18635_gpio`，
请从以下新路径复制板端文件，避免拿错旧产物：**

```text
build/latest/sm18635_gpio_drv.ko
build/latest/sm18635_gpio
build/latest/SHA256SUMS
build/latest/BUILD_INFO.txt
```

本次的 `kernel.log`、`application.log`、源码 SHA256 和实际 `kernel.config` 都保留
在独立构建目录中。完整源码检查和 SHA256 不能代替设备内核 ABI、实际波形验证。

### 原手动/增量构建方式

在本项目目录执行，兼容 CMake 3.5+：

```sh
# 用户态，默认 mips-linux-uclibc-
cmake -H. -Bbuild/user-mips -DCMAKE_BUILD_TYPE=Release
cmake --build build/user-mips

# 模块：默认使用平台要求的 mips-linux-gnu-，输出全部留在当前项目
sh build_module.sh all
```

默认只读内核源码路径：
`/srv/build/users/a/al_charged/bu2_bulbcam/platform_t33a/kernel-4.4.94`。
也可以指定 `KERNEL_SRC`、`KERNEL_CROSS_COMPILE`。要匹配真实设备，提供其内核
构建使用的配置和符号表（配置须是解压后的文本）：

```sh
KERNEL_CONFIG=/path/to/board-kernel.config \
KERNEL_SYMVERS=/path/to/matching/Module.symvers \
sh build_module.sh all
```

不要把用户态 uClibc 工具链与内核工具链混淆。已有配置在 `build/kernel/.config`；
不提供 `KERNEL_CONFIG` 时会沿用它。CONFIG_MODVERSIONS=y 时，脚本要求提供匹配
的 Module.symvers；`modules_prepare` 本身不会产生完整内核的符号表。

## 板端：检查所选 GPIO，按需释放后加载

先把两个产物复制到设备的验证目录。以下步骤在目标板以 root 运行，建议通过 SSH
或独立控制台操作。先只接示波器确认波形，再接灯芯片。

```sh
uname -r
cat /proc/cmdline
cat /sys/kernel/debug/gpio
cat /sys/kernel/debug/pinctrl/*/pinmux-pins
```

若目标脚已经空闲，直接加载模块即可。如果 debugfs 未启用/未挂载，这些查看命令可能
不可用，但模块仍会检查实时 pinctrl 占用。若是闲置非控制台 Ingenic UART，会尝试
自动解绑；其它情况在 dmesg 打印 GPIO 号、占用者和失败/拒绝原因，不能盲目强制释放。

例如选中 GPIO72 时，原 UART0 会占用 PC08：先停止 ttyS0 的应用/getty，并确认
它不是当前终端或 console/earlycon。**不能从 ttyS0 会话中解绑它自己**。
其他 GPIO 不要照抄 UART0 的解绑命令，只处理实际占用目标引脚的设备。

从 sysfs 确认 UART0 平台设备名称及其 `of_node` 指向
`/apb/serial@0x10030000`。常见设备名为 `10030000.serial`，以下命令只适用于
确认名称相同的情况；若不同，使用核实后的 UART0 名称，不要解绑 UART1。

```sh
readlink -f /sys/bus/platform/devices/10030000.serial/of_node

# 你之前已 export 并拉低 GPIO72：需要释放 sysfs 占用，不用 echo 1
if [ -d /sys/class/gpio/gpio72 ]; then
    echo 72 > /sys/class/gpio/unexport
fi

# 默认尝试自动解绑符合条件的 UART0，无需事先手动写 unbind
insmod ./sm18635_gpio_drv.ko
dmesg | tail -n 20
ls -l /dev/sm18635_gpio
```

上面的 GPIO72 和 UART0 查询仅是默认引脚示例。若换了 GPIO，释放对应的 sysfs
导出；若未被导出，不必执行 unexport。没有复用占用时，不会执行任何解绑。
要禁用自动解绑，改用 `insmod ./sm18635_gpio_drv.ko auto_unbind=0`，此时占用设备
必须事先手动释放；对于 GPIO72 的原 UART0，手动命令仍是
`echo 10030000.serial > /sys/bus/platform/drivers/ingenic-uart/unbind`。

模块加载按如下顺序处理：

1. 确认 PRJ009 片上控制器和有效引脚，拒绝可睡眠 GPIO。
2. 在 pinctrl mutex 下检查实时所有者；空闲继续，GPIO占用则报错；复用占用时复制
   所有者名称，释放 mutex 后解析设备并检查自动解绑条件。
3. 允许自动解绑时调用 `device_release_driver()` 一次，再检查设备已无驱动且目标脚
   的 pinctrl 所有者已清空；失败打印错误，不设置 GPIO 输出。不在持有 pinctrl mutex
   时解绑，否则 remove 路径释放 pinctrl 会死锁。
4. 先 `gpio_request()` 申请，不先改变电平；已被 sysfs/其他 GPIO 用户申请就失败。
5. 复查无复用占用，再设置输出低电平；设置后再次核对所有者和硬件输出模式。

`device_release_driver()` 本身没有返回码，因此以解绑后的驱动/引脚状态判断是否成功。
该接口同步执行 remove，没有额外强制超时或中途取消机制。若解绑成功，但后续申请
GPIO/创建设备节点失败，**原 UART 仍保持解绑**，日志会明确提示；不会静默重新绑定
导致引脚突然回到 UART 高电平。不要通过 `gpio_free()` 强行释放别人的 GPIO 预约。

不使用平台的 `used_pins_bitmap` 作为占用判据，也不擅自清除它：该表可能保留
历史映射，所以平台原有的 `gpio functions has redefinition` 提示仍可能出现。
实时所有者不为空会直接报错；不能用清除历史位图来绕过真实占用。
如果板上没有自动创建设备节点，按 `/sys/class/misc/sm18635_gpio/dev` 给出的
主/次设备号创建字符节点，权限设为 `0600`，不要猜 minor 号。

加载后所选脚进入 GPIO 输出低电平。每次发送前复查所有者及硬件输出模式，发送整次
请求期间持有 pinctrl mutex，防止常规 pinctrl 复用配置插入两帧之间。
这会阻塞该 pinctrl 控制器的其他复用配置操作，但不会在复位等待期间关闭中断。

本平台未启用 strict pinmux 互斥，检查不能阻止所有后续改脚行为：`gpio_request`
和 `gpio_direction_output` 内部自行锁定 pinctrl，模块加载期间的“复查→设置方向”
不是一个原子事务。**加载/卸载期间不要并发绑定、解绑其他设备，模块使用期间不要
重新复用所选脚、直接写 GPIO 寄存器或进入 suspend/resume。** 后续发现复用被占用
或输出模式改变会拒绝发送，不再强行拉低新占用者的引脚；卸载也不会恢复旧外设功能。

## 发送

```sh
# 默认：1 颗、全零灰度，发送两帧后退出，所选 GPIO 留在低电平
./sm18635_gpio

# R 通道低灰度，最低增益；首次亮灯优先用这个
./sm18635_gpio -g 1024,0,0,0,0 -a 0,0,0,0,0

# 全零灰度关灯请求；成功仍需确认灯端实际响应
./sm18635_gpio -g 0,0,0,0,0 -a 0,0,0,0,0

# 查看内核测得的两次 burst 持续时间
dmesg | tail -n 20
```

`-d` 是内核模块节点，默认 `/dev/sm18635_gpio`，**不是 `/dev/ttyS0`**。
程序会先查询驱动 ABI 和 GPIO 号，拒绝向普通 tty/文件写入请求。查询只是本地元数据，
不是接收灯芯片数据。发送是一个 28 字节请求，内核自行输出两帧；失败、短写或
被信号打断不自动重试，因为此前可能已发送部分帧。

支持 `-n 1..4`、`-g` 五路16位灰度、`-a` 五路6位增益、`-c/-f/-p` 原始控制字段。
默认增益 `25,25,25,63,63`、control=0、din_flip=0、reg_control=0xc0，与 SPI 相同。
字段仍是 MSB-first，级联末尾只带一份共享寄存器。无 UART 波特率参数。

## 波形与风险：这仍是软件定时，不是 SPI DMA

| 阶段 | 请求的物理电平与时间 |
|---|---|
| 前后复位/空闲 | 低电平，复位等待至少约 512 us，调度可延长 |
| 帧头 | 高 16 us，低 4 us |
| 数据 0 | 高 500 ns，低 1500 ns |
| 数据 1 | 高 1500 ns，低 500 ns |

数据内容和名义时序来自原 SPI 2 MHz 方案。与 UART 不同，此处没有硬件强加的
起始位/停止位，真正由 GPIO 形成低电平空闲和正脉冲，而不是仅将数据字节取反。

但 `gpio_set_value()`、总线写入、循环以及 MIPS `ndelay()` 都有开销，**实际高低
脉宽会偏离表中的请求值**，也受 CPU 时钟/延时校准影响。关本地中断和禁止抢占
只能减少部分打断，不能把软件 bit-bang 变成硬件精确定时。需逐项量测，不声称
能够直接替代已经验证的 SPI DMA 时序。

内核在每帧的“帧头＋数据”期间禁止本地中断和抢占，复位低电平期间恢复中断并睡眠。
1 颗的名义 burst 为 308 us，4 颗为 884 us，实际更长；所以验证版限制最多 4 颗，
建议先 1 颗。在视频/音频/网络负载下可能造成响应延迟，不能直接投入生产。
若完整 burst 实测超过 2 ms，发送返回 ETIMEDOUT、保留低电平且不继续下一帧；
**该检查发生在 burst 完成之后，不是 2 ms 硬截止看门狗**。
关键区中不分配内存、不打印、不访问用户内存，也不在每个位之间采样计时。
信号在帧间处理；强制卸载不可用作打断手段。

示波器至少检查：模块加载后的低电平、16 us 帧头、短/长高脉冲、低电平间隔、
位间连续性、双帧和发送后低电平。dmesg 的整帧耗时不能代替这些测量。

## 卸载及恢复原外设（如有）

先让发送程序退出，再执行：

```sh
rmmod sm18635_gpio_drv
# 仅默认 GPIO72 示例：如曾解绑 UART0 且需要恢复 ttyS0
echo 10030000.serial > /sys/bus/platform/drivers/ingenic-uart/bind
```

模块卸载前确认仍无复用冲突且仍是 GPIO 输出，才拉低并释放 GPIO；不自动重绑外设。
自动解绑成功的 UART 在卸载模块后仍保持解绑，日志会给出设备名，需安全确认后手动
恢复。释放后不再拥有该脚，其他驱动可改变电平。重新绑定 UART0 会恢复其复用，UART TX 空闲通常变高，
因此恢复 UART 前先断开灯控测试接线。仅 `unexport` 不能保证恢复 UART 复用。
这些 sysfs 操作一般不持久化，重启后由原设备树重新初始化。

## 本机测试和 SPI 参考导出

```sh
cmake -H. -Bbuild/user-host -DCMAKE_C_COMPILER=cc -DCMAKE_BUILD_TYPE=Release
cmake --build build/user-host
(cd build/user-host && ctest --output-on-failure)
./build/user-host/sm18635_gpio -o reference.bin
```

`-o` 不打开驱动，导出与 `sm18635_spi_dma` 相同的双帧 SPI 参考文件；它不是
写入 `/dev/sm18635_gpio` 的请求格式。测试校验独立的协议字段、名义高低脉宽、
ABI、运行时 GPIO 查询、参数边界、用户态只发不收/不重试错误路径，以及真实占用
判断函数的空闲/外设占用/GPIO占用/描述符缺失分支，以及自动解绑允许列表、控制台、
打开者/等待打开者保护；不模拟实际设备 remove、内核锁竞争、私有结构
ABI、IRQ 或 GPIO 电气行为。
