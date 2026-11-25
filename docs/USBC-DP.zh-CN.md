# USB-C DisplayPort 输出（UsbcDp）

[English](USBC-DP.md) | 简体中文

分支 `usbc-dp`，配合 edk2-rk3576 的 `usbc-dp` 分支。RK3576 的 DisplayPort 控制器通过 USB-C 口（DisplayPort Alt Mode）驱动一台显示器，在 Windows 11 23H2 下作为 HDMI 旁边的第二块屏。两边默认都关闭。**都还没有在板子上跑过。**

代码：`drivers/maliwddm/dp_calc.h`（算术部分，由 `tests/dp-host` 检查）、`dp.c`（硬件操作）、`hpd.c`（设置里的 `UsbcDp`、子设备、VidPN、`DpDualCommit`、`DpPoll`）、`scanout.c`（把 DP 的帧缓冲加进 VOP2 IOMMU）。固件部分见 edk2-rk3576 的 `docs/usbc-dp.md`。

## 用户能得到什么

- **开机前**插好的 USB-C 显示器、带 DP 或 HDMI 输出的 USB-C 扩展坞、USB-C 转 DP/HDMI 转接头，会作为 HDMI 旁边的扩展桌面出现。Windows 可以按它的 EDID 设置分辨率和刷新率，最高 2560x1600 60 Hz。
- 显示器关了再开，或者转接头后面的线拔了再插，大约两秒内会重新被认出来（驱动通过 AUX 询问接收端，见下面的"HPD"）。
- 暂时还不行：开机后再插 USB-C 线；通过这个口给板子充电的显示器；DSI 启动（泰山派的屏）时同时用 DP；DP 音频；固件开机画面显示在 DP 上。

## 板子

| 板子 | USB-C 是否接了 DP | 依据 |
|---|---|---|
| 泰山派 3M | 是 | 立创 EDA 原理图 v1.4：`DP_TX_AUXP/N` 从 SoC 接到插座的 SBU1/SBU2；`TYPEC_DPTX_AUX_PUPDCTL1` 在 AL3 脚 = GPIO4_C4，`..._PUPDCTL2` 在 AK1 脚 = GPIO4_C5；四对 SuperSpeed 线（`USB3_OTG0_SSTX1/2`、`SSRX1/2`）都接到插座；FUSB302 U40 的 VCONN 脚 12/13 接 VCCA_3V3_S0 |
| ArmSoM CM5-IO | 是 | 底板原理图 V1.1：`TYPEC_AUXP/N` 在 SBU1/SBU2；主线 DTS：FUSB302 带 `altmodes { displayport { svid = 0xff01 } }`、`sbu1-dc-gpios = GPIO4_C4`、`sbu2-dc-gpios = GPIO4_C5`，USBDP PHY 作为方向开关和 DP alt mode 复用器 |
| ROCK 4D | 否 | 它的 USB-C 口只供电 |

两块板子用的是同样的引脚、同样的 PHY 和同样的 Type-C 控制器，所以固件代码是共用的；只有 FUSB302 所在的 I2C 总线（泰山派 I2C2，CM5-IO I2C0）和 VBUS 开关（GPIO0_B6、GPIO2_B6）不同。

## 硬件

```
FUSB302 (I2C) --- CC ---+
                        USB-C 插座
USBDP PHY pad 0..3 -----+  SS 线对   （两条 DP lane + USB3，或四条 DP lane）
USBDP PHY AUX ----------+  SBU1/SBU2 （按插入方向由 GPIO4_C4/C5 偏置）
     ^ lane、AUX 极性、HPD：VO1_GRF_SOC_CON0
DW DP 控制器 DP0 0x27E40000 (PD_VO1) <-- VOP2 VP1 (DP0_IF_CTRL 0x18C)
```

- **DP 控制器**：Synopsys DW DP，主线里的 `rockchip,rk3576-dp`（`drivers/gpu/drm/bridge/synopsys/dw-dp.c`、`rockchip/dw_dp-rockchip.c`），RK3576 上是 dual pixel 模式，每条 lane 最高 8.1 Gb/s。在 PD_VO1 里；时钟为 pclk_dp0、clk_aux16mhz_0（GPLL / 74）、aclk_dp0。
- **PHY**：DRD0 后面的 USB/DP 组合 PHY（`drivers/phy/rockchip/phy-rockchip-usbdp.c`）。固件本来就在 ExitBootServices 时初始化它的 USB3 部分，两个 pad 复用给 USB、两个给 DP（主线默认的 Type-C 映射），所以加 DP 时不用复位 USB3。
- **VOP2**：任何一个 port 都能驱动 DP0。VP0 归 HDMI；HdmiExtend 下 VP2 归泰山派的屏；DP 用 **VP1**（最大 2560x1600，vendor 内核里 dclk 上限 300 MHz）、Esmart1（AXI 总线 0，已有的 scan-out IOMMU 设置覆盖它），dclk_vp1 来自 **VPLL**，别的东西都不用它（VP0 用 HDMI PHY 的 PLL，VP2 用 CPLL）。
- **HPD**：USB-C 上没有 HPD 线。对方通过 PD 消息（DP Status、Attention）报告 HPD。RK3576 上，只要 PHY 驱动这么设置，控制器的 HPD 输入就来自 VO1_GRF_SOC_CON0 的第 10/11 位。

