/*
 * pcm_shell.c — PCM Studio · 系统外壳(场景管理器)
 *
 * 这是"新系统"的骨架, 跟平台无关: 场景注册 / 切换 / 事件路由 / 渲染循环 / 返回栈。
 * Mac 和真机跑的是**同一份这个文件**。
 *
 * 一个场景 = 一屏(播放页/首页/音源/收音机/设置...)。场景只需实现:
 *   enter/leave (可选) · render(必需) · on_event(可选)
 * 场景不碰硬件, 只用 PcmState 读状态 + plat_command 发命令 + 画到给它的帧缓冲。
 */
#ifndef PCM_SHELL_C
#define PCM_SHELL_C

#include "pcm_sys.h"
#include "pcm_i18n.h"   /* 界面文案唯一出处; 场景经由本文件间接拿到 T() */

#define MAX_SCENES 16
#define NAV_DEPTH  8

typedef struct PcmScene {
    const char *id;                               /* 场景标识, 例 "btplay" */
    int title;                                    /* 显示名的**文案 id**(STR_*), 不是字符串 ——
                                                   * 表是 static const, 存字符串就得在编译期定死语言。
                                                   * ⚠️ 目前全树没有任何地方读它; 保留是因为它零成本且类型正确。 */
    void (*enter)(void);                          /* 进入(可选) */
    void (*leave)(void);                          /* 离开(可选) */
    void (*render)(u16_ *fb, const PcmState *st, unsigned t_ms);  /* 画一帧(必需) */
    int  (*on_event)(const PcmEvent *ev, const PcmState *st);     /* 处理输入, 返回1=已消费 */
    /* 场景想要连续动画时返回**重画间隔(ms)**, 不想要返回 0(或整个指针为 0)。
     * 🚨 为什么要场景自己说、而且要给间隔而不是"每帧画":
     *   台架实测一次重画 = 渲染 27ms + 上屏 20ms ≈ **47ms**, 主循环才 25ms 一拍。
     *   无脑每帧画会把 CPU 吃满、触摸响应变钝。所以由场景按"动画本身的步长"给间隔,
     *   动画停了(比如暂停)就返回 0, 一拍都不多画。 */
    int  (*anim_ms)(const PcmState *st);
    /* 局部动画的**脏矩形**: 填 [x0,y0,x1,y1) 并返回 1, 不做局部就返回 0(或指针为 0)。
     * 🚨 只在"这一拍**唯一**的重画理由是动画计时器"时才生效 —— 事件/状态变化/转场一律整页。
     * 为什么这样是安全的(2026-08-13 想清楚的, 别再怀疑一遍):
     *   g_fb 是**唯一一块**系统内存帧缓冲, 场景永远画进它; 局部只覆盖矩形内, 矩形外仍是上一整帧
     *   ⇒ g_fb 任何时刻都是完整正确的一帧。
     *   而 present_diff 拿 g_fb 跟**每块 bank 各自的影子**比 ⇒ 双缓冲"隔一帧陈旧"的坑在这个
     *   结构下根本不存在(影子各算各的, 该补的差它自己会补上)。
     * 💰 收益: 场景 render 整页 ~27ms; 裁剪后成本≈矩形面积占比(原语全带包围盒早退)。 */
    int  (*anim_rect)(const PcmState *st, int *x0, int *y0, int *x1, int *y1);
} PcmScene;

/* ---------------- 弹层(overlay) ----------------
 * 结构定义在 pcm_sys.h(shell_draw.c 也要用)。这里是实体 + 生命周期。
 * 任何地方调 shell_gauge/shell_toast 就能弹, 场景不用管。 */
ShellOverlay g_ov;

/* 返回当前不透明度 0..255, 0 = 已结束 */
static int shell_ov_alpha(unsigned now){
    unsigned dt;
    if(g_ov.kind == OV_NONE) return 0;
    dt = now - g_ov.t0;
    if(dt < (unsigned)g_ov.fade_in)  return (int)(dt*255/(unsigned)g_ov.fade_in);
    dt -= (unsigned)g_ov.fade_in;
    if(dt < (unsigned)g_ov.hold)     return 255;
    dt -= (unsigned)g_ov.hold;
    if(dt < (unsigned)g_ov.fade_out) return 255 - (int)(dt*255/(unsigned)g_ov.fade_out);
    g_ov.kind = OV_NONE;
    return 0;
}
static void shell_gauge(int val,int maxval,int icon){
    g_ov.kind=OV_GAUGE; g_ov.val=val; g_ov.maxval=maxval; g_ov.icon=icon;
    g_ov.text[0]=0; g_ov.t0=plat_now_ms();
    g_ov.fade_in=140; g_ov.hold=1200; g_ov.fade_out=320;
}
static void shell_toast(const char *s){
    int i;
    g_ov.kind=OV_TOAST; g_ov.icon=0;
    for(i=0;i<47 && s[i];i++) g_ov.text[i]=s[i];
    g_ov.text[i]=0;
    g_ov.t0=plat_now_ms();
    g_ov.fade_in=140; g_ov.hold=1600; g_ov.fade_out=320;
}

