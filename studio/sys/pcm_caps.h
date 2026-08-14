/*
 * pcm_caps.h — PCM Studio · **能力契约**
 *
 * 【为什么有这个文件】
 * 这个项目最贵的返工, 不是写错代码, 是**反复试探一件早就知道做不到的事**。
 * 以前这些边界只存在于注释和记忆文件里 —— 于是每次做新功能都要重新踩一遍。
 *
 这个文件把边界写成**一处权威的、带出处的清单**:
 *   · CAP_X = 1  ⇒ 引擎提供这个能力, 用法见注释
 *   · CAP_X = 0  ⇒ 做不到, 或者没验过 —— **两者必须分清**(见 CAP_SEEK 那条的教训)。
 *     每条都带**证据出处**(代码位置 / KB 记忆名 / 台架实测日期), 不是"我觉得不行"。
 * ⚠️ **它现在还不是编译期强制的**(2026-08-14 文档审查发现我把这句写成了既成事实)——
 *    见文件末尾"编译期强制"一节的现状说明。别高估它。
 *
 * 【改这个文件的规矩】
 *   把某条从 0 改成 1, 必须先有**真机实证**, 并把出处写进注释。
 *   "我觉得应该可以"不构成理由 —— 这个文件的全部价值就在于它不说这种话。
 */
#ifndef PCM_CAPS_H
#define PCM_CAPS_H

/* ================= 媒体控制(蓝牙 / MME) ================= */

/* 播放 / 暂停 —— MME set_speed(subtype 5), body 1000/0 */
#define CAP_PLAY_PAUSE      1
/* 上一曲 / 下一曲 —— **必须走 mme_button(18) body 0/1**。
 * ⚠️ mme_next/prev(10/11) 在蓝牙流上恒定 ENODATA(61): 流式源没有 trksession。
 *    引擎会先试一次 10, 失败后 latch 住直接走 18 —— 场景不用管。
 * 🚨 button(18) 的前置条件: **必须正在放音**。停播时服务端回 errno 9(它没有设备 fd)。
 * 出处: KB mme-wire-format-cracked-2026-08-13(台架逐字节实证真换曲) */
#define CAP_NEXT_PREV       1
/* 随机 / 重复 读写 —— MME subtype 14/15(读) + 写。值域 0..4, **引擎钳死**。
 * 出处: KB bt-mme-protocol-layer-2026-08-12 */
#define CAP_SHUFFLE_REPEAT  1

/* ⚠️ 跳转到指定位置(拖动进度条)—— **未验证, 预期失败**, 不是"协议里没有"。
 * 🚨 2026-08-14 更正: 这条我一开始写成"27 条 GCF 表和 115 个 mme_* 符号里都没有 seek",
 *   **那是错的**。KB bt-stack-capability-audit-2026-08-13 §5 明确列着
 *   `mme_seektotime` = **MME subtype 9**, 64 位时间在 body +16/+20, 风险中等,
 *   定性是"**当预期失败来试**"。真正的理由只有一条: 蓝牙是流式源、没有 trksession,
 *   所以**预期**它会像 next/prev(10/11) 那样回 ENODATA —— 但**没有人真的试过**。
 * ⇒ 在试出结论之前保持 0: 进度条不注册触摸(长得像能拖却不响应比不做更糟)。
 *   要改成 1, 先上机试 subtype 9 并把结果写在这里。
 * 📌 把"没试过"写成"不可能"是这个文件最该避免的错误 —— 它会让人再也不去试。 */
#define CAP_SEEK            0

/* ❌ 专辑封面
 * AVRCP 1.3(BlueSDK 2010), 协议里没有封面这回事 —— 四条独立硬证据。
 * ⇒ 版式里**不要留封面位**。要视觉主体就自己生成(见 gfx_artbg/gfx_art_tile),
 *   但那是装饰, 不能冒充"这首歌的封面"。
 * 出处: KB bt-mme-protocol-layer-2026-08-12 */
#define CAP_ALBUM_ART       0

/* ❌ 歌词 */
#define CAP_LYRICS          0

/* ❌ 收藏同步回手机
 * AVRCP 表里根本没有 rating/favorite 这个概念; SendVendorDependent 零命中;
 * OBEX target 是 5 路白名单, 没有 Apple iAP。
 * 出处: KB bt-stack-capability-audit-2026-08-13 */
#define CAP_FAVORITE_SYNC   0

/* ❌ 真实音频波形 / 频谱
 * 蓝牙音频**直接进 DSP**, SH4 主控只做控制面, 摸不到采样。
 * ⇒ 任何"波形""频谱""律动"都是合成的。合成的装饰可以做, 但**不能画成像是真音频**
 *   (2026-08-14: 波形进度条因此被撤 —— 它形状上就在暗示"这是这首歌的音频")。
 * 出处: KB pcm31-audio-control-plane-arch */
