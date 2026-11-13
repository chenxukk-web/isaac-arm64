# PROJECT.md —— 这个移植是什么，不是什么

这是帖子正文的长版本。它存在的理由：Discord 消息必须短，而短消息装不下这个
项目需要的那些限定条件。

**动手构建之前先读这个。** 三件事决定它对你有没有用：

1. **它不是 mod，也不是 APK mod 包。** 它替换的是引擎，不是内容。
   `resources/packed/` 下的东西一个字节都没动。
2. **包里没有 APK，也没有 `.so`。** 只有源码和文档。
3. **没有任何记录的运行走到结局。** 还有两类指令没实现。见 §5。

---

## 1. 它到底是什么

《以撒的结合：忏悔》发布时是 32 位 x86 Windows PE。本项目把那个可执行文件
**静态重编译**：

```
isaac-ng.exe (v1.7.9b, J835)
  -> 脱掉 SteamStub
  -> 15,071 个函数翻译成 C（4,669,714 行，192 个单元）
  -> 镜像基址平移 0x30000000 -> 0x3C000000
  -> 交叉编译到 arm64
  -> 链接成 libisaac.so
  -> 打包成 APK
```

不是模拟器。热路径上没有解释器。没有 DLL 翻译层。生成的 C 就是游戏自己的机器码，
只是重新组织过。

