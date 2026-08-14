/*
 * shell_draw.c — 外壳自己的绘制: 场景转场 + 弹层(音量条/提示)
 *
 * 为什么单独一个文件: pcm_shell.c 要平台无关且不依赖绘图库(它只管调度);
 * 这里才用 gfx。include 顺序 = gfx.c -> shell_draw.c -> pcm_shell.c(场景文件里串起来)。
 */
#ifndef SHELL_DRAW_C
#define SHELL_DRAW_C

/* 场景转场: 整屏压暗 a/255(a=255 全黑 -> 0 无影响) = 淡入 */
static void shell_fade_scene(u16_ *fb, int a){
    int i, n = SCR_W*SCR_H;
    if(a <= 0) return;
    if(a > 255) a = 255;
    for(i=0;i<n;i++){
        u16_ p = fb[i];
        int r=(p>>11)&31, g=(p>>6)&31, b=(p>>1)&31;
        r = r*(255-a)/255; g = g*(255-a)/255; b = b*(255-a)/255;
        fb[i] = (u16_)((r<<11)|(g<<6)|(b<<1)|1);
    }
}

/* ---- 弹层几何(顶部居中的胶囊) ---- */
#define OV_W   300
#define OV_H   64
#define OV_X   ((SCR_W - OV_W)/2)
#define OV_Y   ((SCR_H - OV_H)/2)    /* 居中 —— 配合背景压暗 = 标准车机"临时覆盖层"观感。
                                      * ⚠️ 早先试过塞顶部/底部"找空位": 800×480 根本没有不压东西的位置,
                                      *   压顶部盖设备芯片, 压底部盖播放键。正解是让场景给它让位。 */

/* 喇叭图标(矢量画, 免字库)。y = 图标顶部, 图标高 20。 */
static void shell_icon_speaker(int x,int y,int muted,int r,int g,int b,int a){
    int i,k;
    int cy = y + 10;                     /* 垂直中心 */
    /* 音箱方块 (x..x+6, 中心高 ±4) */
    for(i=0;i<7;i++) for(k=-4;k<=4;k++) gfx_blend(x+i, cy+k, r,g,b, a);
    /* 喇叭口: 从窄到宽的实心梯形 */
    for(i=0;i<8;i++){
        int hh = 4 + i;                  /* 4 -> 11 */
        for(k=-hh;k<=hh;k++) gfx_blend(x+7+i, cy+k, r,g,b, a);
    }
    if(muted){
        /* 静音: 一道粗斜杠盖过去 */
        for(i=0;i<20;i++){
            gfx_blend(x+2+i, cy-9+i, r,g,b, a);
            gfx_blend(x+3+i, cy-9+i, r,g,b, a);
            gfx_blend(x+2+i, cy-8+i, r,g,b, a*70/100);
        }
    } else {
        /* 两道圆弧声波(按角度取点, 不是散点) */
        for(k=-7;k<=7;k++){
            int dx = (49 - k*k);         /* 圆弧: x ∝ sqrt(r²-k²), 用平方近似 */
            dx = dx>0 ? (dx*7/49) : 0;
            gfx_blend(x+18+dx, cy+k, r,g,b, a*75/100);
            gfx_blend(x+18+dx+1, cy+k, r,g,b, a*45/100);
        }
        for(k=-11;k<=11;k++){
            int dx = (121 - k*k);
            dx = dx>0 ? (dx*7/121) : 0;
            gfx_blend(x+24+dx, cy+k, r,g,b, a*50/100);
            gfx_blend(x+24+dx+1, cy+k, r,g,b, a*28/100);
        }
    }
}

/* ============ 共用状态栏: 音量 + 时钟(右上角, 右对齐) ============
 * 🚨 为什么音量从"弹层"改成"常驻状态栏"(用户 2026-08-14 判断 + 实测机制):
 *   音量弹层活着的时候 `in_anim` 为真, 而外壳里局部重绘的条件带着 `!in_anim`
 *   ⇒ **整页重画**, 而且要持续弹层的整个生命周期(140+1200+320 = 1.66 秒)。
 *   台架实测: 蓝牙页渲染 43ms + 上屏 21ms, 一拍 64~89ms, 连续 20 多帧 ——
 *   唱片的转动变成一格一格, 整页 churn。用户描述是"拧音量画面会跳"。
 *   改成状态栏之后, 拧音量只是一次普通的状态变化 = **重画一帧**, 不是 1.7 秒连续重渲。
 * 📌 它只画不判: 音量拿不到(-1)就不画音量, 时钟拿不到就不画时钟 —— 不许编占位。
 * 返回它占了多宽(从 x_right 往左量), 调用方要往左排别的东西时用得上。 */