#define CAP_AUDIO_SPECTRUM  0

/* ⚠️ 浏览手机曲库 —— **未定论**, 不是"做不到"。
 * 我们是 CT, 浏览通道由手机 TG 广播; avrcpbrws.c + CT_BROWSING 置位 +
 * 插件 mediaBt_BrowseFolder/PlayItem 都在。"PSM 0x1B 零命中 ⇒ 不通"是我推错过的一次。
 * ⇒ 只能上机试。在试出结论之前**别在版式里给它留位置**。
 * 出处: KB bt-stack-capability-audit-2026-08-13 */
#define CAP_BROWSE_LIBRARY  0

/* ================= 显示 ================= */

/* 独立 gf 客户端抢硬件层, 整屏 800×480 覆盖原厂。出处: KB gf-independent-hwlayer-overlay-path */
#define CAP_FULLSCREEN_OVERLAY 1
/* 局部重绘(裁剪矩形 + 脏行搬运)。场景给 anim_rect, 外壳设裁剪跑同一个 render。 */
#define CAP_PARTIAL_REDRAW  1

/* ❌ 整屏转场动画
 * 一次整屏上屏在 PCM 上是 130ms 拷贝 + 7ms 比对 —— 6 帧转场做不起,
 * 硬做出来是"闪一下黑再跳到位", 比不做还难看。
 * ⇒ 引擎用 plat_can_animate() 闸门, PCM 侧恒返回 0。
 * 出处: 2026-08-05 台架实测 */
#define CAP_SCREEN_TRANSITION 0

/* ❌ 任意角度旋转 / 真高斯模糊 / 位图贴图 / 亚像素平移
 * gfx 是纯软件逐像素, 没有这些原语, 也没有预算做。
 * 需要"转"的东西用参数化重画(如黑胶: 每帧按角度重算高光), 不是旋转位图。 */
#define CAP_ROTATE_BLIT     0
#define CAP_GAUSSIAN_BLUR   0

/* ================= 输入 ================= */

/* 读触摸/按键(从原厂镜像读, 不抢事件流)。出处: KB stock-state-mirror-anchors-2026-08-05 */
#define CAP_READ_INPUT      1
/* 挡住原厂收触摸(触摸门 *(S+0x9c)=1, 需 *(S+0x1a0)==2)。
 * ⚠️ **有效性尚未定论** —— 08-13 三次实测结果不一致(设置页有效/蓝牙页一次失败一次成功)。
 *    引擎按不变式维护(盖着⇒装门), 但**别假设它一定挡得住**。
 * 出处: KB touch-gate-proven-stock-blind-we-see-2026-08-12 */
#define CAP_BLOCK_STOCK_TOUCH 1

/* ❌ 运行时给原厂进程注入代码
 * /proc/as 只写得了 RW 段, **写不了 RO 代码段** —— 实证过的死路。
 * ⇒ 要改原厂行为只能刷固件(cave), 不能运行时打补丁。
 * 出处: KB cow-wall-runtime-code-injection-impossible */
#define CAP_RUNTIME_CODE_INJECT 0

/* ================= 编译期强制 =================
 * 🚨 **现状(2026-08-14, 说实话)**: 下面这两个机制是**给未来用的**, 目前
 *   `studio/` 里还没有任何代码需要一个 0 能力, 所以 `#if CAP_X` 的调用点为零。
 *   ⚠️ 我在这句的**上一版**写着"唯一在用的是 CAP_BLOCK_STOCK_TOUCH(见 plat_pcm.c)" ——
 *      **那也是假的**, grep 全树 plat_pcm.c 里一次都没引用过它。
 *      在纠正一句假话的同一段里又写了另一句假话, 记在这儿当个提醒:
 *      **凡是"某处在用 X"这种话, 写之前先 grep 一次。**
 *   ⇒ 别把这个文件说成"做不到的代码编不出来" —— **它现在还没到那一步**。
 *     真正在起作用的是另外三件: 命令枚举里没有做不到的命令(写都写不出来)、
 *     不可靠数据必须走访问器、构建期 lint。
 *   这条注释本身就是被自己的文档审查抓出来的: 我在 README 里把它写成了既成事实。 */
/* 用法: 想用某个能力时写
 *     #if CAP_SEEK
 *         ...拖动进度条的代码...
 *     #endif
 * 更强的写法(直接让误用编不过, 不给"忘了加 #if"的机会):
 *     PCM_REQUIRE_CAP(CAP_SEEK);   // CAP 为 0 时是负数组下标, 编译报错
 */
#define PCM_CAT_(a,b) a##b
#define PCM_CAT(a,b)  PCM_CAT_(a,b)
#define PCM_REQUIRE_CAP(c) \
    typedef char PCM_CAT(pcm_cap_required_at_line_, __LINE__)[(c) ? 1 : -1]

#endif /* PCM_CAPS_H */
