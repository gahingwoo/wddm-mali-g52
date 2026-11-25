# 双屏扩展（HdmiExtend）上板测试计划

分支 `dual-display`，设计说明见 [DUAL-DISPLAY.md](DUAL-DISPLAY.md)。代码没有在板子上跑过
（DSI 排线坏了），等新排线到了按本文逐步测。每一步都写了"应该看到什么"和"不对时记录什么"。

## 0. 测之前

**硬件**
- 断电后换上新的 DSI 排线，确认两头卡扣都扣紧（排线松了的症状是"只有背光、触摸正常"，见
  feedback_check_dsi_ribbon_first）。
- 先不插 HDMI。显示器用之前那台 AOC 2560x1440。

**固件**：不需要改。eMMC 上现有的 0.4.x 固件照常把屏点亮在 VP0，双屏全部由驱动完成。
开机时确认 UEFI 菜单在竖屏上正常显示，说明排线没问题。

**驱动包**：GitHub Actions 里 `dual-display` 分支最新一次 CI 的
`wddm-mali-g52-driver-arm64` artifact（整包，已测试签名）。

**退路先准备好**
1. 确认 BCD 里还有 "Windows 11 Rescue"：`bcdedit /enum`。
2. 记下当前在用的驱动版本：`pnputil /enum-drivers | findstr /i maliwddm -A 3`，以及对应的 `oemNN.inf`。
3. 板子没有 RTC 电池，装包前先 `Set-Date` 到当前时间，否则新证书会被判无效。

## 1. 安装

在主系统（DESKTOP-054OHRR）管理员 PowerShell 里：

```powershell
# 每个 CI 包的测试证书都是新的
certutil -addstore Root wddm-mali-g52-test.cer
certutil -addstore TrustedPublisher wddm-mali-g52-test.cer
pnputil /add-driver maliwddm.inf
# 开机时绑定新包（不要在运行中切换适配器，dwm 会卡死）
schtasks /run /tn malireinst
# 轮询 ConfigFlags 里出现 0x20 再重启，不要相信旧的 reinst-out.txt
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0" /v ConfigFlags
```

