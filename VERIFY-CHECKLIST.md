# 真机双屏验证清单（ImmersiveWarpNative）

> 用途：在真机（双屏 / 投影仪）上逐项验证 CI 测不到的实际效果。每项完成后在 `[ ]` 打 `[x]`，
> 异常现象记在表格下方"异常记录"。数据类验证的时序证据链见 G 节（GET perf lat / perflog）。

## 准备

- [ ] 双屏（或投影仪+屏）物理连接，Windows 显示设置确认扩展模式、主副屏标识
- [ ] 备份配置：`copy %APPDATA%\ImmersiveWarpNative\config.txt config.txt.bak`
- [ ] 启动 `ImmersiveWarpNative.exe`，确认状态栏 `Projector: N` 与显示器数量一致
- [ ] 开启时序采样：CMDNET `SET perflog=1;`（或 telnet 127.0.0.1 5252），debug.log 每 10 s 一行 `perf:`
- [ ] 每组测试前 `GET perf;` 记录 baseline（fps/cap/draw/frame/lat）

## A. DXGI 引擎 · 单屏基础

- [ ] Start 后输出窗口全屏铺满目标屏（自愈：手动拖离后 1 秒内弹回）
- [ ] 桌面捕获实时：拖动桌面图标，输出画面同步（预期延迟 = lat + 扫描输出）
- [ ] F3 进/出校准模式，绿点/网格上屏
- [ ] 拖动角点，画面几何实时跟随
- [ ] F10 warp↔blend 切换，融合边可调
- [ ] Ctrl+F12 循环融合边，状态栏同步
- [ ] Ctrl+F3 snapshot：画面冻结，校准稳定（再按恢复）
- [ ] 测试图：checker 20x10 / 动画滚动带（H/V/对角）/ ROI 圆形与绿框上屏
- [ ] Pause（g_testPause）动画冻结；测试图关闭后残留清除

## B. MAG 引擎对比（Settings → Engine 下拉）

- [ ] 切 MAG：输出窗口重建成功（轮询 1.5s 起步，别在重建间隙断言）
- [ ] MAG 桌面捕获实时性肉眼对比 DXGI（分层窗口路径，预期相当或更顺）
- [ ] MAG 测试图动画滚动带与 DXGI 同构（带速、方向、颜色一致）
- [ ] MAG 模式 `GET perf` lat=0.00（已知：无 duplication 时间戳，未测量）
- [ ] 切回 DXGI：重建成功，无黑屏（duplication 释放干净）

## C. In-place 穿透（Ctrl+F9 / INPLACE）

- [ ] 全屏穿透：任务栏/图标被扭曲画面覆盖，鼠标穿透到下层
- [ ] 控制台仍可操作；穿透窗口不出现在 Alt-Tab 截屏
- [ ] Esc 退出校准正常（热键注册在 UI 线程，远程 CALIB 后按 Esc 应有效）
- [ ] 退出 in-place 后桌面恢复原样，无残留 overlay

## D. 融合带真机效果（双机重叠区）

- [ ] 2x1 layout，重叠区设 overlap（如 12.5%）
- [ ] 重叠区渐变无亮带/暗带（线性曲线 wA+wB=1 的互补性）
- [ ] Gain/Slope/Gamma 调整后视觉均匀；R/G/B 分通道各自生效（偏色校正）
- [ ] 高级融合（D 键对话框）多点曲线生效
- [ ] NxN 平铺：内部边界自动融合带、外侧边保留用户值

## E. Move 模式拖拽（校准模式 M 键）

- [ ] M 循环 Single→XY→All，状态栏提示
- [ ] Single：只动抓取点
- [ ] XY：整行+整列十字平移（等距行/列对齐场景）
- [ ] All：全画面平移
- [ ] 方向键微调与鼠标拖拽行为一致；Esc 退出后无状态残留

## F. 双屏异源

