/*
 * scene_btplay.c — 场景 #1: 蓝牙播放页(CarPlay 级)
 *
 * 按 2026-07-16 定稿设计的 800×480 精确坐标画。读 PcmState 的真实状态(歌名/进度/设备/音量),
 * 响应输入(旋钮调音量、上下曲、播放暂停)。**平台无关** —— Mac 里跑的和台架上跑的是这一份。
 */
#ifndef SCENE_BTPLAY_C
#define SCENE_BTPLAY_C

#include "gfx.c"
#include "../sys/shell_draw.c"   /* 外壳绘制(转场/弹层), 要在 pcm_shell.c 之前 */
#include "../sys/pcm_shell.c"

/* ================= 布局 v3「声部与节奏」=================
 * 用户对 v2 的评价是"**有点死板**"。五个独立设计方向 + 十五份评审把它还原成
 * 两个**可测量**的机械成因(不是"审美问题"):
 *
 *   ① **有效声部只剩 2 级。** v2 里歌手与设备名墨色都是 (156,165,181),
 *      专辑/时钟/时间都是 (132,140,156) —— 两级灰的 5551 index 只差 **3 台阶**,
 *      而背景 Bayer 抖动本身就在 ±1 台阶跳、抗锯齿边缘同样被抖
 *      ⇒ 有效分离只剩 2 台阶, 一臂距离上歌手和专辑就是同一个灰。
 *      八个字段挤进两级 ⇒ 眼睛无从判断先看哪个 ⇒ 只能一行行读 ⇒ 规格表。
 *   ② **竖向间隙是节拍器。** 实测墨迹净空隙 21/13/17/16/14,
 *      mean 16.2 / σ 2.78 / **CV 17.2%** / max÷min 1.6。等距 = 没有分组 = 没有节奏。
 *   ③ 连带的第三条: **视觉权重与信息重要度脱钩** —— 按墨量能量排,
 *      歌手(125k)排第 **7**, 排在专辑(263k)和设备名(267k)后面。
 *
 * 📐 v3 的解法(改坐标前先读完这几条, 别破坏它们):
 *   ① **五级声部阶梯**, 相邻两级 5551 index 差 **≥5**(见下面的构建断言)
 *   ② **间隙比例悬殊**: 28/16/83/10/35 ⇒ CV 75%, max÷min 8.3
 *      曲名+歌手用 16 贴成一坨、时间+进度条用 10 贴成一坨, 中间放 **83px 静默**
 *      ⇒ 页面从"7 条横带"变成"2 个块 + 1 条控制条", 扫完从 7 次注视降到 3 次
 *   ③ **两条轨道**: 左轨 x=72 / 右轨 x=728, 量度 656。
 *      专辑+流派挪到右轨、与歌手**共用基线 230** ⇒ 眼动路径变 Z 形而不是一条直线。
 *      ⚠️ 混排必须**基线对齐, 绝不居中对齐** —— 2× 和 1× 居中会同时破坏两条隐形线。
 *   ④ **按键排与文字共用同一条 656 量度**(随机墨左缘 72 / 循环墨右缘 728)。
 *      v2 的按键排是 150..649 而文字是 56..743, 所以它看起来像浮在另一张卡片上。
 *   ⑤ **柔光挪到右中真空区**。实测 v2 右半屏 x∈[500,800) 的背景 lum
 *      p90−p50 = **0.00**(n=137550, 半屏是一个常数), 而左半屏是 9.94 ——
 *      唯一那盏灯照在墨最多的角上, 最大的空区反而最死。
 *
 * 🗑️ 删掉的两样(都是减信息, 理由充分):
 *   · **律动条** —— 合成假波形, 却是全页唯一持续在动的东西, 占着最靠近路面扫视线的
 *     位置和 C_AMBER 这条语义色道。流媒体卡住时它照样跳舞。
 *   · **居中的 `3 / 12`** —— iPhone 常报 0/0(KB 已证不可靠), 且它是全页唯一的居中文本,
 *     把时间行做成了三栏表头。
 */

/* ---- 五级声部(全部落在 5551 网格上, 不会被量化改写) ----
 * 相邻 index 间距 5/5/5/6, ΔL* 11.9/14.9/15.6/20.1。 */
#define C_BG0     16,20,29       /* 背景(上下同色, 不恢复竖向渐变) */
#define C_BG1     16,20,29
#define C_BGR     16            /* 背景分量, 端点暗底要用 */
#define C_BGG     20
#define C_BGB     29
#define C_GLOW    58,120,190
#define C_DISPLAY 247,247,255    /* idx 30/30/31 · 曲名、播停键、进度端点 */
#define C_LEAD    206,214,231    /* idx 25/26/28 · 歌手、上下曲键、暂停态已播 */
#define C_AMBER   239,181,74     /* idx 29/22/9  · 播放态已播、进度填充、开启态模式键 */
#define C_BODY    165,173,189    /* idx 20/21/23 · 专辑、剩余时间、暂停态进度填充 */
#define C_MICRO   123,132,148    /* idx 15/16/18 · 设备名/蓝牙标/时钟/电量/流派/关闭态模式键 */
#define C_TRACK   74,82,99       /* idx 9/10/12  · 进度底槽 */

/* 🚨 构建期断言: 五级声部的相邻 index 差必须 ≥5。
 *   为什么要写成代码而不是只写在文档里 —— v2 的两级灰差 3 台阶这件事,
 *   在文档里写着"三级文字", 在屏幕上是同一个灰。**只有断言拦得住下次手滑**。 */
#define IDX_(v) ((v)>>3)
typedef char btplay_voice_gap_assert_[
    (IDX_(247)-IDX_(206) >= 5 && IDX_(206)-IDX_(165) >= 5 &&
     IDX_(165)-IDX_(123) >= 5 && IDX_(123)-IDX_(74)  >= 5) ? 1 : -1];

/* ---- 两条轨道 ---- */
#define RAIL_L   72              /* 左轨: 墨迹左缘 = 一个 3× 汉字宽 */
#define RAIL_R   728             /* 右轨: 右对齐终点 */
#define MEASURE  (RAIL_R - RAIL_L)   /* 656 —— 文字/进度条/按键排共用这一条量度 */

/* ---- 基线锚点表(不是 draw_y!) ----
 * 🚨 v2 的节奏被写在 draw_y 坐标系里: 排的是 104/40/56/28/68 看着有变化,
 *   但 3× 行的 draw_y 在基线上方 81px、1× 行只有 27px, 抵消之后**墨迹**净空隙
 *   实测是 21/13/17/16/14 —— 一个节拍器。节奏必须排在 baseline 上。 */
#define BASE_TOPBAR  70          /* 设备名/时钟/蓝牙标/电量 (1×) */
#define BASE_TITLE   164         /* 曲名 (3×) */
#define BASE_ARTIST  230         /* 歌手 (2×) — 专辑/流派共用这条基线, 在右轨 */
#define BASE_TIME    355         /* 已播 (2×) / 剩余 (1×, 右轨) */
#define BAR_X        RAIL_L
#define BAR_Y        368
#define BAR_W        MEASURE
#define BAR_H        8           /* v2 是 14。评委判 6px 会在白天强光下消失, 8 是下限 */
#define BAR_CY       372
#define CTL_CY       431         /* 五个键的竖向中心 */

/* 传输键中心 x: 88 / 292 / 400 / 508 / 712
 * 间距 204/108/108/204 —— 和竖向一样的"紧-紧-宽"节拍, 语义上也对:
 * 中间三个是传输(主), 两端是模式(次)。 */
#define CTL_X_SHUF   88
#define CTL_X_PREV   292
#define CTL_X_PLAY   400
#define CTL_X_NEXT   508
#define CTL_X_REP    712

/* 基线绘制封装 —— 所有文字都必须走它, 不许再直接调 gfx_text 传 draw_y。
 * 实测 baseline = draw_y + 27*scale(三处吻合: 360→387, 276→303, 132→213)。
 * ⚠️ 写成函数不是宏: 宏的实参个数在展开**之前**就定了, 而 C_BODY 这类三分量颜色宏
 *   会被当成一个实参 ⇒ "too few arguments"。函数调用里它正常展开成三个尾参。 */
static void text_base(int x, int base, const char *s, int sc, int r, int g, int b){
    gfx_text(x, base - 27*sc, s, sc, r,g,b);
}
static int text_base_lim(int x, int base, const char *s, int sc, int maxw, int r, int g, int b){
    return gfx_text_lim(x, base - 27*sc, s, sc, maxw, r,g,b);
}
/* 定点版(sc16: 16=1.0× 24=1.5×/36px 32=2.0×)。基线换算同样按定点走。 */
static int text_base_lim_s(int x, int base, const char *s, int sc16, int maxw, int r, int g, int b){
    return gfx_text_lim_s(x, base - 27*sc16/16, s, sc16, maxw, r,g,b);
}