/* ============ 全局状态栏: 贯通一行 ============
 * 🎨 2026-08-14 用户连否两版才定的形态。前两版的错都在**结构**上, 不在配色:
 *   v1 "粗喇叭图标+裸数字" 塞进蓝牙页原有刊头 —— 把设备名挤成了 `Demo Ph⋯`(刊头本来就满了);
 *   v2 换成细条, 仍然是塞在原刊头里 —— 用户: "左右上下都不对齐, 丑爆了"。
 *   ⇒ 正解是**给它一整行**, 并且把对齐做成**共用常量**, 而不是每页各自手调 y 和边距。
 *     左边距 / 右边界 / 基线 / 高度全在下面这四个宏里, 页面只管"我在左边放什么"。
 * 📌 它只画不判: 音量拿不到不画音量, 时钟拿不到不画时钟 —— 不许编占位。 */
/* 🎨 三版才定的形态。前两版错在**结构**(往已经满了的刊头里塞东西), 这一版错在**分寸** ——
 *   拿 iOS/Android 状态栏的规矩对了一遍, 差的是这五条:
 *     ① 状态栏文字必须**明显小于正文**, 它该退到背景里, 不该跟内容抢注意力(我用了和正文一样的 24px)
 *     ② 真机状态栏**没有分隔线**; 加了线就从"状态栏"变成了"工具栏"
 *     ③ 图标高度统一对齐到**文字的字高**(我三个元素三个高度)
 *     ④ 间距是**固定节奏**: 组内窄、组间宽(我是 20/24/12/34 随手给的)
 *     ⑤ 整条要矮(我 64px 吃掉了 1/7 的屏)
 *   ⚠️ 唯一不照搬的是字号比例: iPhone 状态字约占屏高 1.8%, 换算到 480 高只有 8px ——
 *      车机是**一臂之外**看的, 照抄比例会不可读。所以取"比正文小一档"(18px vs 24px)。 */
/* 🔎 查了真机规格再定的(见 KB 注释末尾的出处):
 *   · Android 状态栏 **24dp 高, 系统图标也是 24dp** ⇒ **条高 ≈ 图标高, 几乎没有多余留白**。
 *     我上一版 46px 高配 18px 文字 = 2.5 倍字高的空档, 所以显得松垮。
 *   · 真机状态栏里**每个元素都有身份**: 要么是图标要么是文字, 电量 = 图标 + 可选百分比。
 *     我那条光秃秃的音量横条没有图标, 读起来像进度条不像音量 ⇒ 补一个小喇叭, 和电池同等视觉重量。
 *   · 字号比例上一版是对的: iOS 正文 17pt / 状态栏约 12pt ≈ 0.7×, 我 18/24 = 0.75×。 */
/* 🚨 **这一行的对齐必须由基线定义, 不能靠"墨迹中心"**(2026-08-14 量出来才想明白):
 *   我先前拿 `SB_BASE = SB_MID - 9` 硬猜字形在行盒里的位置, 实测偏了 4.5px ——
 *   图标按中线画、文字按行盒画, 等于两套坐标系。
 *   而且"墨迹中心"本身就是错的基准: `Demo Phone` 的 p 有降部, 墨迹会往下拖,
 *   同一行不同内容会得到不同的"中心"。**真机状态栏对的是基线。**
 *   ⇒ 下面全部从字体度量(SFONT_ASC)算出来: 文字坐基线, 图标按**字高带**居中。 */