- [ ] 输出 A = 屏幕 1 捕获桌面，输出 B = 屏幕 2 捕获桌面（各自独立 duplication）
- [ ] 输出 B 源切测试图/图片（SRCSET / Images 页），A 不受影响
- [ ] 长跑 10 分钟：无帧率衰减、无 duplication 报错（debug.log 无 FATAL/DuplicateOutput 失败）
- [ ] 拔插副屏（WM_DISPLAYCHANGE）：自动重建，主屏输出不中断

## G. 时序证据链（性能数据留档）

- [ ] `GET perf;` 三次记录 fps/cap/draw/frame/lat（间隔 10s，取稳定值）
- [ ] perflog=on 状态跑 10 分钟，debug.log 取 `perf:` 行做随时间变化表
- [ ] lat 稳定值 vs 体感延迟：拖动窗口观感（lat 是渲染管线半程；+扫描输出+投影输入延迟）
- [ ] cap 在静态桌面 ≈0、活动桌面 >0（AcquireNextFrame(0) 语义验证）
- [ ] 记录表格：

| 时间 | fps | cap ms | draw ms | frame ms | lat ms | 备注 |
|------|-----|--------|---------|----------|--------|------|
|      |     |        |         |          |        |      |

## H. scanout 注入管线（DWM warp · WarpHook.dll）

> 原理：WarpHook.dll 注入本进程，挂接 D3D11 swapchain Present（vtable slot 8），
> 在应用画面上屏前的 GPU 绘制调用里追加扭曲+融合 pass。与 A-G 节的引擎自身输出窗口
> 相互独立：引擎负责生成/发布网格（IPC v3 共享内存 4 槽位），DLL 负责对**任意**
> D3D11 应用（含引擎自身）执行扭曲。先做 H.1-H.2（注入引擎自身，最安全），再试外部应用。

### H.1 注入链路（挂起启动 → DLL 附加 → vtable 补丁）

- [ ] 三件齐备同目录：`ImmersiveWarpNative.exe` + `IWNInject.exe` + `WarpHook.dll`
- [ ] 删除旧日志：`del %APPDATA%\ImmersiveWarpNative\hook.log`
- [ ] 挂起注入启动：
      `IWNInject.exe --launch ImmersiveWarpNative.exe WarpHook.dll`
      （注入器 CreateProcess(CREATE_SUSPENDED) → 注入 → ResumeThread，补丁先于任何 swapchain 创建）
- [ ] hook.log 出现 `WarpHook.dll attached pid=<N> exe=...ImmersiveWarpNative.exe`
- [ ] `cfg mapping attached (enabled=1 gridN=8)` —— IPC v3 共享内存映射成功
- [ ] `factory1 vtable patched (CreateSwapChain)`（或 factory2 CreateSwapChainForHwnd）
- [ ] 引擎 Start 后 hook.log 出现 `Present vtable patched (vt=...)` 与后续
      `first warp pass attempt (enabled=1 seen)`
- [ ] 备选路径验证：正常启动 exe 后 `IWNInject.exe --pid <pid> WarpHook.dll`，H.1 各行同样出现
- [ ] **内置注入（轮 17）**：正常启动 exe（不依赖 IWNInject.exe），CMDNET 发
      `INJECT LAUNCH notepad.exe;` → 回复 `OK INJECT launch pid=<N>`，hook.log 出现新的
      `WarpHook.dll attached pid=<N> exe=...notepad.exe`
- [ ] `INJECT PID <运行中进程pid>;` 同样回 OK 且 attach；`INJECT PID <引擎自身pid>;` 被拒
      （ERR self-injection——引擎已自扭曲，防双重 hook）
- [ ] 错误路径不伤主程序：坏 pid / 不存在 exe / `INJECT FROBNICATE` 均 ERR，控制台继续响应

### H.2 hook 状态机（CMDNET 驱动，telnet 127.0.0.1 5252）

- [ ] `GET hook;` 回读 `hook=on;outs=N;mesh=N;rev=N;ovl=off`（outs=启用的输出数，mesh>0，rev 随修改递增）
- [ ] `SET hook=off;` → GET 复读 off；引擎输出窗口画面恢复未扭曲
- [ ] `SET hook=on;` → 引擎 swapchain 内 warp pass 恢复；`SET hook=ABC;` 被拒（ERR，非数字）
- [ ] STATUS / GET config 均含 `hook=` 与 `hookovl=` 字段
- [ ] 改动网格（如 subdivide 或角点拖动）后 `rev` 递增、`mesh` 顶点数变化 —— IPC 发布链路活跃