## Windows 下由谁协商 Alt Mode：决定

考虑过的方案（来源见加入本文件的提交和 edk2 文档）：

| 方案 | 需要什么 | 结论 |
|---|---|---|
| UCSI（`UcmUcsiCx` + `UcmUcsiAcpiClient`） | 一个 PPM：在 EC 上跑 PD 和 alt mode 的固件。我们没有 EC；放在另一个 Windows 驱动里的 PPM 照样要装下整套 PD 协议栈，而且 UCSI 仍然没有把 HPD 交给显示驱动的通路 | 否 |
| `UcmTcpciCx` 客户端 | 微软的软件 PD 策略引擎，带 DisplayPort alt mode 事件（`IOCTL_UCMTCPCI_PORT_CONTROLLER_DISPLAYPORT_CONFIGURED`、`..._HPD_STATUS_CHANGED`），接口按 TCPCI 设计。FUSB302 不是 TCPCI（寄存器表和 FIFO token 都是自己的；Linux 也把 `fusb302.c` 和 `tcpci.c` 分开），所以客户端要把 TCPCI 语义映射到它上面。需要 woa-rk3576 的 I2C（`rk3xi2c`）和 GPIO（`rk3xgpio`）驱动以及 FUSB302 的 ACPI 设备；泰山派上固件的 RTC 运行时服务也在用 I2C2 | 做**热插拔**的正路；第二阶段 |
| 自己写 KMDF 驱动（UcmCx 客户端或普通驱动） | 在 Windows 里自带 PD 和 alt mode 协议栈 | 代价等于固件那份再加上 Windows 的各种接口；只有在 23H2 arm64 上 UcmTcpciCx 不存在或不能用时才考虑 |
| **固件在开机时进入 Alt Mode** | 在 UEFI 里、ExitBootServices 时做一次 PD 合约和 VDM；Windows 看到的是一个线上已经有接收端的 DP 控制器 | **第一阶段选这个**：实现"开机前插好就有画面"最省事的路 |

固件方案在开机之后为什么还能用：

- 合约和模式建立后，PD 不需要周期性的消息；对方发来的东西 FUSB302 会继续确认（自动 GoodCRC）。
- 只要对方接受了配置，固件就把 HPD 强制拉高，本驱动改为通过 AUX 检查接收端是否存在（`MwDpSinkThere`：DPCD 有应答，而且转接头这类分支设备报告后面有接收端）。显示器关了再开、转接头另一头重新插拔，都能覆盖到。

它做不到的事，也就是需要第二阶段的原因：

- 开机后插上的线拿不到 VBUS、PD 和 alt mode。
- 对方需要应答的请求（新的 Request、Soft_Reset）没人回；对方随后会发 Hard Reset，alt mode 就断了。大多数显示器和转接头在 Configure 之后就不再说话；扩展坞不一定。

## 两边怎么衔接

固件和驱动之间除了硬件状态，什么都不传：

| 固件留下的 | 驱动怎么读 |
|---|---|
| PD_VO1 打开，pclk_vo1_root 和 pclk_dp0 不门控 | `Powered()`；否则驱动绝不碰 DP0 和 VO1 GRF（访问会造成同步外部中止） |
| PMA `0x288` 低 4 位：两条 DP lane 的使能 | `MwDpLanePads()` 结合 GRF 的 lane 选择：哪个 PHY pad 承载 DP lane 0 和 1（正插 2 和 3；反插 0 和 1） |
| PMA `0x38C` 第 3 位 DP_INIT_RSTN | PHY 的 DP 部分已经出了初始化复位 |
| VO1_GRF_SOC_CON0 第 10/11 位置位 | 已进入 alt mode；否则 `Dp_Off` 为 3 |

## 驱动做了什么

