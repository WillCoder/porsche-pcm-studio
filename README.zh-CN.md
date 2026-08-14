# porsche-pcm-studio

**给 2009 款保时捷 PCM 3.1 做的替换版媒体界面 —— 自绘、与原厂固件并存、零刷写**

> Porsche PCM 3.1(CHN)· QNX 6.3.2 · SH-4A · 800×480 · 2026-08
> **✅ 已在台架上跑通:真实曲目信息、真实播放控制、真实触摸。**
>
> [English](README.md) · **简体中文**

> ⚠️ **免责声明**:仅供学习研究。本项目**不写 flash** —— 所有改动都在内存里,断电即恢复 ——
> 但它会读写车机上另一个进程的内存。**后果自负,别怪我。**
> 全文:[DISCLAIMER.md](DISCLAIMER.md) · 授权:[GPL-3.0](LICENSE)

![PCM Studio 在台架上运行](images/01-bench-btplay.jpg)

*台架,蓝牙播放页。曲名、歌手、专辑、流派、已播/总长和手机名全部是从原厂固件里实时读出来的;
下面那排按钮再通过 MME 把命令发回去。*

---

## 这是什么

PCM 3.1 的界面是通过 QNX 的 `gf` 图形服务画的。**Studio 作为第二个 `gf` 客户端接进去**,
占用一个原厂没在用的硬件层,自己画满 800×480 盖在上面。原厂软件在底下照常跑 ——
音频通路、蓝牙栈、调谐器、持久化存储都还是它的。**我们只替换驾驶员看到的那一层。**

这个选择决定了整个设计:

- **零刷写。** 不刷固件、不改磁盘上的任何东西。杀掉进程或者断电,原厂界面原封不动地回来。
- **不用维护一份分叉固件。** 我们**读**原厂的状态,而不是把它重新实现一遍。
- **原厂仍然是权威。** 音量、音源、电台预设、电话本 —— 全是原厂实现。我们是表现层。

## 现状

| | |
|---|---|
| 蓝牙播放页 | **台架上可用** —— 曲目信息、进度、播停、上下曲、随机、重复、触摸 |
| 收音机 / AUX / 首页 / 设置 | 只有骨架,版式而已 |
| 真车(911 / 9x1) | **还没试过**,目前只有台架 |
| 专辑封面 | AVRCP 1.3 下**做不到** —— 见 [docs/capabilities.md](docs/capabilities.md) |

## 怎么组织的

```
studio/
├── sys/
│   ├── pcm_caps.h        能力契约 —— 引擎能做什么、不能做什么
│   ├── pcm_sys.h         场景**被允许看到**的接口
│   ├── plat_internal.h   引擎专用的平台钩子(场景够不着)
│   ├── pcm_shell.c       场景调度 + 整页/局部重绘策略
│   └── shell_draw.c      外壳自己的绘制(转场、弹层)
├── scenes/
│   ├── gfx.c             平台无关绘图库
│   └── scene_*.c         一页一个文件
├── platform/
│   ├── plat_pcm.c        真机:gf 层、显存、状态镜像、MME、触摸门
│   └── plat_mac.c        macOS:浏览器里的离线预览
├── main_pcm.c            车机上的入口
├── main_mac.c            开发机上的入口
└── tools/                build.sh · bake_font.py · studio_server.py
```

**一份源码,两个后端。** 场景代码永远看不到 `gf` 调用或 `/proc` 读取;它只向 `plat_*` 要
一块帧缓冲、一份状态、一个命令通道。macOS 后端把同样的像素画进浏览器,所以**改版式不用上硬件**。

## 这个项目真正的地基

> **边界应该长在代码里,不是注释里。**

这个项目上浪费掉的时间,大多不是 bug,而是**第三次重新发现某件事做不到** ——
或者忘了一条写下来但没人强制的规矩。所以现在约束都进了构建:

| 约束 | 怎么做到的 | 谁在强制 |
|---|---|---|
| 做不到的命令 | `CMD_SEEK` 从命令枚举里删掉了,所以拖动进度条**写都写不出来** | **编译器** |
| 经常拿不到的值 | 原始字段带 `u_` 前缀,只能走返回"没有这个值"的访问器 —— **必须**处理未知分支 | **编译器** |
| 会静默出错的写法 | 颜色宏放进三元;手写 `read()` 循环;场景去碰引擎内部接口 | **构建期**(`tools/build.sh` 的 `lint_guards()`) |
| 版式漂移 | 每个键画的中心必须落在它自己的命中区里;脏矩形必须盖住会动的东西 | **开机自检** |
| 硬件能做什么不能做什么 | 一处权威的、带出处的清单:[`pcm_caps.h`](studio/sys/pcm_caps.h) | **靠人看** —— 见下方说明 |

> 上面前四行是机器在强制的,最后一行不是:`pcm_caps.h` 提供了 `#if CAP_X` 和
> `PCM_REQUIRE_CAP()`,但目前几乎没有代码在用,所以那份能力清单是靠**读**它来约束的。
> 这一点在文件里如实标着,没有在这里夸大。

举个真做过的例子。`CMD_SEEK` 曾经存在于枚举里,旁边写着*"蓝牙下预期失败,别进主路径"*。
那条注释没拦住任何人 —— 只要它还在,早晚有人会拿它做一个可拖动的进度条。
删掉枚举值之后,**两个后端里的残留实现立刻编译失败**,不清理就编不过。
现在它写都写不出来,而 [`pcm_caps.h`](studio/sys/pcm_caps.h) 记着为什么,带证据。

## 文档

| | |
|---|---|
| [docs/capabilities.md](docs/capabilities.md) | 能做什么、不能做什么、以及**只是还没试过**的 |
| [docs/hardware.md](docs/hardware.md) | 逼着每一个设计决定变形的那些 PCM 3.1 约束 |
| [docs/build-and-deploy.md](docs/build-and-deploy.md) | 构建两个后端、烤字库、装到机器上 |

## 快速开始(离线预览,不需要硬件)

```bash
python3 studio/tools/bake_font.py --body-font /path/to/NotoSansSC-var.ttf \
        --weight Medium --body-px 24 --charset gb2312 --bin studio/studio_notosc.fnt
bash studio/tools/build.sh mac
STUDIO_SCENE=btplay /tmp/pcm_studio_run
python3 studio/tools/studio_server.py     # 然后打开 http://localhost:8770
```

字库不进仓库 —— 它是 Noto Sans SC(SIL OFL)的衍生物,而上面那条命令能逐字节复现出同一份。
见 [docs/build-and-deploy.md](docs/build-and-deploy.md)。

## 相关

[porsche-pcm31-mods](https://github.com/WillCoder/porsche-pcm31-mods) —— 姊妹仓库:同一台机器上
两个已经落地的固件改动(蓝牙开机修复、悬浮音量 OSD),以及做出它们所用的整套逆向工具。