#define SB_TXT   12                        /* 1/16 定点: 12 = 18px, 正文 24px 的 0.75× */
#define SB_ASC   (SFONT_ASC * SB_TXT / 16) /* 这个字号的上伸高 = 基线到行顶 */
#define SB_CAP   (SB_ASC * 2 / 3)          /* 大写字高(近似)—— 图标就按它做高 */
/* ★ 这一行的**基线**。整条状态栏靠它定位, 上面的留白 = SB_BL - SB_ASC - (行顶到墨迹的距离)。
 * 🎨 2026-08-14 用户: "状态栏的内容还是要跟 top 留出 padding, 包括设置按钮"。
 *   原来 27 ⇒ 墨迹从 y=11 起, 只离屏幕上沿 11px, 顶着边。改成 34 ⇒ 墨迹从 ~18 起。 */
#define SB_BL    34
#define SB_TOP   (SB_BL - SB_ASC)          /* 文字的 y(gfx_text_s 要的行顶) */
#define SB_ICY   (SB_BL - SB_CAP / 2)      /* 图标中心 y = 字高带的中点 */
#define SB_H     48                        /* 条高(含上方留白) */
#define SB_L     56
#define SB_R     744
#define SB_GAP   12                        /* 组内间距 */
#define SB_GRP   28                        /* 组间间距 */
#define SB_ICON  SB_CAP                    /* 图标高度 = 字高, 和文字等重 */

/* 状态栏里画一段文字: **只给基线**, 行顶自己算 —— 调用方不许再手写 y。 */
static int sb_text(int x, const char *s, int r, int g, int b){
    return gfx_text_s(x, SB_TOP, s, SB_TXT, r, g, b);
}
static int sb_text_w(const char *s){ return gfx_text_w_s(s, SB_TXT); }

/* 画状态栏右半(音量 + 时钟)。左半由页面画在 (SB_L, SB_BASE), 用 SB_TXT 号。
 * 返回右半占了多宽。 */
static int shell_statusbar(const PcmState *st, int x_right, int r, int g, int b){
    int x = x_right;
    char bf[8]; int h, m, i;

    if(pcm_clock(st, &h, &m)){
        bf[0]=(char)('0'+h/10); bf[1]=(char)('0'+h%10); bf[2]=':';
        bf[3]=(char)('0'+m/10); bf[4]=(char)('0'+m%10); bf[5]=0;
        x -= sb_text_w(bf);
        sb_text(x, bf, r,g,b);
        x -= SB_GRP;
    }
    /* 音量 = **小喇叭 + 细横条 + 数字**。
     *   喇叭给身份(一条裸横条读起来像进度条)、横条给一眼看出的量感、数字给精确值。
     * 🚨 数字用**固定宽度的槽**右对齐(按 "88" 的宽度), 不能按实际宽度排 ——
     *   否则音量从 9 跳到 10 的时候, 左边的横条和喇叭会跟着**整体位移**, 看起来像画面在抖。 */
    if(st->volume >= 0){
        const int VW = 40, VH = 3;
        int slot = sb_text_w("88");
        int fill = st->volume > 40 ? VW : st->volume * VW / 40;
        int by = SB_ICY - VH/2;
        int k, hh = SB_ICON/2;
        char vb[4]; int p2 = 0, v = st->volume;
        if(v >= 10) vb[p2++] = (char)('0' + v/10);
        vb[p2++] = (char)('0' + v%10); vb[p2] = 0;

        x -= slot;                                  /* 数字: 固定槽内右对齐 */
        sb_text(x + slot - sb_text_w(vb), vb, r,g,b);
        x -= 10;
        x -= VW;                                    /* 横条 */
        for(i = 0; i < VW; i++) for(k = 0; k < VH; k++) gfx_blend(x+i, by+k, r,g,b, 65);
        if(!(st->muted > 0))
            for(i = 0; i < fill; i++) for(k = 0; k < VH; k++)
                gfx_blend(x+i, by+k, 233,178,74, 235);
        x -= 9;
        x -= 11;                                    /* 喇叭 */
        for(i = 0; i < 4; i++) for(k = -hh/2; k <= hh/2; k++)
            gfx_blend(x+i, SB_ICY+k, r,g,b, 210);
        for(i = 0; i < 6; i++){ int e = hh*(i+2)/7;
            for(k = -e; k <= e; k++) gfx_blend(x+4+i, SB_ICY+k, r,g,b, 210); }
        x -= SB_GRP;
    }
    return x_right - x;
}

