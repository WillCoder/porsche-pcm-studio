# 免责声明 / Disclaimer

## 中文

**本项目仅供个人学习、研究与技术交流。使用者自负全部风险。**

- **本项目自己不刷写固件。** 它作为第二个 gf 客户端在运行时接管一个硬件层,自己的改动都在**内存**里 ——
  杀掉进程或断电即恢复原厂界面。
- 🚩 **但"本项目不刷写"≠"你不需要刷写"。** 音量、切换音源、调频这三样要驱动原厂的控制面,
  依赖一段**必须先刷进 IFS1 的 code cave**(由姊妹项目
  [porsche-pcm31-mods](https://github.com/WillCoder/porsche-pcm31-mods) 刷入,
  **那个会写 flash,有不可恢复的砖机风险**)。
  没有那段 cave 时这三条命令只是返回错误,界面其余部分照常工作。
  这份文档早先的版本把整个项目说成"不刷写固件",**那是错的**,已更正。
- **但"不刷写"不等于"没风险"。** 本项目会**读写另一个进程的内存**(通过 `/proc/<pid>/as`),
  也会**接管显示层**并可以**挡住原厂收触摸**。这些操作可能让原厂 HMI 行为异常、界面卡死、
  触摸暂时失效,极端情况下相关进程崩溃。断电重启可恢复,**但如果这发生在行驶中,那是安全隐患**。
- **不要在行驶中使用。** 只在车辆静止、变速箱驻车、场地安全的前提下试验。
- 文中的**地址、偏移、结构体成员**只对**特定固件版本 / 硬件变体**有效(本项目验证于 CHN 版台架)。
  换一台机器就可能不同,**照搬有害**。动手前务必自行核对。
- 本项目**不提供任何形式的担保**(不担保适销性、适用性、正确性、无害性)。
- 请遵守你所在地区的法律法规,尊重相关知识产权。改动车载系统可能影响**车辆安全、保修与合规**,风险自担。

> **一句话:它自己不刷 flash,但切源/音量/调频依赖一次已经完成的 IFS1 刷写;
> 而且它会动别人进程的内存 —— 别在开车的时候玩。**

## English

**This project is for personal study, research, and technical exchange only. Use entirely at your own risk.**

- **This project does not flash firmware itself.** It attaches as a second `gf` client and claims a
  hardware layer at runtime; its own changes live in **RAM** and are gone as soon as the process exits
  or power is cut.
- 🚩 **But "this project does not flash" is not "you need no flashing".** Volume, source switching and
  tuner control drive the stock control plane through a code cave that **must already be flashed into
  IFS1** (done by the sibling project
  [porsche-pcm31-mods](https://github.com/WillCoder/porsche-pcm31-mods), which *does* write flash and
  **can brick a unit beyond recovery**). Without that cave those three commands just return an error;
  the rest of the UI is unaffected. An earlier version of this document described the whole project as
  "does not flash firmware" — **that was wrong** and is corrected here.
- **"No flashing" is not "no risk".** This project **reads and writes another process's memory**
  (through `/proc/<pid>/as`), **takes over a display layer**, and can **stop the stock HMI from
  receiving touch input**. That can leave the stock UI misbehaving, frozen, or briefly unresponsive to
  touch, and in the worst case can crash the process it is reading. A power cycle restores everything —
  **but if it happens while the vehicle is moving, that is a safety hazard.**
- **Do not use it while driving.** Only experiment with the vehicle stationary, in park, in a safe place.
- The **addresses, offsets and structure members** documented here are valid only for a **specific
  firmware version / hardware variant** (this project was verified on a CHN-variant bench unit).
  Another unit may differ, and **copying them blindly is harmful**. Always verify for yourself first.
- This project comes with **no warranty of any kind** (no merchantability, fitness, correctness, or safety).
- Obey the laws and regulations of your jurisdiction and respect the relevant intellectual property.
  Modifying an in-vehicle system may affect **vehicle safety, warranty, and compliance** — the risk is
  entirely yours.

> **In one line: it does not flash anything itself, but volume/source/tuner rely on a cave that was
> flashed into IFS1 beforehand — and it reaches into another process's memory. Don't play with it while driving.**
