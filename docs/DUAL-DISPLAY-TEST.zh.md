# 双屏扩展（HdmiExtend）上板测试

设计和 trace 含义见 [DUAL-DISPLAY.md](DUAL-DISPLAY.md)，设置见 [SETTINGS.md](SETTINGS.md)。
每一步写了应该看到什么；不对时导出 trace 并拍照。

## 0. 准备

- DSI 排线两头卡扣扣紧（排线松的症状：只有背光，触摸正常）。先不插 HDMI。
- 固件不用改：现有固件照常把屏点亮在 VP0。UEFI 菜单在竖屏上正常显示，说明排线没问题。
- 驱动包：CI 的 `wddm-mali-g52-driver-arm64` artifact。
- 退路：`bcdedit /enum` 确认有救援 Windows；`pnputil /enum-drivers` 记下当前 maliwddm 的 `oemNN.inf`。
- 板子没有 RTC 电池时，装包前先 `Set-Date` 到当前时间，否则新证书无效。

## 1. 安装和设置

管理员 PowerShell：

```powershell
certutil -addstore Root wddm-mali-g52-test.cer
certutil -addstore TrustedPublisher wddm-mali-g52-test.cer
pnputil /add-driver maliwddm.inf
schtasks /run /tn malireinst      # 开机时绑定新包，不要在运行中切换适配器
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0" /v ConfigFlags   # 出现 0x20 再重启

$k = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters'
reg add $k /v HdmiHpd /t REG_DWORD /d 3 /f
reg add $k /v HdmiExtend /t REG_DWORD /d 0 /f      # 第 2 节先测 0
reg delete "$k\maliwddm" /f                         # 每轮前清空 trace
reg query "$k\maliwddm" > C:\mali\dual-<轮次>.txt   # 每轮后导出
```

## 2. 回归：HdmiExtend 0

1. 只接屏冷启动：屏上有桌面，触摸、亮度正常。trace：`Hpd_ExtendSetting 0`、`Dual_On 0`、
   `Hpd_Exclusive 1`、`Hpd_Children 2`，没有其他 `Dual_*`。
2. 插 HDMI：桌面切到 HDMI，有声音，屏背光关。
3. 拔 HDMI：约 1 秒后屏重新亮。
4. 重复 3 次都一样才进入下一节。

## 3. HdmiExtend 1，只接屏

`reg add $k /v HdmiExtend /t REG_DWORD /d 1 /f`，清 trace，不插 HDMI 冷启动。

- 和第 2 节一样出桌面，触摸、亮度、旋转正常，设置里只有一个显示器。
- trace：`Dual_On 1`、`Hpd_Exclusive 0`、`Hpd_Children 2`、`Dual_Commit 0x00FF0000`、
  `Dual_Same0 0`；不应有 `Dual_P2Step`、`Dual_ParkHold`、`Mset_*`。
- 不对时记：`Dual_IsSup`、`Dual_Commit`、`Dual_CommitStatus`、`Fail_*`。

## 4. 插 HDMI：扩展

- 约 1.5 秒后 HDMI 出现扩展桌面，屏保持点亮（可能黑一下）。鼠标和窗口能跨屏。
  设置里两个显示器：1 内置屏 800x1280，2 HDMI。系统提示音从显示器出来。
- trace：`Dual_Steps 0x43`、`Dual_P2Step 7`、`Dual_P2Status 0`、`Dual_P2Div 15`、
  `Dual_P2IfCtrl 0x8010003b`、`Dual_P2Vp0Hold 1`（`Dual_P2Hold` 0 或 1 都行）、`Mset_Status 0`、
  `Mset_Step 12`、`Aud_Status 1`、`Dual_PanelVp 2`、`Dual_Commit 0x00010002` 或 `0x01000002`。
  `Dual_Bind*`：屏是 Esmart 1、VP 2（`0x0120....`），HDMI 是 Esmart 0、VP 0。30 秒后
  `Per_Flips`、`Per2_Flips`、`Per2_Scanned` 在涨，`Per2_NotMapped`、`Per2_CopySkips` 基本不涨。
- 出问题时：
  - HDMI 有画面、屏黑（背光亮）：屏挪到 VP2 失败。记 `Dual_P2Step`、`Dual_P2Hold`、
    `Dual_P2Vp0Hold`，拍照，拔 HDMI 看屏能否回来（`Dual_Steps 0x21`）。
  - 屏正常、HDMI 黑：记 `Mset_*`、`Chk1_*`、`Per_*`，按 HDMI-HPD.md 排查。
  - 内容花屏或错位：拍照，记 `Dual_Bind*`、`Per2_Fb`。
  - 蓝屏：记停止代码，按第 9 节回退。

## 5. 分别改模式

1. 显示器 2 改 1920x1080，再改刷新率：只有 HDMI 变，`Mset_Status 0`、`Dual_Steps 0x4`。改回。
2. 显示器 1 改横向：屏转，HDMI 不变（只有源 0 能旋转）。
3. 把显示器 2 设为主显示器，再改回来。

## 6. Win+P

| 选项 | 应该看到 | trace |
|---|---|---|
| 仅电脑屏幕 | 只有屏 | `Dual_Steps 0x21`、`Dual_PanelVp 0` |
| 扩展 | 两边都有 | `Dual_Steps 0x43` |
| 仅第二屏幕 | 只有 HDMI，屏背光关 | `Dual_Steps 0x416`、`Dual_PanelVp 15` |
| 扩展 | 两边都有 | `Dual_Steps 0x453` |
| 复制 | 不支持：保持原样或回到扩展 | `Dual_IsSup` 第 4 到 7 位为 1 |

每次等 5 秒，确认触摸和鼠标可用。

## 7. 热插拔 5 次

扩展状态下拔 HDMI（`Dual_Steps 0x21`，屏回到 VP0，窗口还在），再插回（`Dual_Steps 0x43`）。
做 5 次，每次记成败；失败一次就导出 trace，标明第几次。

## 8. 冷启动、重启、关机

两个都接着冷启动 3 次：启动 logo 在屏上，进系统后扩展。再测一次重启和一次关机。

## 9. 回退（从轻到重）

1. 系统能进：`reg add $k /v HdmiExtend /t REG_DWORD /d 0 /f`，重启。
2. 进不去或黑屏：启动菜单选救援 Windows（或在主系统 `bcdedit /bootsequence <救援 guid>`）。在救援系统里（主系统是 D:）：
   ```powershell
   reg load HKLM\X D:\Windows\System32\config\SYSTEM
   reg add "HKLM\X\ControlSet001\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiExtend /t REG_DWORD /d 0 /f
   reg unload HKLM\X
   ```
3. 换回旧驱动：`pnputil /add-driver` 装回第 0 节记下的包，或在救援系统
   `dism /Image:D:\ /Remove-Driver /Driver:oemNN.inf`，再按第 1 节 `malireinst` 重启绑定。
4. 拔掉 HDMI 总是安全的：屏回到 VP0，硬件状态和没有 HdmiExtend 时一样。

## 10. 发回

每节的 `C:\mali\dual-<轮次>.txt`；第 4、6、7 节屏和显示器同框的照片；设置 > 屏幕截图；
第 7 节成败记录；蓝屏时的停止代码和 `C:\Windows\Minidump` 里的文件。
