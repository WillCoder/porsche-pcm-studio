/*
 * pcm_sys.h — PCM Studio · 平台接口(整个开发环境的地基)
 *
 * 【为什么有这个文件】
 * 系统代码(场景/交互/状态机)**只认这个接口**, 不认 layermanager、不认 gf 层、不认 /proc/as、
 * 不认串口。平台各实现一份:
 *   - platform/plat_mac.c  -> Mac 窗口 + 鼠标键盘 + 假数据源  (开发时跑这个)
 *   - platform/plat_pcm.c  -> 真机 gf 层 + FPGA 输入 + 读原厂内存 (上台架跑这个)
 * ⇒ **一份系统源码, 两个后端**。渲染路线(独立gf层/0x1f劫持/PCM3Reload补丁)对系统代码
 *   是不可见的实现细节, 换后端不用改一行 UI 代码。
 *
 * 【像素格式】统一 RGBA5551 (R:15-11 G:10-6 B:5-1 A:bit0), 见 KB M-CANVAS-FMT。
 *   Mac 侧转 RGB888 显示, 真机侧直接就是硬件格式 ⇒ Mac 上看到的 == 台架画出来的。
 */
#ifndef PCM_SYS_H
#define PCM_SYS_H

/* 能力契约 —— **做不到的事在这里是 0, 用到它的代码编不出来**。
 * 加功能之前先读它: 它是这个引擎"能做什么"的唯一权威, 不是注释里的散文。 */
#include "pcm_caps.h"

typedef unsigned short u16_;
typedef unsigned char  u8_;

#define SCR_W 800
#define SCR_H 480

/* ================= 输入事件 ================= */
/* 车机的物理输入。Mac 侧用键盘/鼠标模拟, 真机侧从 FPGA/IPC 来。 */
enum {
    EV_NONE = 0,
    EV_ROTARY,        /* 旋钮: arg = +1/-1 (每格), which = 哪个旋钮 */
    EV_KEY_DOWN,      /* 按键按下: arg = 键码 */
    EV_KEY_UP,
    EV_TOUCH_DOWN,    /* 触摸/点击: x,y */
    EV_TOUCH_UP,
    EV_TOUCH_MOVE,
};
/* 旋钮编号 */
enum { KNOB_VOLUME = 0, KNOB_TUNE = 1 };
/* 键码(对齐 PCM 面板按键) */
enum {
    K_MEDIA = 1, K_RADIO, K_NAV, K_PHONE, K_INFO, K_CAR, K_SETUP,
    K_BACK, K_OK, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_PREV, K_NEXT, K_PLAY,
};

typedef struct {
    int type;         /* EV_* */
    int which;        /* 旋钮编号 / 保留 */
    int arg;          /* 旋钮增量 / 键码 */
    int x, y;         /* 触摸坐标 */
} PcmEvent;

/* ================= 车机状态(系统读它来渲染) ================= */
/* Mac 侧: 由模拟器面板手动喂 / 脚本喂。真机侧: 从原厂内存读(V4音量链等)。 */
enum { SRC_NONE=0, SRC_BT, SRC_FM, SRC_AUX, SRC_CD, SRC_USB, SRC_NAVI };
enum { PLAY_STOP=0, PLAY_PLAYING, PLAY_PAUSED };
/* 手机电量 —— AVRCP 的 5 档枚举(BSS_AVRCP_BATT_STATUS_*, libbssservice.so file-off 0x15e1c0)。
 * 顺序照抄原枚举, 外加一个 NONE 表示"没拿到"。**协议就是没有百分比这回事。** */
enum { BATT_NONE=0, BATT_CRITICAL, BATT_WARNING, BATT_NORMAL, BATT_FULL, BATT_EXTERNAL };