启动（`MwDpStart`，HDMI 启动且 `HdmiHpd` 为 3 时由 `MwHpdStart` 调用）：上面的检查、关掉 `DEFAULT_FAST_LINK_TRAIN_EN`、读 DPCD 0x000..0x00F（以及 0x2200 的扩展能力）、通过 I2C-over-AUX 读 EDID（最多四块）、生成模式。如果接收端有应答，就报告两个 VidPN source 和两个子设备：子设备 0 是 HDMI，子设备 1 是 DisplayPort（`D3DKMDT_VOT_DISPLAYPORT_EXTERNAL`）。

设置模式（`MwDpModeSet`，由 `DpDualCommit` 调用）：

1. 关视频流，DP0 离开它的 port，VP1 进入 standby（DSP_HOLD_VALID）。
2. VPLL 设为像素时钟的偶数倍（`MwVpllFor`，要么精确，要么不提供这个模式），dclk_vp1_src = VPLL / Div。
3. 叠加层（Esmart1 作为 VP1 的第 2 层、三个混合器、BG_DLY 16），Esmart1 指向 DP 自己的帧缓冲。
4. 在 STANDBY 置位的情况下写 VP1 时序，VP_DSP_CTRL 整个只写一次（AAAA）；DP0_IF_CTRL 写 MUX 1、PCLK_DIV、极性，不写 EN。
5. 链路：接收端进 D0，设置 PHY 的链路速率（DP_CMN_RSTN、TX_LINK_BW、ROPLL 锁定），两条 lane，时钟恢复（TPS1）和通道均衡（TPS4、TPS3 或 TPS2），把接收端要求的摆幅和预加重写进 PHY 的驱动寄存器；速率按 HBR2、HBR、RBR 逐级下降。
6. 视频：时序、MSA、TU 和初始阈值（dual pixel）、HBLANK 间隔。
7. VP1 退出 standby，DP0_IF_CTRL 置 EN，打开视频流；如果 TU 或像素时钟不对，`Dp_GenInt` 会显示视频 FIFO 溢出。

速率：最高 HBR2（5.4 Gb/s）。两条 HBR2 lane 是 8.64 Gb/s，超过 VP1 最大模式的需要（2560x1600 60 Hz 缩减消隐：268.5 MHz，6.44 Gb/s）。lane：不管引脚分配是什么，总是两条；为什么优先 D、以及 C 为什么同样可行，见 edk2 文档。

## 设置

`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters`：

| `UsbcDp` | 作用 |
|---|---|
| 0 或不存在 | 关闭。不映射任何寄存器、不预留内存，驱动和以前一样。 |
| 1 | 启动时，在 HDMI 启动且 `HdmiHpd` 为 3（HDMI 的默认值）的情况下：预留 DP 的帧缓冲（16 MiB，VP1 最大模式），如果固件进入了 alt mode 且接收端有应答，DP 就是第二块屏。DSI 启动和 `HdmiExtend` 为 1 时忽略。 |

固件那一半是设置选项"USB-C DisplayPort (experimental)"（设备管理器，RK3576 设置）。两边都要打开。

## 跟踪值

在 `Device Parameters\maliwddm` 下：

| 名称 | 含义 |
|---|---|
| `Dp_Setting`、`Dp_On`、`Dp_Connected` | 设置值；是否报告了 DP；接收端是否存在 |
| `Dp_Off` | 没开的原因：1 PD_VO1 关着或时钟门控，2 映射失败，3 固件没有进 alt mode（lane、HPD 位、DP_INIT_RSTN），4 单 lane 接收端 |
| `Dp_Fw` | PMA 0x288 << 16 \| VO1_GRF_SOC_CON0 低 16 位 |
| `Dp_Pads` | 找到的 lane 数 \| lane 0 的 pad << 8 \| lane 1 的 pad << 16 |
| `Dp_HpdStatus` | 控制器的 HPD_STATUS（bit 11:9 为 7 表示已插入） |
| `Dp_CapsStatus`、`Dp_Caps0`、`Dp_Caps5` | DPCD 读取状态；第 0..3 字节；第 5、6、0x0E 字节 |
| `Dp_EdidSize`、`Dp_EdidStatus`、`Dp_Modes`、`Dp_Mode0` | EDID 和由它得到的模式 |
| `Dp_Commits`、`Dp_Commit`、`Dp_CommitStatus`、`Dp_ModeSet` | commit 次数（source 0 的目标 << 24 \| source 1 的 << 16） |
| `Dp_Step` | 模式设置进行到哪一步：1 standby、2 时钟、3 窗口、4 时序、5 链路、6 视频、7 放开 |
| `Dp_Vpll`、`Dp_VpllLock`、`Dp_Hold` | m \| p << 10 \| s << 16 \| div << 20；CON6；VP1 是否进了 standby |
| `Dp_TrainStep`、`Dp_TrainRate`、`Dp_TrainStatus`、`Dp_TrainDrive`、`Dp_Ropll` | 训练：1 配置 2 CR 3 EQ 4 完成；速率（MHz）\| 状态 << 16；DPCD 0x202..0x205；每条 lane 的 vs/pe；失败时 ROPLL 的锁定位 |
| `Dp_Video5`、`Dp_IfCtrl`、`Dp_GenInt`、`Dp_Status` | VIDEO_CONFIG5；DP0_IF_CTRL；40 ms 后的 GENERAL_INTERRUPT（bit 6 = FIFO 溢出）；结果 |
| `Dp_Changes`、`Dp_Indicate`、`Dp_ParkHold` | 接收端出现或消失；DxgkCbIndicateChildStatus；停用时 VP1 是否进了 standby |