**上游：** 本项目建立在 [`repentogxm`](https://github.com/0xl0cal/repentogxm)
之上——0xl0cal 的 PlayStation Vita 移植版，GPL-2.0。那个项目提供重编译器
（`gen_all.py`）和运行时（`recomp/runtime`：`guest.c`、`gl_bridge.c`、
`kage_pc_backend.*`）。本项目加的是**Android 宿主层**，以及它需要的
GL/EGL/音频/输入绑定。

想理解“重编译到底怎么做的”，先读他们的仓库。本项目是它上面的一层。

**快照说明。** 本分支取自 **2026-11-01**。此后上游发布了 `v0.1.2-alpha`
（2026-11-06）和 `v0.2.0-alpha`（2026-11-13）。这两个版本几乎全是
`host_vita_*` 与 `gl_vita_backend*` 的改动——Vita 专用，对 Android + gl4es
的宿主层不适用。不要假定他们的缺陷清单和我们的相同：他们修好了结局播放和中文
字体图集，而那两个修复都不在本项目用到的文件里。要从上游拿东西之前，先重新
diff `recomp/runtime`。

---

## 2. 在上游基础上加了什么

| 层 | 文件 | 作用 |
|---|---|---|
| CRT / 堆 | `host_crt*.c` | MSVC 运行时导入、低 4GB guest 堆 |
| Win32 | `host_user32.c`、`host_win_sys.c` | 59 个 USER32 导入、窗口账本 |
| 图形 | `host_gl4es.c`、`gl4es_bridge_gen.c` | 经 gl4es 把 GL 2.1 转 GLES 2.0 |
| 音频 | `host_gl_audio.c`、`host_openal.c` | DirectSound / OpenAL Soft |
| 输入 | `host_dinput.c`、`host_xinput.c` | DirectInput8 + XInput，假设备 |
| Lua | `host_lua.c`、`host_lua_tramp.inc` | 67 个 Lua 5.3.3 导出 |
| Steam | `host_steam.c` | Steamworks 桩 |
| JNI | `jni_bridge.c` | APK 用的 8 个 JNI 入口 |

15,071 个翻译函数在 `gen/` 里，**不在本包中**（213 MB，而且可以用生成器加你自己
的 PE 重新生成）。

---

## 3. 架构，一段话

guest 地址是 `uint32_t`。所以整个 guest 地址空间被映射在宿主进程的低 4GB 内，
`ld32()` 字面上就是 `*(uint32_t*)addr`。落到 Windows API 的调用进入宿主层，
在导入表里注册；从 guest 出去的回调（虚表调用、GL、Lua）走 token 空间，保证
32 位 guest 指针不会意外跑到 64 位宿主代码里。GL 层是最有意思的部分：gl4es 把
固定管线调用翻译成 GLES2 着色器，但这个游戏实际上直接用着色器，所以 FPE 那条路
基本是死重量——怎么发现的见 `PITFALLS.md` §1。

---

## 4. 当前状态（实测，单设备）

| 项 | 值 |
|---|---|
| 设备 | Adreno 830 / Android 17（骁龙 8 Elite 级） |
| 帧率 | 142 个采样中位 56.9 fps（P10 43.6，最高 60.4） |
| 较早构建的最好情况 | 58-60 fps（17.10 ms/帧） |
| 冷启动 | 约 18.7 秒 |
| 崩溃 | 当前构建无（实测到第 8400 帧） |

**只在一台设备上验证过。** 其他 GPU 未知。你在别的机器上测出来，那是真正的新
信息。

**演示录像（3 分钟）：** https://youtu.be/V-F3hw618yU ——真机录制，未剪辑。
两个已知缺陷在里面是原样呈现的。

58-60 fps 那个数字来自 2026-11-12 之前的构建。当前构建带着排查字体图集时加的
诊断探针，实测更慢。去掉探针应该能找回约 3 fps；这件事没有测过。

---

## 5. 已知缺陷——根因已定位，未修

**5.1 字体图集上传为空。** 538 张纹理里 105 张全零。确认的实际缺陷是字体图集，
于是 HUD 数字全部渲染成同一个 `µ`，主菜单列表空白。六个假设被排除；线索指向
第七条 `SetTexelData` 调用路径。

**5.2 密集房间掉帧。** 追到 gl4es 的 `ToBuffer()` 被 `vao->locked == 1` 挡住。
修法写在 `PITFALLS.md` §10，但从未实施。

**5.3 未翻译指令。** 还有 20 条指令未实现，全是 MMX/SSE：`pabsd`、`pcmpeqd`、
`pshufb`、`vpcmpeqb`、`pmaddubsw`、`movq`、`pxor`、`movd`、`stmxcsr`、
`popfd`。它们在引用路径上，但正常游玩碰不到。**这就是为什么没有记录的运行走到
结局，也是为什么「能玩」不等于「能通关」。**

每条未实现指令都会调用 `guest_fault(c, addr, "untranslated: ...")`，所以撞上了
指令名会直接进日志。

---

## 6. 哪些没被验证

- **与 PC 的行为一致性。** 不存在同种子对照。x87 用 `double` 建模，不是 80 位；
  本平台的 `long double` 是 binary128（113 位），所以修法是存在的，但要重编全部
  192 个单元并做 profile。
- **长时间运行。** guest 堆上限自 2026-11-10 起是 2640 MB（两次 `std::bad_alloc`
  中止后从 1664 MB 提上来的）。改动之后没有长时运行记录。
- **其他设备。** 一块 GPU。
- **模组。** 测试设备上从未加载过任何 mod。mod 路径完全未测，而且 Windows 文件
  API 的宿主层有三个缺陷（非 ASCII 名字、同时 8 个目录句柄、`*.ext` 之外的通配
  符），会一次性弄坏很多 mod。

`LIMITATIONS.md` 是完整清单，每条都附验证命令。

---

## 7. 为什么放弃

**是迭代成本，不是难度。** 整个移植是在一台跑 Termux 的安卓手机上做的——没有
PC，没有 CI。每次改动都要重编全部 192 个翻译单元（208 MB 生成的 C，10-20 分钟），
然后手动装 APK（从 Termux 执行 `pm install` 被 SELinux 拒绝，只能点安装），再玩到
bug 出现。一轮 20-30 分钟，大部分在等，而剩下两个缺陷各自都需要很多轮。

在桌面电脑上，同样的重编只要几分钟，而且 ADB 能代替手动安装。工作本身不神秘，
瓶颈是硬件。

---

## 8. 构建需要什么

- **你自己的**正版忏悔 v1.7.9b（J835）——8,650,240 字节，
  sha256 `31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404`
- 它的资源：`resources/packed/*.a`（22 个归档，约 1.0 GiB）
- 上游 `repentogxm`——要 `recomp/runtime`、`gl_bridge.c`、
  `kage_pc_backend.h`，以及 `windows.h` shim
- Lua 5.3.3 头文件
- gl4es——从 `ptitSeb/gl4es` 构建 `libGL.a`
- 一台构建机：任意 x86-64，8 GB 内存。不需要显卡（构建期不渲染任何东西）。
  8 个并行编译的 RSS 峰值 1.48 GB。

包里的 `BUILD_SPEC_FOR_AGENTS.md` 是完整流水线，写成分阶段的契约，带通过/失败
判据。它的读者既是 AI 代理也是人，所以密度高但完整。

**这个压缩包不是能直接构建的树。** 刚解压时
`clang -fsyntax-only -I source source/minimal_kage.c` 会报
`'kage_pc_backend.h' file not found`。这是预期结果，不是上传坏了。

---

## 9. 来源

包里每一份文档——本文、`PITFALLS.md`、`BUILD_SPEC_FOR_AGENTS.md`、
`DISTRIBUTION.md`、`LIMITATIONS.md`、`ANNOUNCEMENT.md`、`README.md`——都是 AI
跨多轮会话写的，依据是设备日志、源码，以及在源码上跑过的命令。

测量结果可追溯：每个数字都来自某条日志，或某条你能重跑的命令。**不是第一手的**
是观察本身——没有任何 AI 在设备上装过它或玩过它。每次设备测试都是人跑的，AI
负责写记录。

如果这里的说法和你实测的不一致，以你的实测为准。