/* 画弹层。a = 整体不透明度 0..255 */
static void shell_draw_overlay(u16_ *fb, int a){
    int ow = OV_W, x0, y0 = OV_Y;
    gfx_target(fb);
    if(g_ov.kind == OV_NONE || a <= 0) return;
    /* toast 宽度按文字自适应(gauge 固定宽)。⚠️ 早先固定 300 宽 + 从左 24 起画,
     * 长文字直接溢出面板 —— 看起来像"文字被截断"。 */
    if(g_ov.kind == OV_TOAST){
        ow = gfx_text_w(g_ov.text,1) + 56;
        if(ow < 180) ow = 180;
        if(ow > SCR_W-80) ow = SCR_W-80;
    }
    x0 = (SCR_W - ow)/2;

    /* 背景压暗 —— 弹层不是"挤进空位", 是临时接管注意力。压暗随 alpha 一起进出。
     *
     * 🚨 只压**面板周围一圈**, 绝不整屏。台架实测(2026-08-05, 用户报"拧旋钮明显卡滞"):
     *   整屏压暗时压暗量随 alpha 变 ⇒ 每一帧全屏都跟影子不同 ⇒ 局部更新退化成整屏,
     *   日志实录 `搬行=233/480 上屏=62ms`(常态只有 27 行 / 8ms)。一次音量条 ≈ 2.3 个整屏。
     *   压暗的视觉作用本来也只在面板附近 —— 屏幕边角压不压 15% 根本看不出来。
     *   限制到面板 + 投影的包围盒后, 搬行数回到几十行。 */
    {
        int yy, xx, d = a*38/255;                  /* 最深压 15% */
        int bx0 = x0 - 40,        bx1 = x0 + ow + 40;
        int by0 = y0 - 40,        by1 = y0 + OV_H + 40;
        if(bx0 < 0) bx0 = 0;  if(bx1 > SCR_W) bx1 = SCR_W;
        if(by0 < 0) by0 = 0;  if(by1 > SCR_H) by1 = SCR_H;
        for(yy=by0; yy<by1; yy++){
            u16_ *row = fb + yy*SCR_W;
            for(xx=bx0; xx<bx1; xx++){
                u16_ p = row[xx];
                int r=(p>>11)&31, g=(p>>6)&31, b=(p>>1)&31;
                r = r*(255-d)/255; g = g*(255-d)/255; b = b*(255-d)/255;
                row[xx] = (u16_)((r<<11)|(g<<6)|(b<<1)|1);
            }
        }
    }
    /* 投影: 面板外圈渐隐, 让它浮起来 */
    {
        int k;
        for(k=1;k<=10;k++)
            gfx_rrect_ring(x0-k, y0-k+3, ow+2*k, OV_H+2*k, 18+k, 0,0,0, a*(11-k)*4/255);
    }

    /* 面板(深色胶囊 + 细边) */
    gfx_rrect_a(x0, y0, ow, OV_H, 18, 22,27,38, a*235/255);
    gfx_rrect_ring(x0, y0, ow, OV_H, 18, 255,255,255, a*22/255);

    if(g_ov.kind == OV_GAUGE){
        int bx = x0+62, by = y0+OV_H/2-4, bw = ow-62-58, bh = 8;
        int pct = g_ov.maxval>0 ? g_ov.val*100/g_ov.maxval : 0;
        int fw;
        if(pct<0)pct=0; if(pct>100)pct=100;
        fw = bw*pct/100;
        shell_icon_speaker(x0+24, y0+OV_H/2-10, g_ov.icon==2, 233,237,244, a);
        gfx_pill_a(bx, by, bw, bh, 60,68,84, a*90/100);
        if(fw > bh) gfx_pill_a(bx, by, fw, bh, 233,178,74, a);
        /* 数值 */
        {
            char n[8]; int p=0, v=g_ov.val;
            if(v>=10){ n[p++]='0'+(v/10)%10; }
            n[p++]='0'+v%10; n[p]=0;
            gfx_text_a(x0+ow-46, y0+OV_H/2-13, n, 1, 243,245,249, a);
        }
    } else {
        gfx_text_a(x0 + (ow - gfx_text_w(g_ov.text,1))/2, y0+OV_H/2-13, g_ov.text, 1, 243,245,249, a);
    }
}

#endif /* SHELL_DRAW_C */