typedef struct {
    /* --- 音频 --- */
    int  volume;              /* 0..40 主音量 */
    int  muted;
    int  source;              /* SRC_* 当前音源 */
    /* --- 媒体播放 --- */
    int  play_state;          /* PLAY_* */
    int  pos_ms, dur_ms;      /* 播放进度 */
    char title[96];           /* 曲名 (UTF-8) */
    char artist[64];
    /* 蓝牙(AVRCP)一共只给 7 个属性: TITLE/ARTIST/ALBUM/TRACK/NUM_TRACKS/GENRE/DURATION
     * (出处: libbssservice.so file-off 0x15e100 的枚举表, 第 8 项就是 INVALID)。
     * 专辑和流派原厂 HMI 早就拿到了, 我们之前白扔着没读。 */
    char album[80];
    char genre[40];
    char device[48];          /* 蓝牙设备名 */
    int  u_trk_cur, u_trk_total;  /* 第几首 / 共几首。**很多手机不报, 那就都是 0** —— UI 必须能不显示 */
    /* 手机电量。🚨 AVRCP 只给 5 档枚举, **不是百分比** —— 所以是电池图标不是数字。
     * BATT_NONE = 没拿到(手机没报 / 通路不通)⇒ **整个图标不画**, 不许画个空电池假装。 */
    int  u_battery;
    /* 随机/重复。值域 0..4(MME 的 MME_RANDOM_* / MME_REPEAT_*)。
     * -1 = 还没读到 ⇒ 图标画成灰的; 手机不支持时服务端会 latch 关闭能力位。 */
    int  u_shuffle, u_repeat;
    /* --- 收音机 --- */
    int  freq_khz;            /* 例 87500 = 87.5MHz */
    char station[32];
    /* --- 系统 --- */
    int  u_hour, u_minute;
    int  ignition;            /* 点火状态 */
    /* ---- 原厂状态镜像(只读得来, 锚定说明见 plat_pcm.c) ----
     * 我们的 UI 盖住屏幕, 但**原厂仍在解释用户输入**并切它自己的页。
     * 这三个值就是"用户做了什么"的结果 —— 我们不碰事件流, 只读结果。 */
    int  stock_page;          /* CHBMenuManagerImpl +0x9a; -1 = 还没读到 */
    int  stock_src_slot;      /* CPSoundPresCtrl +0x860 */
    int  stock_src_app;       /* CPSoundPresCtrl +0x870 */

    /* ---- 收音机(PCM3Root CPTunerPresCtrl, 只读) ----
     * 🚨 **-1 = 未知**。场景遇到 -1 **一律不画该元素**, 不是画 0 也不是编一个 ——
     *   本项目铁律: 不许假渲染。 */
    int  u_freq_am_khz;         /* AM kHz; -1 未知 */
    int  preset_freq[9];      /* 预设/收藏电台 kHz; 0 = 空槽 */
    int  preset_band[9];      /* 1=FM 2=AM 0=空 */
    /* ⚠️ 拿不到、且**页面上不要留位置**的(2026-08-06 六份 dump 复核):
     *   · RDS 文本 / 电台名 —— 固件里只有请求侧, 8 份 dump 全是共享空串单例
     *   · 信号强度 —— 位置可信但量程完全未知(恒为 8), 画信号格就是编
     *   · FM1/FM2 子波段 —— 走 DSI 属性容器, 离线定位不了
     *   · 蓝牙设备名 —— 候选判据不唯一, 未收口 */
} PcmState;


/* ================= 不可靠数据: 必须走访问器 =================
 * 🚨 为什么要这么设计(2026-08-14):
 *   下面这些值**经常拿不到** —— 台架 RTC 给 -1、iPhone 常报曲目号 0/0、
 *   手机不报电量、MME 模式位还没读到就是 -1。而本项目有条铁律: **不许假渲染**,
 *   拿不到就什么都别画, 不许画个 0 或空图标假装。
 *   以前这条只写在注释里, 结果 4 个场景各自重写了一遍"-1 就不画"的判断 —— 靠人记得。
 * ⇒ 现在原始字段一律加 `u_` 前缀(unreliable), 场景**不许直接读**(构建期 lint 拦),
 *   只能走下面的访问器。访问器返回 0 就是"没有这个值", 你**必须**处理这个分支,
 *   于是"忘了判断"这件事在结构上做不到。
 */