/* 整数正弦表(±1000)。放最前面: 图标圆弧和唱片高光**两处都要用**,
 * 原来定义在唱片那一节里, 图标要用就是"用在定义之前"。
*/
static const short VSIN[91] = {
    0,17,35,52,70,87,105,122,139,156,174,191,208,225,242,259,276,292,309,326,
    342,358,375,391,407,423,438,454,469,485,500,515,530,545,559,574,588,602,616,629,
    643,656,669,682,695,707,719,731,743,755,766,777,788,799,809,819,829,839,848,857,
    866,875,883,891,899,906,914,921,927,934,940,946,951,956,961,966,970,974,978,982,
    985,988,990,993,995,996,998,999,999,1000,1000 };
static int vsin(int d){                      /* 返回 ±1000 */
    while(d < 0) d += 360;
    d %= 360;
    if(d <=  90) return  VSIN[d];
    if(d <= 180) return  VSIN[180-d];
    if(d <= 270) return -VSIN[d-180];
    return -VSIN[360-d];
}
static int vcos(int d){ return vsin(d + 90); }

/* 蓝牙符文(顶栏用的小号)。
 * 🚨 旧的 draw_bt_glyph 是给 280px 大砖里那个 52px 符文写的, 用 5px 硬边刷子沿路径盖圆盘
 *   ⇒ 斜线是阶梯, 而且小尺寸下会糊成一团。这里改用距离场描粗(和别的图标同一套原语),
 *   转角圆头、抗锯齿都是免费的。 */
static void draw_bt_mark(int cx, int cy, int s, int r, int g, int b, int a){
    float X=(float)cx, Y=(float)cy, S=(float)s;
    float stem[4] = { X, Y-S,  X, Y+S };
    float up[6]   = { X, Y-S,  X+S*0.72f, Y-S*0.5f,  X-S*0.5f, Y+S*0.25f };
    float dn[6]   = { X, Y+S,  X+S*0.72f, Y+S*0.5f,  X-S*0.5f, Y-S*0.25f };
    float w = S*0.20f; if(w < 1.2f) w = 1.2f;
    gfx_shape(stem,2,w,0,0,r,g,b,a);
    gfx_shape(up,  3,w,0,0,r,g,b,a);
    gfx_shape(dn,  3,w,0,0,r,g,b,a);
}
/* 细线段 —— 蓝牙符文那个宏是 5px 粗的, 小图标要细的 */
static void icon_line(int x1,int y1,int x2,int y2,int r,int g,int b,int a){
    int dx = x2>x1 ? x2-x1 : x1-x2;
    int dy = y2>y1 ? y2-y1 : y1-y2;
    int n = dx>dy ? dx : dy, i, ox, oy;
    if(n < 1) n = 1;
    for(i=0;i<=n;i++){
        int x = x1 + (x2-x1)*i/n, y = y1 + (y2-y1)*i/n;
        for(oy=-1;oy<=1;oy++) for(ox=-1;ox<=1;ox++)
            if(ox*ox+oy*oy <= 1) gfx_blend(x+ox, y+oy, r,g,b, a);
    }
}

/* 手机电量图标。🚨 **只有 5 档, 没有百分比** —— 所以画的是"格数"不是精确液位,
 * 免得用户以为那是真实读数。BATT_NONE 时**整个不画**(不许画空电池假装拿到了)。 */
static void draw_battery(int x, int y, int lvl){
    int W = 22, H = 11, fill = 0, r = 154, g = 163, b = 178;
    if(lvl == BATT_NONE) return;
    switch(lvl){
        case BATT_CRITICAL: fill = 3;  r=226; g=94;  b=82;  break;   /* 只有它变红 */
        case BATT_WARNING:  fill = 7;  break;
        case BATT_NORMAL:   fill = 13; break;
        case BATT_FULL:     fill = 18; break;
        case BATT_EXTERNAL: fill = 18; r=233; g=178; b=74;  break;   /* 外接电源 = 琥珀 */
    }
    gfx_rrect_ring(x, y, W, H, 3, r,g,b, 200);          /* 外框 */
    gfx_rect(x+W+1, y+3, 2, H-6, r,g,b);                /* 正极小突起 */
    if(fill > 0) gfx_rect(x+2, y+2, fill, H-4, r,g,b);
    if(lvl == BATT_EXTERNAL){                            /* 充电闪电 */
        icon_line(x+12, y+2, x+8,  y+6, 20,26,36, 255);
        icon_line(x+8,  y+6, x+13, y+6, 20,26,36, 255);
        icon_line(x+13, y+6, x+9,  y+9, 20,26,36, 255);
    }
}

/* ---- 按下反馈 ----
 * 车上点下去没有任何反馈, 人会怀疑"是不是没点到"然后连点 —— 而连点在蓝牙上就是连续换曲。
 * 做法: 按下时在控件底下垫一个柔和的圆(不是把图标本身变形, 那在 22px 上会糊)。
 * 🚨 **最短显示时长**: 一次快速点击的 down→up 可能只有几十毫秒, 而我们一帧要 ~40ms
 *   (渲染 27 + 上屏 12~25), 不设下限的话**按下那一帧可能根本没画出来就被抬起清掉了**。
 *   所以 up 来了也要等够 PRESS_MIN_MS 才熄。
 * 🚨 还要有**上限**: 万一 up 事件丢了(触摸源是我们镜像读的, 不保证不漏),
 *   不设上限就会永久亮着。两头都夹住。 */
#define PRESS_MIN_MS 120
#define PRESS_MAX_MS 700
enum { PRESS_NONE=0, PRESS_SHUFFLE, PRESS_PREV, PRESS_PLAY, PRESS_NEXT, PRESS_REPEAT };
static int      g_press = PRESS_NONE;
static unsigned g_press_t0 = 0;
static int      g_press_released = 0;    /* 手指已抬起, 但还没到最短显示时长 */

/* 该不该继续显示按下态 —— 同时负责到点自动熄灭 */
/* ⚠️ 用 plat_now_ms() 而不是 render 传进来的 t_ms —— 后者是**场景相对时间**(切页会归零),
 * 事件侧拿不到同一个基准, 混用会算出巨大的 dt 让光晕当场熄掉。 */
static int press_active(void){
    unsigned dt, now = plat_now_ms();
    if(g_press == PRESS_NONE) return 0;
    dt = now - g_press_t0;
    if(dt >= PRESS_MAX_MS){ g_press = PRESS_NONE; return 0; }          /* 兜底: up 丢了 */
    if(g_press_released && dt >= PRESS_MIN_MS){ g_press = PRESS_NONE; return 0; }
    return 1;
}
/* ================= 图标一套(全部走 gfx_shape 距离场) =================
 * 📐 参考用户给的 iMusic 设计图。它那套的关键不是形状而是**圆润**:
 *   三角的三个角是圆的、线条端点是圆头、转角是圆的。手画尖角怎么调都像廉价货。
 * 统一参数(一套图标要像一套, 就靠这两个数一致):
 *   ICON_R    实心图形的圆角半径
 *   STROKE_R  描边图形的线半径(线宽 = 2×)
 * 骨架坐标都写成"以图标中心为原点"的相对值, 尺寸靠 ICON_H 缩放。 */
#define ICON_R    2.4f
#define STROKE_R  1.9f

/* 实心圆角三角。dir=+1 朝右 / -1 朝左。w/h 是**外轮廓**尺寸(已含圆角) */
static void ico_tri(float cx, float cy, float w, float h, int dir,
                    int r, int g, int b, int a){
    /* 骨架 = 外形往里缩 ICON_R, 描粗后正好回到外形 */
    float hw = w*0.5f - ICON_R, hh = h*0.5f - ICON_R;
    float p[6];
    p[0]=cx-dir*hw; p[1]=cy-hh;
    p[2]=cx+dir*hw; p[3]=cy;
    p[4]=cx-dir*hw; p[5]=cy+hh;
    gfx_shape(p, 3, ICON_R, 1, 1, r,g,b,a);
}
/* 可指定圆角半径的实心三角。ico_tri 固定用 ICON_R=2.4, 那是给 16×27 的传输键三角定的;
 * 随机/循环上的小箭头只有 7~9px, 2.4 的圆角会把尖端整个抹圆, 必须能调。 */
static void ico_tri_r(float cx, float cy, float w, float h, int dir, float rad,
                      int r, int g, int b, int a){
    float hw = w*0.5f - rad, hh = h*0.5f - rad;
    float p[6];
    p[0]=cx-dir*hw; p[1]=cy-hh;
    p[2]=cx+dir*hw; p[3]=cy;
    p[4]=cx-dir*hw; p[5]=cy+hh;
    gfx_shape(p, 3, rad, 1, 1, r,g,b,a);
}