### H.3 端到端证据（扭曲真上屏）

- [ ] hook.log 反复出现 `warp pass running, frames=N`（N 单调增长）
- [ ] hook=on 时引擎输出画面几何与校准网格一致（角点拖动 → 画面实时跟随，无需重启）
- [ ] hook=off 与 on 切换无崩溃、无 D3D 设备丢失；VEH 行（EXC）不出现于 hook.log
- [ ] 手机拍摄输出屏：扭曲画面在屏幕上（PrintScreen 截不到属预期，见已知限制）

### H.4 多输出槽位绑定（IPC v3 per-output）

- [ ] 配置 outputs=2（DISPLAY1+DISPLAY2），GET hook `outs=2`
- [ ] hook.log 每个 slot 一行 `mesh v2: rows=N cols=N verts=N slot=K mon=\\.\DISPLAYx`，
      slot 与 monitor 对账无错位
- [ ] 改一个输出的角点：仅该输出对应 swapchain 的画面变化，另一输出不动
- [ ] 禁用输出 B 后 `outs` 降为 1，槽位清理干净（无残留 mesh 应用于已移除输出）
- [ ] 外部 D3D11 应用（如自带窗口的测试程序）经 --pid 注入：其窗口画面被扭曲且绑定
      正确显示器；非目标显示器上的 swapchain 干净透传（无匹配槽位→不动画面）

### H.5 标定覆盖层（hookovl）

- [ ] `SET hookovl=on;` → 被注入画面上叠加黄色线框网格（与 warp 网格同源），状态栏同步
- [ ] 线框与实际扭曲几何重合（顶点表同源，理论必然；不重合=bug）
- [ ] `SET hookovl=off;` 恢复；`SET hookovl=maybe;` 被拒

### H.6 hooktest 固定网格模式

- [ ] `SET hooktest=on;` → DLL 使用内置固定网格（不依赖主机发布），画面呈固定扭曲
- [ ] hooktest=on 会把 warp 模式钉回 corner —— 断言 mesh 行为前先 `SET hooktest=off; SET hook=on;`
- [ ] hooktest=off 后恢复主机发布网格

### H.7 稳定性与遥测

- [ ] hook=on 长跑 10 分钟：frames 持续增长、无 EXC 行、引擎 fps 不塌（GET perf 对照 A 节 baseline）
- [ ] 目标应用正常退出（关窗口）：注入器/引擎无残留崩溃；再注入可复用
- [ ] 强杀被注入进程：无系统级残留（DWM 正常，其他应用画面正常）
- [ ] RTSS/OBS 共存冒烟：开 OBS 游戏捕获时 hook=on 不互相顶掉（同为 Present hook，
      后装者覆盖先装者属预期；重开引擎窗口恢复）

## 异常记录

（时间 / 复现步骤 / debug.log FATAL 行 / 截图路径）

## 已知限制（预期内，不算缺陷）

- MAG 引擎 lat 未测量（分层窗口无 duplication 时间戳）；R/G/B 分通道在 MAG 降级为标量 alpha
- PrintScreen 截到的是未扭曲画面（扭曲发生在 DWM 合成后、scanout 前，用户态截屏层之下）——取证用手机拍
- lat 不含扫描输出与投影仪输入延迟，真机体感延迟 = lat + 扫描 + 投影
- 注入 hook 仅覆盖 D3D11 flip-model/swapchain Present 路径；D3D9/OpenGL/Vulkan 应用不在范围
- 后装的 Present hook（如 OBS 叠加）可能覆盖 WarpHook 补丁或反之——属 hook 链固有行为，
  表现为扭曲暂时消失，重建 swapchain 或重开窗口即恢复
- RDP/远程桌面会话下 MAG 引擎 feed 恒失败（gle=0，已知）——注入管线验证请用物理控制台会话