## 规模

| 部分 | 行数 |
|---|---|
| `dp_calc.h`（AUX、能力、训练规则、驱动表、VPLL、视频、模式过滤） | 约 600 |
| `dp.c`（AUX、EDID、PHY 速率和驱动强度、训练、VP1、停用） | 约 700 |
| `hpd.c` 和 `scanout.c` 的集成（设置、预留、子设备、`DpDualCommit`、`DpPoll`、帧缓冲种类） | 约 350 |
| `tests/dp-host` | 约 250 |

固件部分（edk2）：PD 和 alt mode 约 760 行，PHY/时钟/电源约 150 行，选项和板级约 200 行。

第二阶段（热插拔，基于 `rk3xi2c` 和 `rk3xgpio` 的 UcmTcpciCx 客户端）：估计 1500 到 2500 行外加 ACPI，而且要先解决泰山派上 I2C2 和 RTC 运行时服务共用的问题。

## 板上测试计划

需要：一块泰山派 3M 或 CM5-IO，HDMI 接一台显示器；USB-C 上依次接第二台显示器：(1) USB-C 转 HDMI 或 DP 的转接线，(2) 一台带 USB-C 输入、不给对方充电的显示器，(3) 一个带 DP 或 HDMI 输出的扩展坞；串口。固件和驱动用 `usbc-dp` 分支的 CI 产物；打开固件选项，`UsbcDp` 设为 1，重启。

1. **只测固件**（选项开，驱动设置为 0），每种对端设备、两个插入方向，各至少 3 次：串口显示 `DP alt mode: configured`，`pin D`（扩展坞）或 `pin C`（转接头），`UsbDpPhy DP: PD_VO1 on, lanes 0x..`。扩展坞上的 USB3 仍然是 SuperSpeed（设备管理器）。不接对端、以及接 USB 硬盘时，启动时间不变。
2. **驱动**：`Dp_On` 为 1，`Dp_Caps0` 合理，`Dp_Modes` 大于 2，Windows 显示两块屏；扩展能用；在 DP 屏上分别设 1920x1080、2560x1440、1280x720；每次 `Dp_TrainStep` 为 4，`Dp_GenInt` 第 6 位为 0。
3. USB-C 插头**两个方向**都测。
4. **显示器关了再开**，转接头的 HDMI 线拔了再插：`Dp_Changes` 增加，画面回来。
5. **HDMI 不受影响**：`UsbcDp` 为 0 时，注册表跟踪值和 HDMI 的行为（改模式、热插拔）都和以前一样。
6. **关机和重启**干净（没有 WHEA：之前 XHC0 的 SError 就出在这个 PHY 的 USB3 部分）。

## 风险

- 所有寄存器序列都还没跑过。最没把握的几处：在 USB3 初始化里放开 DP_INIT_RSTN（主线在 DP+USB 模式下也在同一位置这么做）、由固件打开 PD_VO1、Windows 下 VPLL 是否真的空闲、dual pixel 的 TU 值。
- 引脚分配 C 只用两条 lane：对方的 lane 2 和 3 收到的是 USB3 发送端的信号。它们不参与训练（lane 数为 2），但坚持要四条 lane 的接收端不会出画面。
- 扩展坞如果在 Configure 之后发需要应答的 PD 消息，在 Windows 下就会复位 alt mode（第一阶段没有端口驱动）。
- DP 的模式列表由 `MwHdmiModes` 生成，所以 HDMI PHY 做不出的模式在 DP 列表里也没有（实际上很少）。
- HdmiExtend 下 Esmart1 是屏的窗口；这两个功能互斥。