/* 上/下一曲 = 两个实心圆角三角(按参考图, **不带竖线**) */
static void draw_skip(int cx, int cy, int dir, int r, int g, int b, int a){
    const float W = 16.0f, H = 27.0f, GAP = 4.0f;
    float off = (W + GAP) * 0.5f;
    ico_tri((float)cx - dir*off, (float)cy, W, H, dir, r,g,b,a);
    ico_tri((float)cx + dir*off, (float)cy, W, H, dir, r,g,b,a);
}
/* 播放三角 / 暂停两条圆角竖条 */
static void draw_playpause(int cx, int cy, int playing, int r, int g, int b, int a){
    /* 📐 比例: 白圆半径 33, 里面的图形该占**直径的一半上下**才压得住 ——
     *   第一稿暂停条只有 22px 高、4px 宽, 在 66px 的圆里像两根细棍。
     *   现在 28 高 / 6.5 宽, 播放三角 26×28。 */
    if(playing){
        float bw = 9.0f, gap = 9.0f, hh = 21.0f - bw*0.5f;
        float p1[4], p2[4];
        p1[0] = (float)cx - (gap+bw)*0.5f;  p1[1] = (float)cy - hh;
        p1[2] = p1[0];                      p1[3] = (float)cy + hh;
        p2[0] = (float)cx + (gap+bw)*0.5f;  p2[1] = (float)cy - hh;
        p2[2] = p2[0];                      p2[3] = (float)cy + hh;
        gfx_shape(p1, 2, bw*0.5f, 0, 0, r,g,b,a);
        gfx_shape(p2, 2, bw*0.5f, 0, 0, r,g,b,a);
    } else {
        ico_tri((float)cx + 2.5f, (float)cy, 36.0f, 42.0f, +1, r,g,b,a);
    }
}
/* ---- 随机 / 循环: 细线 + 实心小箭头 ----
 * 🚨 2026-08-14 用户判"不精致 / 线条太粗", 附了参考图。放大 6 倍看自己的:
 *   线半宽 1.9 ⇒ **3.8px 的线画在 24px 的图标上**, 两条交叉线在中间糊成一个疙瘩,
 *   箭头又是用折线描出来的"开口 V", 跟线本身粘在一起 —— 整个读成一个胖 X。
 * 修三处:
 *   ① 线半宽 1.9 -> **1.15**(2.3px), 图标同时放大一点补回视觉重量
 *   ② 箭头从"描边折线"改成**实心三角**(ico_tri_r, 圆角只留 1.1)—— 参考图里就是实心的
 *   ③ 循环的环放大到 24×17 且线变细 ⇒ 中间的孔才看得出来, 不再是个实心块
 * ⚠️ 线细了对比度会掉(抗锯齿把边缘像素冲淡), 所以颜色要偏亮:
 *   关态 158,167,182 而不是调色板里的 C_MICRO(123,132,148)。 */
#define ICO_STROKE 1.15f

static void draw_shuffle(int cx, int cy, int on){
    int R = on ? 239 : 158, G = on ? 181 : 167, B = on ? 74 : 182, A = 255;
    float X=(float)cx, Y=(float)cy;
    const float W = 15.0f, H = 7.0f, TIP = 8.0f;   /* TIP = 箭头长度 */
    float ex = X + W - TIP;                        /* 折线终点 = 箭头底 */
    /* 左上 -> 右下 */
    float a1[8] = { X-W, Y-H,  X-W+5, Y-H,  ex-1, Y+H,  ex, Y+H };
    /* 左下 -> 右上 */
    float a2[8] = { X-W, Y+H,  X-W+5, Y+H,  ex-1, Y-H,  ex, Y-H };
    gfx_shape(a1,4,ICO_STROKE,0,0,R,G,B,A);
    gfx_shape(a2,4,ICO_STROKE,0,0,R,G,B,A);
    ico_tri_r(ex + TIP*0.5f, Y+H, TIP, 9.5f, +1, 1.1f, R,G,B,A);
    ico_tri_r(ex + TIP*0.5f, Y-H, TIP, 9.5f, +1, 1.1f, R,G,B,A);
}
/* 循环: 细线圆角环 + 两个实心小箭头。mode==1 环心一个点(单曲循环)。
 * 🚨 环心不画 "1": 环内孔只有 ~13px, 而字形竖笔含线宽就占掉大半, 会焊成一体
 *   ⇒ "全部循环"和"单曲循环"分不出来。改成实心圆点, 判断从"认字形"降级成
 *   "孔里有没有东西", 一臂距离下才可靠。 */
/* 往点集里追加一段圆弧(角度用 vsin 的约定: 0=正上, 顺时针)。返回新的点数。 */
static int arc_append(float *p, int n, float cx, float cy, float r, int a0, int a1, int step){
    int a;
    for(a = a0; a <= a1; a += step){
        p[n*2]   = cx + r * vsin(a) / 1000.0f;
        p[n*2+1] = cy - r * vcos(a) / 1000.0f;
        n++;
    }
    if((a - step) != a1){                      /* 收尾精确落在终点 */
        p[n*2]   = cx + r * vsin(a1) / 1000.0f;
        p[n*2+1] = cy - r * vcos(a1) / 1000.0f;
        n++;
    }
    return n;
}

static void draw_repeat(int cx, int cy, int mode){
    int on = mode > 0;
    int R = on ? 239 : 158, G = on ? 181 : 167, B = on ? 74 : 182, A = 255;
    float X=(float)cx, Y=(float)cy, W=12.0f, H=8.5f, C=5.0f;
    float top[64], bot[64];
    int n;
    /* 🚨 2026-08-14 用户: "弧度明显有锯齿"。根因**不是抗锯齿没生效** ——
     *   原来每个圆角只有**一条直线切角**(从 (X-W,Y-H+C) 直连 (X-W+C,Y-H)),
     *   距离场把这条弦描粗出来就是个八边形, 转折处必然看到折角。
     *   现在每个角用 15° 步长的**真圆弧**(7 个点), 轮廓才是连续曲率。
     * 💰 代价: gfx_shape 是逐像素遍历所有线段, 段数从 4 涨到 ~18,
     *   但图标包围盒才 30×22 ≈ 660 像素, 总共 1.2 万次距离计算, 可以忽略。 */
    n = 0;
    top[0] = X-W; top[1] = Y;                                   /* 左侧中点 */
    n = 1;
    top[n*2] = X-W; top[n*2+1] = Y-H+C; n++;                    /* 上到左上圆角起点 */
    n = arc_append(top, n, X-W+C, Y-H+C, C, 270, 360, 15);      /* 左上角: 左 -> 上 */
    top[n*2] = X+W-C; top[n*2+1] = Y-H; n++;                    /* 上边 */
    n = arc_append(top, n, X+W-C, Y-H+C, C,   0,  90, 15);      /* 右上角: 上 -> 右 */
    top[n*2] = X+W; top[n*2+1] = Y; n++;                        /* 到右侧中点(缺口) */
    gfx_shape(top, n, ICO_STROKE, 0, 0, R,G,B,A);

    n = 0;
    bot[0] = X+W; bot[1] = Y;
    n = 1;
    bot[n*2] = X+W; bot[n*2+1] = Y+H-C; n++;
    n = arc_append(bot, n, X+W-C, Y+H-C, C,  90, 180, 15);      /* 右下角: 右 -> 下 */
    bot[n*2] = X-W+C; bot[n*2+1] = Y+H; n++;
    n = arc_append(bot, n, X-W+C, Y+H-C, C, 180, 270, 15);      /* 左下角: 下 -> 左 */
    bot[n*2] = X-W; bot[n*2+1] = Y; n++;
    gfx_shape(bot, n, ICO_STROKE, 0, 0, R,G,B,A);

    /* 两个实心小箭头在缺口处 */
    ico_tri_r(X+W-0.5f, Y-H, 7.5f, 9.0f, +1, 1.0f, R,G,B,A);
    ico_tri_r(X-W+0.5f, Y+H, 7.5f, 9.0f, -1, 1.0f, R,G,B,A);
    /* 单曲循环: 环心一个点。不画 "1" —— 环内孔只有 ~13px, 字形竖笔含线宽占掉大半会焊成一体,
     * "全部循环"和"单曲循环"就分不出来了。 */
    if(mode == 1) gfx_circle((int)X, (int)Y, 2, R,G,B);
}

/* 按下光晕。
 * 🎨 用**冷白**而不是琥珀: 重复/随机在"开"的时候图标本身就是琥珀, 琥珀光晕垫在琥珀图标下
 *   等于没对比。冷白对灰图标和琥珀图标都拉得开, 也是各家触摸涟漪的通用做法。
 * 🚨 第一版峰值 70、半径 41, 实测**可见环带最大色差只有 17/255 = 肉眼没有**
 *   (量法: 按下前后各存一帧, 逐像素比 r=34..41 的环带)。
 *   两个错: ①峰值太低 ②光都堆在圆心, 而圆心被不透明的播放圆整个盖住 ——
 *   **真正看得见的只有图标外面那一圈**, 亮度得按那里算。
 *   现在峰值 175, 半径给到图标外 13px 左右, 内边缘实测色差 ~70。 */
static void draw_press_halo(int cx, int cy, int rad){
    int x, y, r2 = rad*rad;
    for(y = -rad; y <= rad; y++) for(x = -rad; x <= rad; x++){
        int d2 = x*x + y*y;
        if(d2 < r2){
            int a = 175 - d2*175/r2;                /* 心 175 -> 边 0, 线性 */
            if(a > 0) gfx_blend(cx+x, cy+y, 205,220,240, a);
        }
    }
}