static int pcm_clock(const PcmState *s, int *h, int *m){
    if(s->u_hour < 0 || s->u_minute < 0) return 0;      /* 台架没设 RTC 时就是 -1 */
    *h = s->u_hour; *m = s->u_minute; return 1;
}
static int pcm_battery(const PcmState *s, int *lvl){
    if(s->u_battery == BATT_NONE) return 0;             /* 手机没报 / 通路不通 */
    *lvl = s->u_battery; return 1;
}
static int pcm_track_index(const PcmState *s, int *cur, int *total){
    if(s->u_trk_cur <= 0 || s->u_trk_total <= 0) return 0;   /* iPhone 常报 0/0 */
    *cur = s->u_trk_cur; *total = s->u_trk_total; return 1;
}
static int pcm_shuffle(const PcmState *s, int *v){
    if(s->u_shuffle < 0) return 0;                      /* 还没读到 */
    *v = s->u_shuffle; return 1;
}
static int pcm_repeat(const PcmState *s, int *v){
    if(s->u_repeat < 0) return 0;
    *v = s->u_repeat; return 1;
}
static int pcm_freq_am(const PcmState *s, int *khz){
    if(s->u_freq_am_khz < 0) return 0;
    *khz = s->u_freq_am_khz; return 1;
}

/* ================= 弹层(overlay) =================
 * 场景之上的临时提示: 音量条/静音/提示消息。外壳统一管生命周期(淡入-停留-淡出)。
 * 结构放这里是因为**外壳调度**(pcm_shell.c)和**外壳绘制**(shell_draw.c)都要用它,
 * 放任一边都会造成循环依赖。 */
enum { OV_NONE=0, OV_GAUGE, OV_TOAST };
typedef struct {
    int  kind;          /* OV_* */
    int  val, maxval;   /* gauge 用 */
    int  icon;          /* 0=无 1=喇叭 2=静音 */
    char text[48];
    unsigned t0;        /* 出现时刻 */
    int  fade_in, hold, fade_out;   /* 毫秒 */
} ShellOverlay;
extern ShellOverlay g_ov;

/* ================= 平台接口(两个后端各实现一份) ================= */

/* 启动/关闭 */
int  plat_init(void);
void plat_shutdown(void);

/* 帧缓冲: 返回 800×480 RGBA5551 的可写缓冲。系统往里画。 */
u16_ *plat_framebuf(void);
/* 把画好的帧推上屏(Mac=刷新窗口, 真机=gf_layer_update/ShowDisplayables) */
void plat_present(void);

/* 取一个输入事件, 无事件返回 0 */
int  plat_poll_event(PcmEvent *ev);

/* 取当前车机状态(Mac=模拟面板给的, 真机=读原厂内存) */
void plat_read_state(PcmState *st);

/* 发命令给底层(Mac=改模拟状态+打日志, 真机=走原厂控制面 IPC) */
/* 🚨 **这个枚举就是能力清单本身** —— 做不到的命令不在这里, 于是场景写都写不出来。
 *   CMD_SEEK 曾经在这儿, 旁边写着"蓝牙下预期失败, 别进主路径"。
 *   注释拦不住任何人: 只要它存在, 就总有人会去做拖动进度条。2026-08-14 删掉。
 *   理由和证据在 pcm_caps.h 的 CAP_SEEK。要加回来, 先把 CAP_SEEK 改成 1 并附真机实证。 */
enum { CMD_PLAY=1, CMD_PAUSE, CMD_NEXT, CMD_PREV, CMD_SET_VOLUME, CMD_SET_SOURCE,
       CMD_TUNE,        /* 收音机: 调到 arg kHz */
       CMD_SET_SHUFFLE, /* arg 0..4 —— **值域在 plat 层钳死**, 服务端不做上界检查 */
       CMD_SET_REPEAT };
int  plat_command(int cmd, int arg);
/* 🚨 触摸门 / 内存探针这些**引擎内部**的东西已经搬到 sys/plat_internal.h ——
 *   场景层看不到它们。触摸门是引擎维护的**不变式**(盖着⟺装着), 不是谁想调就能调的开关;
 *   一旦某个场景自己去 arm/disarm, 那条不变式就没人说得清了。 */

/* 毫秒时钟(动画/生命周期用) */
unsigned plat_now_ms(void);
/* 日志 */
void plat_log(const char *s);

/* 平台名(诊断用): "mac" / "pcm" */
const char *plat_name(void);

/* 这台机器做得起整屏转场动画吗?
 * 转场要连画 6+ 帧整屏, 而 PCM 上一次整屏上屏 = 130ms(拷贝) + 7ms(比对) —— 做不起,
 * 硬做出来只是"闪一下黑再跳到位", 比不做还难看(2026-08-05 台架实测过)。
 * Mac 返回 1, PCM 返回 0。等显存写路径再快一个数量级(store queue)时再把 PCM 打开。 */
int plat_can_animate(void);

#endif /* PCM_SYS_H */
