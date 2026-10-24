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

## 异常记录

（时间 / 复现步骤 / debug.log FATAL 行 / 截图路径）

## 已知限制（预期内，不算缺陷）

- MAG 引擎 lat 未测量（分层窗口无 duplication 时间戳）；R/G/B 分通道在 MAG 降级为标量 alpha
- PrintScreen 截到的是未扭曲画面（扭曲发生在 DWM 合成后、scanout 前，用户态截屏层之下）——取证用手机拍
- lat 不含扫描输出与投影仪输入延迟，真机体感延迟 = lat + 扫描 + 投影