/* mm:ss 格式化。neg=1 时前面加一个减号(剩余时间用)。返回写了多少字符。 */
static int fmt_mmss(char *b, int secs, int neg){
    int p = 0;
    if(secs < 0) secs = 0;
    if(neg) b[p++] = '-';
    b[p++] = (char)('0' + secs/60/10%10); b[p++] = (char)('0' + secs/60%10);
    b[p++] = ':';
    b[p++] = (char)('0' + secs%60/10);    b[p++] = (char)('0' + secs%60%10);
    b[p] = 0;
    return p;
}


/* ================= 备选版式(编译期 -DBT_VARIANT=n 切换) =================
 * v3 判据全绿但用户判"更丑了" —— 说明判据是代理指标, 不是目标。
 * 用户当初说的是"死板"(rigid), **不是"太满"**; 我却把它改空了, 方向反了。
 * 这块屏在原厂那套密集界面里, 太疏会读成"没画完"。
 * 下面两版都保留 v3 真正有效的部分(五级声部 + 有轻重的间距), 但把密度还回来。 */

/* 专辑 · 流派 —— 一行, 左对齐, 先给流派留位置(v1 踩过的坑) */
static void draw_meta_line(const PcmState *st, int x, int base, int maxw){
    int gw = st->genre[0] ? gfx_text_w(st->genre, 1) : 0;
    int tx = x, room = maxw;
    if(!st->album[0] && !st->genre[0]) return;
    if(st->album[0]){
        if(gw) room -= (18 + (gw > 160 ? 160 : gw));
        if(room < 60) room = 60;
        tx = text_base_lim(x, base, st->album, 1, room, C_BODY);
    }
    if(st->album[0] && gw && x + maxw - tx > 42){
        gfx_circle(tx + 9, base - 8, 2, C_MICRO);
        tx += 18;
    } else if(st->album[0]) gw = 0;
    if(gw && x + maxw - tx >= 24)
        text_base_lim(tx, base, st->genre, 1, x + maxw - tx, C_MICRO);
}

/* 五个传输键 + 按下光晕。xs[] 给五个中心 x。 */
static void draw_transport(const PcmState *st, const int *xs, int cy){
    if(press_active()){
        switch(g_press){
            case PRESS_SHUFFLE: draw_press_halo(xs[0], cy, 30); break;
            case PRESS_PREV:    draw_press_halo(xs[1], cy, 32); break;
            case PRESS_PLAY:    draw_press_halo(xs[2], cy, 48); break;
            case PRESS_NEXT:    draw_press_halo(xs[3], cy, 32); break;
            case PRESS_REPEAT:  draw_press_halo(xs[4], cy, 30); break;
            default: break;
        }
    }
    { int v; draw_shuffle(xs[0], cy, pcm_shuffle(st, &v) && v > 0); }
    draw_skip(xs[1], cy, -1, C_LEAD, 255);
    draw_playpause(xs[2], cy, st->play_state == PLAY_PLAYING, C_DISPLAY, 255);
    draw_skip(xs[3], cy, +1, C_LEAD, 255);
    { int v; draw_repeat(xs[4], cy, pcm_repeat(st, &v) ? v : 0); }
}

/* ================= v1 复原(用户 2026-08-14 要求把这版拿回来) =================
 * 用户原话: "还不如以前那版, 你把以前那版弄回来吧, **有蓝牙logo, 有eq波浪的**"。
 * 那是 v1: 左边一块 280×280 的 art tile(里面是大蓝牙符文 + 五根律动柱), 右边一整栏信息。
 * 后来 v2 把这块砖删了, 理由是"占 20.4% 屏幕而里面只有 3.23% 有墨" —— 那个理由**只算了墨量,
 * 没算构图**: 砖不是用来装信息的, 它是这一页的视觉主体。删掉之后就只剩排版, 之后五版都在
 * 同一具骨架上换漆, 用户连否五次。这条教训值钱: **可测量的密度 ≠ 好看**。
 *
 * 源码是从会话记录里原样捞回来的, 不是凭记忆重写(这个仓库没开 git)。 */

/* v1 的蓝牙符文: 粗线段沿路径盖圆盘。比 draw_bt_mark 糙, 但在 52px 上就是要这个分量。 */
static void draw_bt_glyph_v1(int cx, int cy, int sz){
    int i, n;
    #define V1LINE(x1,y1,x2,y2) do{ \
        int _n = ((x2)-(x1))>0?((x2)-(x1)):-((x2)-(x1)); \
        int _m = ((y2)-(y1))>0?((y2)-(y1)):-((y2)-(y1)); \
        n = _n>_m?_n:_m; if(n<1) n=1; \
        for(i=0;i<=n;i++){ \
            int _x=(x1)+((x2)-(x1))*i/n, _y=(y1)+((y2)-(y1))*i/n, dx,dy; \
            for(dy=-2;dy<=2;dy++) for(dx=-2;dx<=2;dx++) \
                if(dx*dx+dy*dy<=5) gfx_blend(_x+dx,_y+dy,143,184,232,225); \
        } }while(0)
    V1LINE(cx, cy-sz, cx, cy+sz);
    V1LINE(cx, cy-sz, cx+sz*3/4, cy-sz/2);
    V1LINE(cx+sz*3/4, cy-sz/2, cx-sz/2, cy+sz/4);
    V1LINE(cx, cy+sz, cx+sz*3/4, cy+sz/2);
    V1LINE(cx+sz*3/4, cy+sz/2, cx-sz/2, cy-sz/4);
    #undef V1LINE
}


#define V1_ART_X 56
#define V1_ART_Y 100
#define V1_ART_S 280
#define V1_COL_X 392
#define V1_COL_W 368

/* ================= 黑胶唱片 =================
 * 🚨 第一个要避开的坑: **圆盘是旋转对称的, 画个转的圆盘根本看不出它在转**。
 *   真实黑胶能看出转, 靠的是两样不对称特征:
 *     ① 盘面反光 —— 两道对置的高光。⚠️ 注意光源是不动的, 转的是盘,
 *        但视觉上高光**跟着盘走**才读得出旋转(纯物理的话高光不动, 那就白转了)。
 *     ② 中心标签上的一个记号 —— 最直接的"这玩意在转"。
 * 💰 第二个坑是成本。唱片要 12.5fps 连续重画, 所以每个像素只能做**常数级**的活:
 *   · 沟纹: 一张 101 项的查表(索引 = d2>>6), **每像素一次查表**, 不开方不做除法
 *   · 高光: 不做逐像素角度计算(要 atan2), 改成沿弧线**刷一串软圆点**(6000 像素搞定)
 *   整盘一次 ≈ 2.6 万像素, 只有全屏的 6.7%。
 * 🎵 转速 = 33⅓ rpm(1.8 秒一圈), 和真唱片一样。**暂停就停** —— 静止本身就是读数。 */
#define VIN_CX (V1_ART_X + V1_ART_S/2)      /* 196 */
#define VIN_CY 240      /* 砖(100..380)的正中 */
#define VIN_R  84
#define VIN_LUT_N ((VIN_R*VIN_R >> 6) + 2)   /* 沟纹/倒数表的长度, 必须跟着 VIN_R 走 */


