# P2b · IWNPPL.sys 内核辅助驱动（dwm 去 PPL）

## 背景定性（轮 29，2026-10-26）

- IFEO 快路径（P2a）实测无效：dwm.exe 是 **WinTcb 级 PPL 受保护进程**，重启后（CFG 已关）注入仍失败——`LoadLibraryW gle=5` + `ReadProcessMemory gle=5`（连读内存都被拒）。
- IFEO MitigationOptions 管不了 PPL 保护级别；只有内核能清。
- 本驱动 **不是显示/GPU 驱动**：只在 dwm.exe 创建瞬间把 EPROCESS 的 PS_PROTECTION 字节清零，让桌面合成器以普通进程出生，用户态 manual map 管线随后直接可用。

## 工作原理

- `PsSetCreateProcessNotifyRoutineEx` 捕获 dwm.exe 创建 → 初始化前清保护字节（出生即无 PPL）。
- 偏移无关：内核 `ZwQueryInformationProcess(ProcessProtectionInformation=61)` 读出当前保护值，再在 EPROCESS 前 0x900 字节内做唯一匹配扫描清零；歧义时回退 1909 已知偏移 0x6CA。全程 DbgPrint 打点（DbgView 可看）。

## 目标机加载步骤（管理员）

```bat
REM 1. 开测试签名（一次性，重启生效；桌面右下角会出现"测试模式"水印）
bcdedit /set testsigning on
REM 2. 信任测试证书（root + TrustedPublisher）
certutil -addstore root IWNTestSigning.cer
certutil -addstore TrustedPublisher IWNTestSigning.cer
REM 3. 重启
shutdown /r /t 0
```

重启后（管理员）：

```bat
sc create IWNPPL type= kernel binPath= C:\path\to\IWNPPL.sys
sc start IWNPPL
```

然后管理员运行 ImmersiveWarpNative.exe → Displays 页 Enable Desktop warping (DWM hook)。
（如 dwm 已经在跑而驱动是后启动的：`sc stop` 后先重启一次 dwm——注销重登或重启机器——回调会在 dwm 新实例出生时清保护。）

卸载：

```bat
sc stop IWNPPL
sc delete IWNPPL
bcdedit /set testsigning off   REM 恢复正常签名策略（可选）
```

## 构建

CI（GitHub Actions `driver` job）：WDK NuGet 包（26100.6584）+ runner MSVC，`driver/build-driver.ps1` 编译链接（/kernel /GS- /SUBSYSTEM:NATIVE /ENTRY:DriverEntry），`driver/sign-driver.ps1` 自签名证书测试签名。产物：`IWNPPL.sys` + `IWNTestSigning.cer`（artifact `IWNPPL-driver` + latest release assets）。

## 安全边界

- 驱动无 IOCTL、无持久化，加载即挂进程回调，卸载即移除；只动 dwm.exe 的保护字节，不改其它任何内存。
- 测试签名模式 + 自签证书仅本机有效；随时 `bcdedit /set testsigning off` 回滚。