设置（都在 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters`）：

```powershell
$k = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters'
reg add $k /v HdmiHpd /t REG_DWORD /d 3 /f
reg add $k /v HdmiExtend /t REG_DWORD /d 0 /f      # 第 2 节先测 0
reg delete "$k\maliwddm" /f                         # 每轮测试前清空 trace
```

每轮结束后导出 trace：

```powershell
reg query "$k\maliwddm" > C:\mali\dual-<轮次>.txt
```

## 2. 回归：HdmiExtend 0，单屏行为必须和 main 完全一样

`HdmiExtend` 为 0（或不存在）时，新驱动应当和 main（11c6be0）在同样设置下一模一样。

1. 只接屏，冷启动。
   - 应该看到：屏上 LogonUI 和桌面，触摸、亮度正常。
   - trace：`Hpd_ExtendSetting 0`，`Dual_On 0`，`Hpd_Exclusive 1`，`Hpd_Children 2`，没有任何 `Dual_*` 其他值。
2. 插 HDMI。
   - 应该看到：和以前一样，桌面切到 HDMI（`HdmiExclusive` 1 的行为），有声音；屏背光关。
3. 拔 HDMI。
   - 应该看到：约 1 秒后屏重新亮起。
4. 以上三步重复 3 次，每次都一样才算通过。任何一次不同，停在这里，不要进入下一节，把 trace 发回来。

## 3. HdmiExtend 1，只接屏

```powershell
reg add $k /v HdmiExtend /t REG_DWORD /d 1 /f
reg delete "$k\maliwddm" /f
```

不插 HDMI，冷启动。

- 应该看到：屏上的启动过程、LogonUI、桌面都和第 2 节一样；触摸、亮度、旋转正常。
- trace：
  - `Hpd_ExtendSetting 1`，`Dual_On 1`，`Hpd_Exclusive 0`，`Hpd_Children 2`
  - `Dual_Commit` = `0x00FF0000`（源 0 接屏，源 1 无，屏在 VP0 前后都是 VP0）
  - `Dual_Same0 0`（第一次提交什么寄存器都不写）
  - **不应该**出现 `Dual_P2Step`、`Dual_ParkHold`、`Mset_*`
- 设置 > 系统 > 屏幕：只有一个显示器。
- 不对时：记下桌面是否出来、`Dual_IsSup`、`Dual_Commit`、`Dual_CommitStatus`、`Fail_*`。

## 4. 插上 HDMI：扩展

系统起来后插 HDMI 线。

- 应该看到：约 1.5 秒后 HDMI 显示器出现桌面的扩展部分（默认是"扩展"），**屏保持点亮**，
  中间可能黑一下（屏从 VP0 挪到 VP2）。鼠标能从屏移到显示器，窗口能拖过去。
- 设置 > 系统 > 屏幕：两个显示器，1 为内置屏（800x1280），2 为 HDMI（2560x1440）。
- trace：
  - `Dual_Steps 0x43`（先"屏到 VP2"，再"HDMI 设模式"）
  - `Dual_P2Step 7`，`Dual_P2Status 0`，`Dual_P2Div 15`，`Dual_P2IfCtrl 0x8010003b`
  - `Dual_P2Hold`（VP2 从没跑过，0 或 1 都可以），`Dual_P2Vp0Hold 1`
  - `Mset_Status 0`，`Mset_Step 12`，`Aud_Status 1`
  - `Dual_PanelVp 2`，`Dual_Commit` 为 `0x00010002` 或 `0x01000002`（谁接哪个都行）
  - `Dual_Bind0` / `Dual_Bind1`：接屏的那个应该是 Esmart 1、VP 2（`0x0120....`），接 HDMI 的是 Esmart 0、VP 0（`0x1000....`）
  - 等 30 秒后看 `Per_Flips`、`Per2_Flips`、`Per2_Scanned` 都在涨，`Per2_NotMapped`、`Per2_CopySkips` 基本不涨
- HDMI 声音：播放一个系统提示音，从显示器喇叭出来。
- **最可能出问题的地方和对应要记的东西**：
  - HDMI 有画面但屏黑了（背光亮）：屏挪到 VP2 失败。记 `Dual_P2Step`（停在几）、`Dual_P2Hold`、
    `Dual_P2Vp0Hold`，拍照。然后拔掉 HDMI，看屏能不能回来（`Dual_Steps 0x21`）。
  - 屏正常但 HDMI 黑：记 `Mset_*`、`Chk1_*`、`Per_*`，和以前 HDMI 黑屏的排查一样（HDMI-HPD.md）。
  - 屏上有画面但内容不对（花屏、错位、只有一部分）：拍照，记 `Dual_Bind*`、`Per2_Fb`。
  - 蓝屏：记下停止代码，重启进救援系统（第 9 节）。

## 5. 两边分别改模式

1. 设置 > 屏幕 > 选显示器 2，把分辨率改成 1920x1080，再改刷新率（高级显示里 60/75 Hz）。
   - 应该看到：只有 HDMI 变，屏不闪。trace 里新的 `Mset_Status 0`，`Dual_Steps 0x4`（只有 HDMI 一步）。
2. 改回 2560x1440。
3. 选显示器 1（屏），改方向为横向。
   - 应该看到：屏转过来，HDMI 不受影响。（只有源 0 能旋转；如果屏在源 1 上，Windows 不会给旋转选项，这是已知限制，记下来即可。）
4. 把显示器 2 设为主显示器，再改回来。

## 6. Win+P 四个选项

依次按 Win+P 选：

| 选项 | 应该看到 | trace |
|---|---|---|
| 仅电脑屏幕 | 只有屏，HDMI 无信号 | `Dual_Steps 0x21`，`Dual_PanelVp 0` |
| 扩展 | 两边都有 | `Dual_Steps 0x43` |
| 仅第二屏幕 | 只有 HDMI，屏背光关 | `Dual_Steps 0x416`，`Dual_PanelVp 15` |
| 扩展 | 两边都有 | `Dual_Steps 0x453`（屏到 VP2，背光开，HDMI 不变） |
| 复制 | 预期不支持：保持原样或回到扩展 | `Dual_IsSup` 第 4 到 7 位是 1（clone 被拒） |

每个选项之后等 5 秒，确认触摸、鼠标还能用。

## 7. 热插拔循环

在扩展状态下：拔 HDMI，等屏单独显示桌面（约 1 到 2 秒）；再插回，等扩展恢复。**做 5 次**
（这块板子以前出现过约 25% 的偶发失败，两次成功不能说明问题）。

- 拔：`Dual_Steps 0x21`，屏回到 VP0，屏上窗口还在。
- 插：`Dual_Steps 0x43`。
- 每次都记一下是否成功；有一次失败就把 trace 导出，标明第几次。

## 8. 冷启动两个都接着，重启和关机

1. 关机，屏和 HDMI 都接着，冷启动。
   - 应该看到：启动 logo 在屏上（固件选 DSI），进系统后直接扩展。`Dual_Commit` 一开始可能是只有屏，随后变成扩展。
2. 开始菜单 > 重启：能正常重启，回来还是扩展。
3. 关机：能正常关机（以前出现过一次关机最后卡住，n=1，注意观察）。
4. 冷启动做 3 次。

## 9. 回退

按需要选一种，从轻到重：

1. **只关掉双屏**（系统还能进）：
   ```powershell
   reg add $k /v HdmiExtend /t REG_DWORD /d 0 /f
   ```
   重启后就是第 2 节的单屏行为（和 main 相同）。
2. **系统进不去或黑屏**：开机在固件画的启动菜单（屏上可见）里选 "Windows 11 Rescue"，或者从主系统
   `bcdedit /bootsequence <rescue guid>`（guid 在 `C:\drvold\rescue-guid.txt`）。在救援系统里（主系统是 D:）：
   ```powershell
   reg load HKLM\X D:\Windows\System32\config\SYSTEM
   reg add "HKLM\X\ControlSet001\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiExtend /t REG_DWORD /d 0 /f
   reg unload HKLM\X
   ```
   重启回主系统。
3. **回到 main 的驱动**：`pnputil /add-driver` 装回第 0 节记下的那个包（或用救援系统
   `dism /Image:D:\ /Remove-Driver /Driver:oemNN.inf` 删掉新包），再按第 1 节的 `malireinst` 流程重启绑定。
4. 拔掉 HDMI 线永远是安全的：拔掉后屏回到 VP0，硬件状态和没有 HdmiExtend 时一样。

## 10. 要发回来的东西

- 每一节的 `C:\mali\dual-<轮次>.txt`（完整的 `maliwddm` trace）。
- 第 4、6、7 节的照片（屏和显示器同框）。
- 设置 > 屏幕页面的截图。
- 第 7 节每次插拔的成功/失败记录。
- 如有蓝屏：停止代码和 `C:\Windows\Minidump` 里的文件（如果有）。