static void draw_vinyl(int ang, int playing){
    /* 两张 101 项的表, 索引都是 d2>>6(d2 最大 6400) —— 建表时才开方, 之后每像素只查表。
     *   s_groove[] 沟纹明暗   s_invd[] = 65536/d, 给高光做归一化用 */
    /* 🚨 表长必须**从 VIN_R 推**, 不能写死。索引是 d2>>6, d2 最大 = VIN_R²,
     *   所以最大索引 = VIN_R²>>6。VIN_R=80 时是 100(写死 101 刚好够, 纯属巧合);
     *   2026-08-14 把唱片放大到 84 之后 d²=7056 ⇒ 索引到 **110**, 越界读 10 项垃圾:
     *   s_invd 越界让高光归一化算出超范围值 ⇒ 盘缘在高光那两瓣**饱和成纯白**
     *   (真屏实测 45°/225° 的 r=82 处是 (248,248,248))。s_groove 越界则让沟纹颜色乱。
     *   只在盘缘一圈发作, 而放大前一直没事 —— 典型的"改一个常量踩爆另一处假设"。 */
    static unsigned char s_groove[VIN_LUT_N];
    static unsigned short s_invd[VIN_LUT_N];
    static int s_ok = 0;
    int x, y, ux, uy;
    const int R2 = VIN_R*VIN_R, LAB2 = 30*30, LABR2 = 32*32, HOLE2 = 5*5, RIM2 = (VIN_R-2)*(VIN_R-2);
    const int SEC_IN = 21*21, SEC_OUT = 27*27;   /* 标签上那道旋转标记的内外半径² */
    if(!s_ok){
        int i;
        for(i = 0; i < VIN_LUT_N; i++){
            int d2 = i << 6, d = 0;
            while((d+1)*(d+1) <= d2) d++;
            /* 🚨 沟纹周期原来是 6px 明 + 6px 暗 = **12px 一圈**, 放大看是同心色带不是沟。
             *   改成 3+3 = 6px 一圈, 明暗差也收窄(21 vs 27, 约 5551 的一个量化台阶),
             *   靠 Bayer 抖动把中间值铺开。 */
            s_groove[i] = (unsigned char)(((d / 3) & 1) ? 20 : 15);
            s_invd[i] = (unsigned short)(d ? (65536 / d) : 0);
        }
        s_ok = 1;
    }
    ux = vsin(ang);  uy = -vcos(ang);          /* 高光方向, ±1000 */
    /* ---- 盘体 + 高光: 一次遍历, 每像素两次查表、四次乘法, **没有除法没有开方** ----
     * 高光 = 把像素方向和"盘面反光方向"做点积再平方 ⇒ 天然是**两瓣对置**的光带,
     *   正好是黑胶转起来的样子, 而且平方让中间亮、两侧平滑淡出, 不用做任何软化。
     * 🚨 上一版是沿弧线刷一串圆点, 放大看是一片麻点(像脏了)—— 那条路废弃。 */
    for(y = -VIN_R; y <= VIN_R; y++){
        int yy = VIN_CY + y, y2 = y*y;
        for(x = -VIN_R; x <= VIN_R; x++){
            int d2 = x*x + y2, idx, v, dn, sh;
            if(d2 > R2) continue;
            idx = d2 >> 6;
            if(idx >= VIN_LUT_N) idx = VIN_LUT_N - 1;      /* 双保险: 常量再改也不越界 */
            if(d2 <= HOLE2)  { gfx_px(VIN_CX+x, yy, 16,20,29);    continue; }   /* 中心孔 */
            if(d2 <= LAB2){                                                     /* 标签 */
                /* 🚨 原来在标签上点一个小黑圆当"转"的记号 —— 它和中心孔并排, 看着像**两个洞**。
                 *   改成一道**旋转的暗扇形**(真唱片标签上的印刷图案就是这个感觉):
                 *   复用同一个点积, 不额外开销。 */
                dn = (x*ux + y*uy) * s_invd[idx] >> 16;
                /* 收成一小段**弧**而不是从圆心切出去的扇形 —— 扇形看着像被咬了一口,
                 * 弧才像标签上的印刷图案。 */
                if(d2 > SEC_IN && d2 < SEC_OUT && dn > 760) gfx_px(VIN_CX+x, yy, 128, 92, 38);
                else                                        gfx_px(VIN_CX+x, yy, 176,132,58);
                continue;
            }
            if(d2 <= LABR2)  { gfx_px(VIN_CX+x, yy, 22,26,34);    continue; }   /* 标签外暗环 */
            if(d2 >= RIM2)   { gfx_px(VIN_CX+x, yy, 52,60,78);    continue; }   /* 盘缘亮边 */
            v   = s_groove[idx];
            dn  = (x*ux + y*uy) * s_invd[idx] >> 16;      /* ≈ 1000*cos(θ), ±1000 */
            sh  = (dn*dn) >> 14;                          /* 0..61 */
            /* 🚨 cos² 的光斑太宽, 整盘被抬成灰塑料。再平方一次 ⇒ **cos⁴**, 高光收窄、
             *   盘面大部分回到黑, 这才是黑胶。峰值也提到 78, 亮的地方更亮。 */
            sh  = sh*sh*78/(61*61);
            gfx_px(VIN_CX+x, yy, v + sh, v + 3 + sh*11/10, v + 9 + sh*13/10);
        }
    }
    /* 暂停: 压一层灰。静止 + 变暗, 两个通道都在说"停了" */
    if(!playing){
        for(y = -VIN_R; y <= VIN_R; y++){
            int yy = VIN_CY + y, y2 = y*y;
            for(x = -VIN_R; x <= VIN_R; x++)
                if(x*x + y2 <= R2) gfx_blend(VIN_CX+x, yy, 33,43,61, 120);
        }
    }
}


/* ================= 波形进度条 =================
 * 🎨 抄自用户给的网易云车机版设计: 进度条不是一条素杠, 是**音频包络的形状**,
 *   已播部分亮、未播部分暗。它同时是这一页最有记忆点的细节。
 * 🚨 诚实性: 我们**拿不到音频采样**(蓝牙音频直接进 DSP, 主控只做控制面),
 *   所以这条波形是按"歌名+歌手"哈希生成的**确定性包络** —— 同一首歌永远同一条,
 *   换歌才换。它不冒充频谱, 它是这首歌的"图形签名"。装饰合规(用户 08-14 已划界)。
 * 💰 成本: 73 根 3px 竖条 ≈ 1 万像素, 比原来那条 368×8 的实心杠还便宜。 */
#define WAVE_N    73          /* 根数 */
#define WAVE_BW   3           /* 每根宽 */
#define WAVE_PIT  5           /* 间距(宽3+空2) */
#define WAVE_MAXH 26

#ifdef BT_WAVEBAR
static void draw_waveform(int x0, int cy, int seed, int played_px, int playing){
    int i;
    for(i = 0; i < WAVE_N; i++){
        int xi = x0 + i*WAVE_PIT;
        unsigned r;
        int env, hgt, half;
        /* 包络: 两个频率叠加, 让它有起伏而不是一条直线 —— 真音乐的包络就是这样 */
        env = 58 + vsin(i*360/WAVE_N)*22/1000 + vsin(i*3*360/WAVE_N)*14/1000;
        /* 逐根扰动: 定点 LCG, 同一个 seed 永远同一条波形 */
        r = ((unsigned)seed ^ (unsigned)(i*2654435761u)) * 1664525u + 1013904223u;
        env = env * (72 + (int)((r >> 17) % 46)) / 100;
        hgt = WAVE_MAXH * env / 100;
        if(hgt < 3) hgt = 3;
        half = hgt / 2;
        if(xi + WAVE_BW <= x0 + played_px){
            if(playing) gfx_rrect(xi, cy - half, WAVE_BW, hgt, 1, C_AMBER);
            else        gfx_rrect(xi, cy - half, WAVE_BW, hgt, 1, C_BODY);
        } else {
            gfx_rrect(xi, cy - half, WAVE_BW, hgt, 1, C_TRACK);
        }
    }
}
#endif /* BT_WAVEBAR */

/* ================= 唱臂 =================
 * 🎨 抄自用户给的 QQ音乐 转盘设计: 那个转盘一眼就认得出, 靠的就是唱臂。
 * 🔑 而且它可以承载真信息: **唱臂随播放进度从盘缘往盘心移** —— 和真唱机一模一样,
 *   于是它是第二个进度读数, 不是纯装饰。暂停时抬起归位。 */
#ifdef BT_TONEARM
/* ⚠️ 用户 2026-08-14 判"唱臂有点多余", 默认不编。留着是因为它其实带真信息
 *   (臂随播放进度从盘缘往盘心走, 是第二个进度读数), 哪天想要就 -DBT_TONEARM。 */
static void draw_tonearm(int pos_pct, int playing){
    const int PX = 302, PY = 172;             /* 转轴(砖内, 唱片右上方) */
    float pts[4], hs[4];
    int r, tipx, tipy, ang;
    int dx, dy, len, nx, ny;
    if(playing){
        /* 接触点角度固定在 115°(右下方), 半径随进度从 74 收到 38 —— 和真唱机一样往内走 */
        r    = 78 - (78 - 40) * pos_pct / 100;
        ang  = 115;
        tipx = VIN_CX + r * vsin(ang) / 1000;
        tipy = VIN_CY - r * vcos(ang) / 1000;
    } else {
        tipx = PX - 6; tipy = PY + 104;       /* 抬起归位: 竖着停在盘外 */
    }
    /* 臂杆 */
    pts[0] = (float)PX;   pts[1] = (float)PY;
    pts[2] = (float)tipx; pts[3] = (float)tipy;
    gfx_shape(pts, 2, 3.0f, 0, 0, 150,158,176, 255);
    /* 唱头: 臂末端一小段**垂直于臂**的粗块 —— 这一笔是"这是唱臂不是一根棍"的关键。
     * 法向量 = 把臂方向转 90°, 整数算, 不用三角函数。 */
    dx = tipx - PX; dy = tipy - PY;
    len = dx*dx + dy*dy; { int q=0; while((q+1)*(q+1) <= len) q++; len = q ? q : 1; }
    nx = -dy * 9 / len; ny = dx * 9 / len;
    hs[0] = (float)(tipx - nx/2); hs[1] = (float)(tipy - ny/2);
    hs[2] = (float)(tipx + nx);   hs[3] = (float)(tipy + ny);
    gfx_shape(hs, 2, 4.0f, 0, 0, 186,194,210, 255);
    /* 转轴座: 外圈暗、内圈亮, 做出一点"金属柱"的体积 */
    gfx_circle(PX, PY, 12, 40, 47, 61);
    gfx_circle(PX, PY, 9,  92,100,118);
    gfx_circle(PX, PY, 5,  178,186,202);
}
#endif /* BT_TONEARM */