/* ---------------- 内部状态 ---------------- */
static const PcmScene *g_scenes[MAX_SCENES];
static int   g_nscene = 0;
static int   g_cur = -1;                 /* 当前场景索引 */
static int   g_navstack[NAV_DEPTH];      /* 返回栈 */
static int   g_navtop = 0;
static int   g_dirty = 1;                /* 需要重画 */
/* 这一拍的重画是不是"整页级"的理由(事件/状态变化/切场景/外部 invalidate)。
 * 只有它 == 0 而 g_dirty == 1 时, 才允许走 anim_rect 的局部重绘。 */
static int   g_dirty_full = 1;
static unsigned g_scene_t0 = 0;          /* 当前场景进入时刻(动画用) */
#define SCENE_FADE_MS 220                /* 场景转场时长 */
static int   g_was_anim = 0;             /* 上一拍是否在动画中(用于"动画结束补最后一帧") */
static unsigned g_tick_ms = 0;           /* 实测一拍耗时 */
unsigned g_render_ms = 0;                /* 实测场景 render 耗时(平台侧打日志用) */
static unsigned g_fade_ms = SCENE_FADE_MS;  /* 本拍生效的转场时长(慢机器上会被压成 0) */

/* ---------------- 注册 / 查找 ---------------- */
static int shell_register(const PcmScene *sc){
    if(g_nscene >= MAX_SCENES || !sc || !sc->render) return -1;
    g_scenes[g_nscene] = sc;
    return g_nscene++;
}
static int shell_find(const char *id){
    int i, j;
    for(i=0;i<g_nscene;i++){
        const char *a=g_scenes[i]->id, *b=id;
        for(j=0; a[j] && b[j] && a[j]==b[j]; j++);
        if(!a[j] && !b[j]) return i;
    }
    return -1;
}

/* ---------------- 切换 ---------------- */
static void shell_goto_idx(int idx, int push){
    if(idx < 0 || idx >= g_nscene || idx == g_cur) return;
    if(g_cur >= 0){
        if(push && g_navtop < NAV_DEPTH) g_navstack[g_navtop++] = g_cur;
        if(g_scenes[g_cur]->leave) g_scenes[g_cur]->leave();
    }
    g_cur = idx;
    g_scene_t0 = plat_now_ms();
    if(g_scenes[g_cur]->enter) g_scenes[g_cur]->enter();
    g_dirty = 1; g_dirty_full = 1;
    plat_log("scene -> ");
    plat_log(g_scenes[g_cur]->id);
    plat_log("\n");
}
/* 切场景。⚠️ 找不到就 toast 提示 —— 早先版本静默返回, 点了没反应还以为是事件没送到, 白查。 */
static void shell_goto(const char *id){
    int idx = shell_find(id);
    if(idx < 0){
        plat_log("scene not found: "); plat_log(id); plat_log("\n");
        shell_toast(T(STR_NOT_BUILT));
        return;
    }
    shell_goto_idx(idx, 1);
}
static void shell_back(void){
    if(g_navtop > 0) shell_goto_idx(g_navstack[--g_navtop], 0);
}
static const PcmScene *shell_current(void){ return g_cur>=0 ? g_scenes[g_cur] : 0; }
static void shell_invalidate(void){ g_dirty = 1; g_dirty_full = 1; }

/* 🔁 **请路由重判一次。**
 * 2026-08-17 用户报的 bug: 当前源已经是蓝牙时, 在音源页再点蓝牙卡片**毫无反应**。
 * 根因: 点卡片只做一件事 —— 发 `CMD_SET_SOURCE`。而"去哪一页"完全由 main_pcm 的页路由
 * 决定, 它的触发条件是 `原厂页 id 变了`。源本来就是蓝牙 ⇒ 原厂页不变 ⇒ 路由一次都不跑。
 *
 * ⚠️ 为什么不让场景直接 `shell_goto("btplay")`:
 *   路由那里写着"**谁显示只在这一处决定**", 两边各判一次迟早打架, 而且到时候
 *   "为什么这页是原厂的"会变得没人说得清。所以场景只**请求重判**, 决定权仍在路由 ——
 *   接管开关、页白名单、让不让屏, 全都还是那一套逻辑说了算, 不会出现绕过开关的后门。 */