/* 那块砖: 圆角底 + 内柔光 + 蓝牙符文 + 五根律动柱。两个 v1 版式共用。 */
static void draw_v1_tile(const PcmState *st, unsigned t_ms){
    int cx = V1_ART_X + V1_ART_S/2;
    /* 🎵 2026-08-14 用户要求: 砖里的蓝牙标换成**转起来的黑胶唱片**。
     *   转速 33⅓ rpm(1.8s 一圈, 和真唱片一样), 暂停就停 —— 转不转本身就是播放状态的读数。
     *   原来那个 52px 蓝牙符文(draw_bt_glyph_v1)留着没删, 想换回去改这一行即可。 */
    gfx_rrect(V1_ART_X, V1_ART_Y, V1_ART_S, V1_ART_S, 36, 33,43,61);
    {   static int s_ang = 0;
        int playing = (st->play_state == PLAY_PLAYING);
        int ds = st->dur_ms/1000, ps = st->pos_ms/1000;
        int pct = (ds > 0) ? ps*100/ds : 0;
        if(pct < 0) pct = 0; if(pct > 100) pct = 100;
        (void)pct;
        /* 🚨 转速: 真唱片是 33⅓ rpm = 1.8s 一圈(t_ms/5), 但台架实测**看着偏快** ——
         *   因为我们只有 12.5fps, 1.8s 一圈意味着每帧跳 28.8°, 眼睛读成频闪而不是转动。
         *   用户连着调了四次: 1.8s -> 3.6s -> 5.8s -> 10s -> **12s 一圈**(33ms/度, 每帧 4.2°)。
         *   ⚠️ 已经完全脱离物理正确性(真唱片 1.8s)。这不是妥协, 是这块屏的正解:
         *   800×480 上唱片直径才 168px, 12.5fps, 一臂距离 —— 慢转才读得出"在转",
         *   快转只会读成频闪。**物理正确 ≠ 观感正确, 以观感为准。**
         *   顺带: 转得慢 ⇒ 每帧变化的像素更少 ⇒ 脏行搬运更省。
         *   ⚠️ 物理正确 ≠ 观感正确, 低帧率下要按观感调。 */
        if(playing) s_ang = (int)((t_ms / 33) % 360);   /* 33ms/度 ≈ **12 秒一圈** */
        draw_vinyl(s_ang, playing);
#ifdef BT_TONEARM
        draw_tonearm(pct, playing);                    /* 用户 08-14 判"有点多余", 默认关 */
#endif
    }
    /* 🗑️ 2026-08-14 删掉砖里的五根律动柱。
     * 理由不是"占地方": 这一页有两个"在放音"的指示 —— 唱片在转、右栏有波形进度条 ——
     * 而律动柱是其中**唯一假的那个**(合成波形, 流媒体卡住时照跳)。
     * 用户给的两张参考设计里也都没有它, 它的视觉角色被波形进度条接走了。
     * 要退回去: 恢复这一段 + 把 VIN_CY 改回 212、VIN_R 改回 80。 */
}

/* ---------- V1+ 同一个骨架, 把后来做出来的功能都装回去 ----------
 * v1 缺的: 真设备名 / 专辑·流派 / 随机·重复 / 手机电量 / 按下反馈 / 剩余时间 /
 *          抗锯齿图标 / 五级声部。这些是几周的活, 不该跟着版式一起退掉。 */
/* 🚨 控件坐标**只此一份**。绘制和命中区都从它算 ——
 * 2026-08-14 又踩了一次"画在新位置、点在旧位置": 版式从 v3 换回两栏后,
 * 五个键画在 408/486/576/666/744, 而命中区还留着 v3 的 88/292/400/508/712,
 * 于是点播放键(画在 576)什么都不发生, 点屏幕正中(400, 那儿画的是随机键)反而触发播放。
 * 文件里那句"命中区必须和绘制用同一组常量算"的注释, 上一版就写着, 我还是犯了 ——
 * **光写注释没用, 得让它在结构上做不到分叉。** */
/* ---- v1+ 版式的**唯一一份**几何常量。绘制、命中区、脏矩形全从这里推。 ---- */
#define CTL_CY_V1     418
#define V1_BAR_Y      312
#define V1_BAR_H      8
#define V1_TIME_BASE  348
static const int CTL_XS[5] = { 408, 486, 576, 666, 744 };   /* 随机/上一曲/播停/下一曲/循环 */
/* 命中边界 = 相邻中心的中点, 两端各外扩。
 * 🚨 2026-08-14 用户实测反馈: **这是块电阻屏**, 要压力、定位也糙, 按起来费劲。
 *   ⇒ 热区尽量大。横向已经是**相邻区互相贴着**(按中点切), 没有空隙可挖了;
 *     能扩的只有**竖向**和**两端外缘**。
 *   竖向从 386→356 再到屏底 = **124px 高**(图标本身才 34px, 上下各留 45px 余量)。
 *   上边界 356 是安全的: 时间行基线 348, 墨迹到 348 为止, 还差 8px。
 *   两端从 ±40 放到 ±52。 */
#define CTL_HIT_Y0 356
#define CTL_HIT_Y1 480
#define CTL_HIT_EDGE 52
static int ctl_hit(int x, int y){
    int i;
    if(y < CTL_HIT_Y0 || y >= CTL_HIT_Y1) return -1;
    for(i = 0; i < 5; i++){
        int lo = (i == 0) ? CTL_XS[0] - CTL_HIT_EDGE : (CTL_XS[i-1] + CTL_XS[i]) / 2;
        int hi = (i == 4) ? CTL_XS[4] + CTL_HIT_EDGE : (CTL_XS[i] + CTL_XS[i+1]) / 2;
        if(x >= lo && x < hi) return i;
    }
    return -1;
}


/* ================= 等待连接页 =================
 * 🚨 为什么单独做一个: 没有曲目时, 播放页会退化成"标题位一根灰条 + 两行空白 +
 *   空进度条配 00:00/--:-- + 五个按不动的键"。那看着**不是"在等连接", 是"这页坏了"**。
 *   而且五个死按钮会招人去按 —— 按了没反应比压根没有更糟。
 * ⇒ 没曲目时整页换成为等待设计的样子: 隐藏进度条/时间/传输键, 唱片保持静止,
 *   文案说清**现在该做什么**。
 *
 * 🔑 三种"没曲目"必须分开, 它们以前长得一模一样, 用户无从判断该干嘛:
 *   A 没手机连上          -> 去手机上连
 *   B 连上了但没在放       -> 显示设备名, 等就行
 *   C 手机在放但**车机音源不是蓝牙** -> 去切音源
 *   C 真发生过: 手机在放、页面一片空白, 查半天发现主机音源停在 FM。 */
#define STOCK_SLOT_BT  40           /* 原厂音源槽号: FM=11 AUX=26 BT=40 */

/* 这一拍该画播放页还是等待页 —— 判据只有一处, 三个回调都用它,
 * 免得 render 走等待页而 anim_ms/anim_rect 还按播放页算(那会白刷 12fps)。 */
static int bt_has_track(const PcmState *st){ return st->title[0] != 0; }

enum { BTW_NO_DEVICE = 0, BTW_IDLE, BTW_WRONG_SOURCE };

/* 没曲目时该显示哪一种。有曲目就不该调它。 */
static int bt_wait_kind(const PcmState *st){
    /* 槽号读得到、且明确不是蓝牙 ⇒ 音源不对。-1 = 还没读到, 不算数。 */
    if(st->stock_src_slot > 0 && st->stock_src_slot != STOCK_SLOT_BT) return BTW_WRONG_SOURCE;
    if(!st->device[0]) return BTW_NO_DEVICE;
    return BTW_IDLE;
}

static void render_bt_waiting(const PcmState *st, unsigned t_ms){
    const char *head, *sub;
    gfx_backdrop(11,14,20, 23,29,41, 180, 140, 330, 58,120,190, 120);
    /* 唱片**恒定静止**。不能直接把 st 传下去 —— 状态 C 里 play_state 是 PLAYING(手机确实在放),
     * 唱片就会转; 而这一页的意思是"我们这儿没有可放的东西", 转着是误导。
     * 造一份 play_state 清零的副本, 只用来画砖。 */
    {   PcmState still = *st;
        still.play_state = PLAY_PAUSED;
        draw_v1_tile(&still, t_ms);
    }

    /* 状态栏(等待态): 左边只放"蓝牙", 右半由 shell_statusbar 画。 */
    {   shell_statusbar(st, SB_R, C_MICRO);
        draw_bt_mark(SB_L + SB_ICON/2, SB_ICY, SB_ICON/2, C_MICRO, 255);   /* cx,cy 是中心 */
        sb_text(SB_L + SB_ICON + SB_GAP, T(STR_BLUETOOTH), C_MICRO);
    }

    switch(bt_wait_kind(st)){
        case BTW_WRONG_SOURCE:
            head = T(STR_BT_WRONG);
            sub  = T(STR_BT_WRONG2);
            break;
        case BTW_IDLE:
            head = st->device[0] ? st->device : T(STR_CONNECTED);
            sub  = T(STR_BT_IDLE);
            break;
        default:
            head = T(STR_BT_CONNECT);
            sub  = T(STR_BT_CONNECT2);
            break;
    }
    /* 主行用 36px(和曲名同一档 sc16=24), 让两个状态之间不跳版式 */
    /* 📐 对着砖的竖向中心(240)排, 不沿用播放页的 186/228 ——
     *   播放页下面还有专辑行/进度条/控件把版面压住, 等待页什么都没有,
     *   照抄那两个基线就会上重下空, 又是"没画完"的样子。
     *   两行墨迹 218..276, 中心 247 ≈ 砖心 240, 视觉上就平了。 */
    text_base_lim_s(V1_COL_X, 238, head, 24, V1_COL_W, C_DISPLAY);
    text_base_lim (V1_COL_X, 276, sub,  1,  V1_COL_W, C_MICRO);
    /* 🚫 这里**故意不画**进度条、时间、传输键 —— 没有可控的东西, 就不摆能按的东西。 */
}

static void render_v1_plus(const PcmState *st, unsigned t_ms){
    char buf[64];
    int ts, w2_;
    gfx_backdrop(11,14,20, 23,29,41, 180, 140, 330, 58,120,190, 120);
    draw_v1_tile(st, t_ms);
    /* 右栏顶: 蓝牙标 + 真设备名 + 电量 + 时钟 */
    /* 状态栏(播放态): 左边 = 蓝牙标 + 真设备名 + 手机电量, 右半由 shell_statusbar 画。
     * ⚠️ 这一页有**两处刊头**(等待态/播放态), 两处都必须走同一条路 ——
     *   2026-08-14 只改了一处, Mac 预览一眼看出来(播放态没显示音量)。 */
    {   const char *dev = st->device[0] ? st->device : T(STR_BLUETOOTH);
        int sw    = shell_statusbar(st, SB_R, C_MICRO);
        int avail = (SB_R - sw - SB_GRP) - (SB_L + SB_ICON + SB_GAP);
        int lv, bw = 0, nw, x = SB_L;
        /* 🎨 左半是**一组连排的东西**: 蓝牙标 → 设备名 → 手机电量。
         *   电量是**这个手机的**属性, 就该紧跟在设备名后面, 而不是按右边界去摆
         *   (那样它会悬在中间不挨着任何东西)。
         *   全组用状态栏字号 SB_TXT, 并对齐到同一条光学中线 SB_MID。 */
        if(pcm_battery(st, &lv)) bw = 26 + SB_GAP;
        draw_bt_mark(x + SB_ICON/2, SB_ICY, SB_ICON/2, C_MICRO, 255);      /* cx,cy 是中心 */
        x += SB_ICON + SB_GAP;
        nw = sb_text_w(dev);
        if(nw > avail - bw) nw = avail - bw;
        gfx_text_lim_s(x, SB_TOP, dev, SB_TXT, avail - bw, C_MICRO);
        if(bw) draw_battery(x + nw + SB_GAP, SB_ICY - 7, lv);
    }
    /* 🚨 曲名 36px(sc16=24 = 1.5×), 用户 2026-08-14 定的 —— 原来是 2×=48px, 太大。
     *   字库烤在 24px, 以前只有 24/48/72 三档, 36 落在中间只能重烤一份字库;
     *   现在文字缩放泛化成 1/16 定点(见 gfx.c), 分数倍直接可用, 不多一个字节。
     *   放不下就退到 24px(sc16=16), 不再退到 48。 */
    w2_ = st->title[0] ? gfx_text_w_s(st->title, 24) : 0;
    ts = (w2_ > 0 && w2_ <= V1_COL_W) ? 24 : 16;
    if(st->title[0]) text_base_lim_s(V1_COL_X, 186, st->title, ts, V1_COL_W, C_DISPLAY);
    else             gfx_pill(V1_COL_X, 154, 200, 6, 45,55,72);
    if(st->artist[0]) text_base_lim(V1_COL_X, 228, st->artist, 1, V1_COL_W, C_LEAD);
    draw_meta_line(st, V1_COL_X, 264, V1_COL_W);
    {   int ps = st->pos_ms/1000, ds = st->dur_ms/1000;
        int playing = (st->play_state == PLAY_PLAYING);
        int w = (ds > 0) ? V1_COL_W*ps/ds : 0;
        if(w < 0) w = 0; if(w > V1_COL_W) w = V1_COL_W;
#ifdef BT_WAVEBAR
        draw_waveform(V1_COL_X, 316, gfx_str_seed(st->title, st->artist), w, playing);
#else
        /* 素条。🚨 2026-08-14 用户从波形条**换回**这个, 理由值得记住:
         *   那条波形是按歌名哈希生成的, **和真实音频无关**(SH4 摸不到采样, 蓝牙音频直接进 DSP)。
         *   抽象色块没人会当成数据, 但一条波形摆在进度条位置上, 看着就像"这首歌的音频长这样"
         *   —— 它卡在"装饰"和"伪造数据"的界线上, 所以撤掉。
         *   代码留在 -DBT_WAVEBAR 里(哪天真能拿到包络再说)。 */
        gfx_rrect(V1_COL_X, V1_BAR_Y, V1_COL_W, V1_BAR_H, 4, C_TRACK);
        if(w >= V1_BAR_H){ if(playing) gfx_rrect(V1_COL_X, V1_BAR_Y, w, V1_BAR_H, 4, C_AMBER);
                           else        gfx_rrect(V1_COL_X, V1_BAR_Y, w, V1_BAR_H, 4, C_BODY); }
        if(w >= 14 && w <= V1_COL_W - 14){
            /* 端点夹一圈背景色暗底: C_AMBER lum 186 vs C_DISPLAY lum 248 只差 62,
             * 一臂距离 + 32 级色阶下白点会糊进琥珀。 */
            gfx_circle(V1_COL_X + w, V1_BAR_Y + V1_BAR_H/2, 7, C_BGR, C_BGG, C_BGB);
            gfx_circle(V1_COL_X + w, V1_BAR_Y + V1_BAR_H/2, 5, C_DISPLAY);
        }
#endif
        fmt_mmss(buf, ps, 0);  text_base(V1_COL_X, V1_TIME_BASE, buf, 1, C_LEAD);
        /* 右边是**总时长**, 不是剩余时间。
         * 🚨 用户 2026-07 就明确定过:"两个时间, 一个已经播放时长, 另一个应该是总时长"。
         *   08-14 那轮设计评审建议改成剩余(理由是"还剩多久不用心算"), 我照做了 ——
         *   **那是拿评审意见推翻用户已经做过的决定**, 不对。评审能提供论据, 不能替用户拍板。
         *   要再改回剩余, 得用户自己说。 */
        if(ds > 0) fmt_mmss(buf, ds, 0);
        else { buf[0]='-';buf[1]='-';buf[2]=':';buf[3]='-';buf[4]='-';buf[5]=0; }
        text_base(760 - gfx_text_w(buf,1), V1_TIME_BASE, buf, 1, C_BODY);
    }
    draw_transport(st, CTL_XS, CTL_CY_V1);
}

static void btplay_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    gfx_target(fb);
    /* 有曲目 = 播放页; 没曲目 = 等待页。判据用**曲名**而不是 play_state ——
     * 暂停在半首歌上仍然该显示完整播放页(只是唱片停转), 那不是"等待"。 */
    if(bt_has_track(st)) render_v1_plus(st, t_ms);
    else                 render_bt_waiting(st, t_ms);
}

static int btplay_event(const PcmEvent *ev, const PcmState *st){
    /* 音量已上移到外壳的全局处理(shell_global_key) —— 任何页面都生效, 场景不再各写一遍。 */
    if(ev->type == EV_KEY_DOWN){
        switch(ev->arg){
            case K_PLAY: plat_command(st->play_state==PLAY_PLAYING?CMD_PAUSE:CMD_PLAY,0); return 1;
            case K_NEXT: plat_command(CMD_NEXT,0); return 1;
            case K_PREV: plat_command(CMD_PREV,0); return 1;
        }
    }
    /* 手指抬起: 只标记, 不立刻熄 —— 让 press_active 保证最短显示时长。
     * (一次快点的 down→up 可能比一帧还短, 立刻熄的话那一帧根本没画出来。) */
    if(ev->type == EV_TOUCH_UP){ g_press_released = 1; return 0; }
    if(ev->type == EV_TOUCH_DOWN){
        /* 命中区由 ctl_hit() 从 CTL_XS[] 算 —— 和绘制**同一份坐标**, 结构上不可能再分叉。
         * 五个区最窄 **84px**、最矮 **124px** ⇒ 内切半径 42, 满足车载 ≥32 的要求。
         *   (78/94 是放大之前的数, 改了常量没改注释 —— 2026-08-14 文档审查抓到。)
         * 🚫 进度条不注册触摸 —— MME 侧没有 seek, 长得像能拖却不响应比没有更糟。 */
        int k = ctl_hit(ev->x, ev->y);
        if(k < 0) return 0;
        g_press = PRESS_NONE; g_press_released = 0; g_press_t0 = plat_now_ms();
        switch(k){
            case 0: g_press = PRESS_SHUFFLE;
                    { int v; plat_command(CMD_SET_SHUFFLE, (pcm_shuffle(st,&v) && v > 0) ? 0 : 1); } return 1;
            case 1: g_press = PRESS_PREV; plat_command(CMD_PREV,0); return 1;
            case 2: g_press = PRESS_PLAY;
                    plat_command(st->play_state==PLAY_PLAYING?CMD_PAUSE:CMD_PLAY,0); return 1;
            case 3: g_press = PRESS_NEXT; plat_command(CMD_NEXT,0); return 1;
            /* 重复只在 0→1→2 循环, 不碰 3/4(文件夹/子文件夹) —— 对蓝牙流没意义 */
            case 4: g_press = PRESS_REPEAT;
                    { int v; plat_command(CMD_SET_REPEAT, pcm_repeat(st,&v) ? (v + 1) % 3 : 1); } return 1;
        }
    }
    return 0;
}