static int g_reroute = 0;
static void shell_request_reroute(void){ g_reroute = 1; }
static int  shell_take_reroute(void){ int r = g_reroute; g_reroute = 0; return r; }

/* ---------------- 全局输入(任何场景都生效) ---------------- */
static int shell_global_key(const PcmEvent *ev){
    /* 🔊 音量是**系统级**的, 不该由某个场景各写一遍 —— 原来只有 btplay 处理,
     *   在 FM 页拧音量旋钮什么都不会发生(2026-08-12 上机发现)。
     *   放在这里 = 任何页面都能调音量, 和原厂行为一致。
     *   (音量条弹出由下面主循环里的"volume 变了"自动触发, 不用场景管。) */
    if(ev->type == EV_ROTARY && ev->which == KNOB_VOLUME){
        PcmState s; plat_read_state(&s);
        plat_command(CMD_SET_VOLUME, s.volume + ev->arg);
        return 1;
    }
    if(ev->type != EV_KEY_DOWN) return 0;
    switch(ev->arg){
        case K_BACK:  shell_back(); return 1;
        case K_MEDIA: shell_goto("btplay"); return 1;
        case K_RADIO: shell_goto("radio");  return 1;
        case K_SETUP: shell_goto("settings"); return 1;
        case K_CAR:   shell_goto("source"); return 1;   /* 原来叫 home; 它就是音源页 */
    }
    return 0;
}