/* v3 起**没有任何自走动画**: 规则是"所有运动必须由 pos_ms 派生"。
 * 数据停 ⇒ 画面停; 暂停 ⇒ 全页冻结 —— **静止本身就是"已暂停"的读数**, 不用认图标。
 * (v2 那五根律动条是合成假波形, 流媒体卡住时它照样跳舞, 已删。)
 * 唯一还需要主动重画的是按下光晕 —— 它的熄灭逻辑(press_active)只在渲染时才跑,
 * 不给它一个节拍的话, 暂停态下光晕会**永久留在屏上**。
 * 50ms 比 PRESS_MIN_MS(120) 短, 保证按下那一帧一定画得出来、也一定熄得掉。 */
static int btplay_anim_ms(const PcmState *st){
    /* 🚨 等待页**没有任何东西会动** —— 唱片是强制静止的, 没有进度条也没有传输键。
     *   不加这一条的话: 状态 C 里 play_state 是 PLAYING(手机确实在放),
     *   定时器照常 12.5fps 触发, 每秒白重画 12 次一个静止页面。
     *   动画由**画面上有没有动的东西**决定, 不是由播放状态决定。 */
    if(!bt_has_track(st)) return press_active() ? 50 : 0;
    /* 🚨 按下光晕期间必须有重画 —— 熄灭逻辑(press_active)只在渲染时才跑,
     *   不给节拍的话暂停态下光晕会**永久留在屏上**。50ms < PRESS_MIN_MS(120)。 */
    if(press_active()) return 50;
    /* 律动条是 t_ms 驱动的真动画, 播放时要连续重画。
     * 100ms = 10fps: 台架一次整页重画约 47ms(渲染 27 + 上屏 20), 再快会拖钝触摸;
     * 而下面的脏矩形把它压到整页的 10%, 所以 10fps 实际很便宜。
     * 暂停时柱子压平成静态图, 返回 0 —— 一拍都不多画。 */
    /* 局部重绘之后一帧只重画全屏的 **7.9%**(见 btplay_anim_rect), 成本从 47ms 掉到几毫秒
     * ⇒ 帧率可以从 10fps 提到 12.5fps, 步进更细、看着更顺, CPU 反而比以前低一个量级。 */
    return (st->play_state == PLAY_PLAYING) ? 80 : 0;
}

/* 稳态下会变的东西有两处: 砖里的律动条(10Hz)和右栏的进度+时间(1Hz)。
 * 它们左右分居, 但**竖向范围几乎重合** ⇒ 用一个横贯的扁矩形一次盖住两者,
 * 比拆成两块简单得多, 也不会画出两种结果。
 *   律动条  x 156..236  y 288..334(柱底 334, 最高 46)
 *   进度条  x 392..760  y 312..320(端点 309..323)
 *   时间行  x 392..760  ink 330..348
 * ⇒ 实算(按当前常量, 不是旧版式):
 *     窄档 (109,153)-(283,327) = 174×174 = 30276 px = 全屏的 **7.9%**
 *     宽档 (109,153)-(766,352) = 657×199 = 130743 px = 全屏的 **34%**
 *   ⚠️ 这里原来写着 1.4% / 9.6% —— 那是**律动条时代**的坐标(x148..244), 版式换成黑胶之后
 *     矩形大了一圈, 数字没跟着改。2026-08-14 文档审查抓到。
 *     ⇒ 宽档 34% 不算便宜, 但它每秒只走一次; 真正省的是另外 11 帧走窄档。
 * ⚠️ 按下光晕画在控件行(y≈418), 不在这个矩形里 ⇒ 那 120ms 必须走整页。 */
/* 脏矩形**从版式常量推**, 不许再手打数字。
 * 🚨 2026-08-14 教训: 换版式时脏矩形没跟上, 结果时间数字上半截是几秒前的、
 *   下半截是当前的(看着像被横线穿过), 矩形外的字干脆不刷。手打的数字必然会漂。
 * 两档:
 *   窄 = 只有砖里的唱片在转(每秒 11 帧走这条)
 *   宽 = 这一秒进度也变了(每秒 1 帧), 连右栏的进度条+时间一起盖
 * ⚠️ 按下光晕画在控件行, 两档都不含它 ⇒ press 期间返回 0 走整页。 */
static int btplay_anim_rect(const PcmState *st, int *x0, int *y0, int *x1, int *y1){
    static int last_drawn_s = -1;
    int now_s = st->pos_ms / 1000;
    if(press_active()) return 0;
    /* 等待页没有局部动画区 —— 返回 0 让外壳走整页(它本来也极少重画)。
     * 不返回 0 的话, 脏矩形是**播放页**那块, 会在等待页上反复刷一块空背景。 */
    if(!bt_has_track(st)) return 0;
    *x0 = VIN_CX - VIN_R - 3;  *x1 = VIN_CX + VIN_R + 3;
    *y0 = VIN_CY - VIN_R - 3;  *y1 = VIN_CY + VIN_R + 3;
    if(now_s != last_drawn_s){
        last_drawn_s = now_s;
        if(V1_COL_X - 10          < *x0) *x0 = V1_COL_X - 10;
        if(760 + 6                > *x1) *x1 = 760 + 6;
        if(V1_BAR_Y - 10          < *y0) *y0 = V1_BAR_Y - 10;
        if(V1_TIME_BASE + 4       > *y1) *y1 = V1_TIME_BASE + 4;
    }
    if(*x0 < 0) *x0 = 0; if(*y0 < 0) *y0 = 0;
    if(*x1 > SCR_W) *x1 = SCR_W; if(*y1 > SCR_H) *y1 = SCR_H;
    return 1;
}

/* ============ 开机自检: 把"画的"和"点的"对上 ============
 * 🚨 这是今天犯了**两次**的那一类 bug(画在新位置、点在旧位置)。
 *   上一版只在注释里写"命中区必须和绘制用同一组常量算" —— 没用, 照样犯。
 *   现在改成: 进场景时逐个验"每个键画的中心点, 落进它自己的命中区吗", 不对就打红字。
 *   代价是几次整数比较, 只在进场景时跑一次。**注释拦不住的, 断言能。** */
static void sc_logd(int v){                 /* 场景层要保持平台无关, 不用平台私有的 p_logd */
    char b[12]; int i = 11, neg = v < 0;
    b[i] = 0;
    if(neg) v = -v;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while(v && i > 1);
    if(neg && i > 0) b[--i] = '-';
    plat_log(b + i);
}

static void btplay_enter(void){
    int i, bad = 0;
    for(i = 0; i < 5; i++){
        int k = ctl_hit(CTL_XS[i], CTL_CY_V1);
        if(k != i){
            plat_log("🚨 控件自检失败: 第"); sc_logd(i);
            plat_log(" 个键画在 x="); sc_logd(CTL_XS[i]);
            plat_log(" 但命中区判成 "); sc_logd(k); plat_log("\n");
            bad = 1;
        }
    }
    /* 脏矩形必须真的盖住会变的东西, 否则会留残影 */
    {   PcmState s; int a,b,c,d;
        plat_read_state(&s);
        s.pos_ms = 0;                       /* 强制走"宽"那一档 */
        if(btplay_anim_rect(&s, &a, &b, &c, &d)){
            if(a > VIN_CX-VIN_R || c < VIN_CX+VIN_R || b > VIN_CY-VIN_R || d < VIN_CY+VIN_R){
                plat_log("🚨 脏矩形没盖住唱片\n"); bad = 1; }
            if(a > V1_COL_X || c < 760 || b > V1_BAR_Y || d < V1_TIME_BASE){
                plat_log("🚨 脏矩形没盖住进度条/时间行\n"); bad = 1; }
        }
    }
    /* 热区上边界不许压到时间行的墨(基线 348) —— 压上去会出现"点时间也换歌" */
    if(CTL_HIT_Y0 <= V1_TIME_BASE){
        plat_log("🚨 触摸热区上边界压到时间行了\n"); bad = 1;
    }
    if(!bad) plat_log("控件/脏矩形自检 ✓\n");
}

static const PcmScene SCENE_BTPLAY = {
    "btplay", STR_BT_TITLE, btplay_enter, 0, btplay_render, btplay_event, btplay_anim_ms, btplay_anim_rect
};

#endif /* SCENE_BTPLAY_C */