/* ---------------- 主循环的一拍 ---------------- */
/* 返回 0 = 该退出。宿主(Mac 窗口 / 真机 main)反复调它。 */
static int shell_tick(void){
    PcmEvent ev;
    PcmState st;
    const PcmScene *sc;
    static PcmState last_st;
    static int have_last = 0;
    unsigned now;
    int ov_a, in_anim;

    /* 先读状态 —— 事件处理要用它(早先版本在这之前就把 &st 传给 on_event, 是未初始化读) */
    plat_read_state(&st);

    /* 1. 吃掉所有待处理事件 */
    while(plat_poll_event(&ev)){
        if(ev.type == EV_NONE) continue;
        /* 🔓 SOURCE 键**在场景之前处理, 任何场景都不许吞掉它**。
         *   它是我们让开屏幕之后被叫回来的唯一入口 —— 一旦某个场景把它当自己的键消费掉,
         *   用户就再也回不来了, 而且这种"某个页面按 SOURCE 没反应"极难归因。
         *   所以它不走 shell_global_key(那条路是场景优先), 直接在这儿截。 */
        if(ev.type == EV_KEY_DOWN && ev.arg == K_SOURCE){
            plat_take_screen();          /* 让开中的话先把屏幕收回来 */
            shell_goto("source");
            g_dirty = 1; g_dirty_full = 1;
            continue;
        }
        sc = shell_current();
        /* 先给场景, 场景不要再走全局 */
        if(!(sc && sc->on_event && sc->on_event(&ev, &st)))
            shell_global_key(&ev);
        g_dirty = 1; g_dirty_full = 1;
    }

    /* 2. 状态变了就重画(省性能: SH4 上整屏 768KB/帧, 不能白刷) */
    if(!have_last ||
       st.volume!=last_st.volume || st.source!=last_st.source ||
       st.play_state!=last_st.play_state ||
       st.u_minute!=last_st.u_minute || st.freq_khz!=last_st.freq_khz){
        g_dirty = 1; g_dirty_full = 1;
    }
    /* 2a. 播放位置每秒推进 —— **降级成"可局部"**。
     * 这是稳态下最频繁的重画理由(每秒一次), 原来它走整页 = 每秒白烧 27ms 渲染,
     * 而实际变的只有进度条那一条和两个时间数字。场景给了 anim_rect 就只重画那一块;
     * 没给就照旧整页(下面渲染处不设裁剪即可), 行为不退化。 */
    if(have_last && st.pos_ms/1000 != last_st.pos_ms/1000) g_dirty = 1;
    /* 2b. 场景要连续动画(比如播放中的 EQ 条)—— 按它自己给的间隔标脏。
     *     ⚠️ 不是"每帧画": 一次重画 ~47ms 而一拍 25ms, 每帧画会吃满 CPU 拖钝触摸。 */
    { const PcmScene *asc = shell_current();
      static unsigned last_anim = 0;
      int iv = (asc && asc->anim_ms) ? asc->anim_ms(&st) : 0;
      if(iv > 0){
          unsigned n2 = plat_now_ms();
          if(last_anim == 0 || n2 - last_anim >= (unsigned)iv){ last_anim = n2; g_dirty = 1; }
      } else last_anim = 0; }
    /* 音量变化自动弹音量条 —— 系统级行为, 任何场景都有 */
    /* 🚫 **音量不再弹层**(2026-08-14)。弹层活着 ⇒ in_anim ⇒ 局部重绘被禁 ⇒ 整页重画,
     *   而且持续 1.66 秒 —— 台架上表现为"拧音量画面会跳"。
     *   音量现在画在各页的状态栏里(shell_draw_status), 拧一下 = 一次普通状态变化 = 重画一帧。
     *   ⚠️ 上面那段"状态变了就重画"里已经含 st.volume, 所以去掉弹层不影响刷新。 */
    last_st = st; have_last = 1;

    /* 3. 画。动画期间(转场/弹层)必须每帧画, 不能等 dirty */
    now    = plat_now_ms();
    /* 🚨 自适应转场: 量一拍实际多久, **装不下动画就干脆不做动画**。
     *   台架实测一拍 ~358ms > 220ms 转场 —— 这种机器上淡入只会闪一下黑再跳到位,
     *   既难看又白搬两次整屏(每次 258ms)。Mac 上一拍 33ms, 动画照常。 */
    { static unsigned last_now = 0;
      if(last_now){ unsigned d = now - last_now; if(d < 5000) g_tick_ms = d; }
      last_now = now; }
    /* 硬闸门: 平台说做不起动画就不做。再叠一层实测保护(一拍太慢也不做)。 */
    g_fade_ms = (!plat_can_animate() || g_tick_ms * 2 >= SCENE_FADE_MS) ? 0 : SCENE_FADE_MS;
    ov_a   = shell_ov_alpha(now);
    in_anim = (ov_a > 0) || (now - g_scene_t0 < g_fade_ms);
    /* 🚨 动画结束必须**补画最后一帧**, 否则屏幕永久停在动画的中间态。
     *   2026-08-05 台架实测: 切到蓝牙页后**整屏纯黑不动**。
     *   原因是真机一拍 ~358ms(100ms sleep + 258ms 整屏拷进显存), 比整个 220ms 转场还长:
     *     第1拍 t≈0  -> 淡入 alpha=255 = 全黑, 画出去;
     *     第2拍 now-t0 已 >220ms -> in_anim=false, 而 g_dirty 上一拍就清了 -> 再也不画。
     *   Mac 上一拍 33ms 所以永远碰不到, 这是**只有真机才暴露的时序 bug**。 */
    if(g_was_anim && !in_anim){ g_dirty = 1; g_dirty_full = 1; }
    g_was_anim = in_anim;
    sc = shell_current();
    if(sc && (g_dirty || in_anim)){
        u16_ *fb = plat_framebuf();
        if(fb){
            unsigned t = now - g_scene_t0, t_r0 = plat_now_ms();
            /* 🔪 局部重绘: 只有这一拍**没有任何整页级理由**、且不在转场/弹层动画中时才走。
             *   场景没实现 anim_rect 或它返回 0 -> 不设裁剪 -> 整页, 行为与以前完全一致。 */
            int part = 0;
            if(g_dirty && !g_dirty_full && !in_anim && sc->anim_rect){
                int ax0, ay0, ax1, ay1;
                if(sc->anim_rect(&st, &ax0, &ay0, &ax1, &ay1) && ax1 > ax0 && ay1 > ay0){
                    gfx_clip_set(ax0, ay0, ax1, ay1);
                    part = 1;
                }
            }
            sc->render(fb, &st, t);
            if(part) gfx_clip_reset();
            /* 渲染耗时一直没量过 —— 上屏压到 7ms 之后, 这才是剩下的大头。
             * 报给平台去打日志(shell 保持平台无关, 不自己写日志)。 */
            g_render_ms = plat_now_ms() - t_r0;
            if(g_fade_ms && t < g_fade_ms) shell_fade_scene(fb, 255 - (int)(t*255/g_fade_ms));
            if(ov_a > 0) shell_draw_overlay(fb, ov_a);
            plat_present();
        }
        g_dirty = 0; g_dirty_full = 0;   /* ⚠️ full 也必须清, 否则一次整页之后局部路径永远进不去 */
    }
    return 1;
}

#endif /* PCM_SHELL_C */
