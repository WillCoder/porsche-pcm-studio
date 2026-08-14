/*
 * gfx.c — PCM Studio · 场景共用绘图库(平台无关, 纯 buffer)
 *
 * 所有场景都用这套原语画。Mac 和 SH4 用**同一份**, 所以 Mac 上看到的就是台架画出来的。
 * 像素格式 RGBA5551(R:15-11 G:10-6 B:5-1 A:bit0), 见 KB M-CANVAS-FMT。
 *
 * 字体走离线烤字模(coexist-app/mvp/ui_font.h, dev/bake_font.py 生成) —— 嵌入式标准做法,
 * 免运行时 TTF 光栅器, SH4 上也跑得动。
 */
#ifndef STUDIO_GFX_C
#define STUDIO_GFX_C

#include "../sys/pcm_sys.h"

static u16_ *G_FB = 0;                 /* 当前目标帧缓冲 */
static void gfx_target(u16_ *fb){ G_FB = fb; }

/* ============ 裁剪矩形(局部重绘的地基) ============
 * 🚨 问题: 场景 render 一帧 **27ms**(整页 384000 像素), 而 EQ 律动条只占 3312 像素(0.86%)。
 *   为了让它动就得 10fps 整页重画 ⇒ 约 40% CPU 花在 99% 不变的像素上。
 *   (上屏那边**早就便宜了** —— 脏行搬运只搬 30/480 行。贵的一直是 render。)
 * 💡 解法: 给 gfx 加裁剪矩形, 局部重绘 = **设好裁剪, 跑同一个 render 函数**。
 *   好处是**不复制任何绘制代码** —— 局部和整页走的是同一条路径, 不会画出两种结果。
 * 🔑 省时间的关键**不是** gfx_blend 里那句边界检查(那只省了写, 没省循环),
 *   而是**每个原语先拿自己的包围盒和裁剪框求交, 不相交就整个跳过**。 */
static int G_CX0 = 0, G_CY0 = 0, G_CX1 = SCR_W, G_CY1 = SCR_H;   /* 半开区间 [x0,x1) */
static void gfx_clip_set(int x0,int y0,int x1,int y1){
    if(x0 < 0) x0 = 0; if(y0 < 0) y0 = 0;
    if(x1 > SCR_W) x1 = SCR_W; if(y1 > SCR_H) y1 = SCR_H;
    G_CX0 = x0; G_CY0 = y0; G_CX1 = x1; G_CY1 = y1;
}
static void gfx_clip_reset(void){ G_CX0 = 0; G_CY0 = 0; G_CX1 = SCR_W; G_CY1 = SCR_H; }
static int  gfx_clip_full(void){ return G_CX0==0 && G_CY0==0 && G_CX1==SCR_W && G_CY1==SCR_H; }
/* 包围盒和裁剪框相交? 不相交 -> 调用方整个跳过 */
static int gfx_hit(int x0,int y0,int x1,int y1){
    return !(x1 <= G_CX0 || x0 >= G_CX1 || y1 <= G_CY0 || y0 >= G_CY1);
}

#define RGBA5551(r,g,b) ((u16_)((((r)>>3)<<11)|(((g)>>3)<<6)|(((b)>>3)<<1)|1u))

/* 🚨 颜色宏用法警告(2026-07-17 踩过, 查了半天):
 *   本项目的颜色宏(C_INK / R_CARD / H_AMBER ...)展开是**三个逗号分隔的值**, 不是一个值。
 *   所以**绝不能**写:   gfx_rrect(..., cond ? C_A : C_B)
 *   三元只吃到第一个值, 展开成 `cond?38:27, 50,72, 34,48` -> 参数整体错位。
 *   症状极具误导性: 只有**红分量**变化(因为只有第一个值被三元选中), 画出一片红棕,
 *   看起来像"配色错了/格式错了", 完全不指向真因。
 *   ✅ 正确写法: if(cond) gfx_rrect(..., C_A); else gfx_rrect(..., C_B);
 *   (或传单独的 r,g,b 变量) */

static void gfx_unpack(u16_ p,int*r,int*g,int*b){
    int R=(p>>11)&31, G=(p>>6)&31, B=(p>>1)&31;
    *r=(R<<3)|(R>>2); *g=(G<<3)|(G>>2); *b=(B<<3)|(B>>2);
}
/* ============ 有序抖动(治 RGBA5551 的色带) ============
 * 🚨 问题实测: 每通道只有 **32 级**(步长 8)。柔光跨度大、级差稀 ⇒ 摊成同心圆环。
 *   量法: 穿过柔光中心的一条 520px 水平线上, **只有 9 种颜色, 最长同色连段 280 像素**。
 *   40~280 像素宽的纯色块, 肉眼当然看得见一圈一圈。
 * 💊 药: 量化前按 Bayer 8×8 加一个 0..7 的位置相关偏移, 再 >>3。
 *   · 值本来就是 8 的整数倍(= 5551 能精确表示)⇒ 加 0..7 后仍落在同一级 ⇒ **纯色区不会起噪点**
 *   · 值落在两级之间 ⇒ 按比例在相邻两级间交替 ⇒ 平均值正确, 带边被打散成细密纹理
 *   代价: 一次查表 + 三次加法, 可忽略。
 * ⚠️ 必须用**屏幕坐标**做索引, 不能用循环变量 —— 否则背景缓存整块拷到别处时纹理会错位。 */
static const unsigned char G_BAYER8[64] = {
     0,32, 8,40, 2,34,10,42,   48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38,   60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41,   51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37,   63,31,55,23,61,29,53,21
};
static u16_ gfx_pack_d(int x,int y,int r,int g,int b){
    int d = G_BAYER8[((y & 7) << 3) | (x & 7)] >> 3;    /* 0..7, 与量化步长同量级 */
    r += d; if(r > 255) r = 255;
    g += d; if(g > 255) g = 255;
    b += d; if(b > 255) b = 255;
    return RGBA5551(r,g,b);
}
static void gfx_px(int x,int y,int r,int g,int b){
    if(!G_FB||x<G_CX0||x>=G_CX1||y<G_CY0||y>=G_CY1) return;
    G_FB[y*SCR_W+x] = gfx_pack_d(x,y,r,g,b);
}
/* alpha 混合(a=0..255), 软边全靠它 */
static void gfx_blend(int x,int y,int r,int g,int b,int a){
    int br,bg,bb;
    if(!G_FB||x<G_CX0||x>=G_CX1||y<G_CY0||y>=G_CY1||a<=0) return;
    if(a>255) a=255;
    gfx_unpack(G_FB[y*SCR_W+x],&br,&bg,&bb);
    /* 混合结果同样要抖动 —— 柔光是一层层 blend 出来的, 只抖 gfx_px 治不了它 */
    G_FB[y*SCR_W+x] = gfx_pack_d(x,y,(r*a+br*(255-a))/255,(g*a+bg*(255-a))/255,(b*a+bb*(255-a))/255);
}
static void gfx_rect(int x0,int y0,int w,int h,int r,int g,int b){
    int x,y;
    if(!gfx_hit(x0,y0,x0+w,y0+h)) return; for(y=y0;y<y0+h;y++) for(x=x0;x<x0+w;x++) gfx_px(x,y,r,g,b);
}
/* 圆角矩形, 角上 4×4 超采样抗锯齿 */
static void gfx_rrect(int x0,int y0,int w,int h,int rad,int r,int g,int b){
    int x,y,sx,sy;
    if(!gfx_hit(x0,y0,x0+w,y0+h)) return;
    if(rad<1){ gfx_rect(x0,y0,w,h,r,g,b); return; }
    for(y=0;y<h;y++) for(x=0;x<w;x++){
        int cx=-1,cy=-1,cov=0;
        if(x<rad&&y<rad){cx=rad;cy=rad;}
        else if(x>=w-rad&&y<rad){cx=w-rad-1;cy=rad;}
        else if(x<rad&&y>=h-rad){cx=rad;cy=h-rad-1;}
        else if(x>=w-rad&&y>=h-rad){cx=w-rad-1;cy=h-rad-1;}
        if(cx<0){ gfx_px(x0+x,y0+y,r,g,b); continue; }
        for(sy=0;sy<4;sy++) for(sx=0;sx<4;sx++){
            int ddx=(x*4+sx)-(cx*4+2), ddy=(y*4+sy)-(cy*4+2);
            if(ddx*ddx+ddy*ddy <= rad*rad*16) cov++;
        }
        gfx_blend(x0+x,y0+y,r,g,b,cov*255/16);
    }
}
/* 胶囊。⚠️ 圆角半径必须同时 <= w/2 和 h/2, 否则窄条(如 9px 宽的 EQ 柱)会画成畸形月牙。 */
static void gfx_pill(int x,int y,int w,int h,int r,int g,int b){
    int rad = h/2; if(rad > w/2) rad = w/2; if(rad < 1) rad = 1;
    gfx_rrect(x,y,w,h,rad,r,g,b);
}
/* 实心圆。边缘 4×4 超采样 -> 真抗锯齿(早先"柔化一档"在大圆上仍能看见台阶)。 */
static void gfx_circle(int cx,int cy,int rad,int r,int g,int b){
    int x,y,sx,sy;
    if(!gfx_hit(cx-rad-1,cy-rad-1,cx+rad+2,cy+rad+2)) return;
    int rr_in  = (rad-1)*(rad-1);
    int rr_out = (rad+1)*(rad+1);
    for(y=-rad-1;y<=rad+1;y++) for(x=-rad-1;x<=rad+1;x++){
        int d2 = x*x + y*y, cov;
        if(d2 > rr_out) continue;
        if(d2 <= rr_in){ gfx_blend(cx+x,cy+y,r,g,b,255); continue; }
        cov = 0;
        for(sy=0;sy<4;sy++) for(sx=0;sx<4;sx++){
            int fx = x*4 + sx - 2, fy = y*4 + sy - 2;      /* 子像素中心(×4 定点) */
            if(fx*fx + fy*fy <= rad*rad*16) cov++;
        }
        if(cov) gfx_blend(cx+x,cy+y,r,g,b,cov*255/16);
    }
}
/* 实心三角(播放/上下曲图标用)。dir: +1 右, -1 左。用扫描线 + 边缘覆盖。 */
static void gfx_tri(int cx,int cy,int half_h,int dir,int r,int g,int b){
    int y;
    if(!gfx_hit(cx-half_h-1,cy-half_h-1,cx+half_h+2,cy+half_h+2)) return;
    for(y=-half_h; y<=half_h; y++){
        int ay = y<0?-y:y;
        int w  = (half_h - ay) * 13 / 14;          /* 高:宽 ≈ 14:13 */
        int a  = (ay >= half_h-1) ? 150 : 255;     /* 尖角柔一点 */
        int x;
        for(x=0; x<=w; x++) gfx_blend(cx + dir*x, cy+y, r,g,b, a);
    }
}
/* 竖直渐变背景 + 一处柔光(座舱底) */
/* ============ 背景缓存 ============
 * 台架实测(2026-08-05): 场景 render 一帧 **83ms**, 而上屏只要 8ms —— 瓶颈已经是 CPU 画。
 * 背景(整屏渐变 + 柔光圆)是这里面的大头, 而且**同一场景每帧完全一样**:
 *   渐变 = 384000 次 gfx_px(带边界检查的函数调用);柔光 = 十万量级的 gfx_blend + 一次除法。
 * 所以按参数缓存一份, 命中就直接整块拷(RAM→RAM 走缓存, 实测 768KB 只要 ~7ms 量级)。
 * 12 个参数任一变化就重算 —— 三个场景各一套参数, 切页时重算一次, 之后一直命中。 */
static u16_ g_bd_cache[SCR_W * SCR_H];
static int  g_bd_p[13];
static int  g_bd_ok = 0;

static void gfx_backdrop_compute(int r0,int g0,int b0,int r1,int g1,int b1,
                                 int glow_x,int glow_y,int glow_r,int gr,int gg,int gb,
                                 int vig);

/* vig = 暗角强度 0..255(0 关)。
 * 🎨 为什么加它: 之前的底是一块**平涂**深灰蓝, 所以不管上面排得多整齐, 整体都显得薄。
 *   暗角让画面读起来像"一块被照亮的表面"而不是"一块填充色" —— 这是感知深度最便宜的来源。
 * 💰 成本为零: 背景整块按 13 个参数缓存, 暗角在缓存里算一次, 每帧只是拷贝。 */
static void gfx_backdrop(int r0,int g0,int b0,int r1,int g1,int b1,
                         int glow_x,int glow_y,int glow_r,int gr,int gg,int gb,
                         int vig){
    int p[13], i, same = g_bd_ok;
    u16_ *dst = G_FB;
    p[0]=r0; p[1]=g0; p[2]=b0; p[3]=r1; p[4]=g1; p[5]=b1;
    p[6]=glow_x; p[7]=glow_y; p[8]=glow_r; p[9]=gr; p[10]=gg; p[11]=gb;
    p[12]=vig;
    for(i=0;i<13;i++) if(p[i]!=g_bd_p[i]){ same=0; break; }
    if(!same){
        gfx_target(g_bd_cache);
        gfx_backdrop_compute(r0,g0,b0,r1,g1,b1,glow_x,glow_y,glow_r,gr,gg,gb,vig);
        gfx_target(dst);
        for(i=0;i<13;i++) g_bd_p[i]=p[i];
        g_bd_ok = 1;
    }
    if(dst != g_bd_cache){
        /* 🚨 局部重绘时**只拷裁剪到的行** —— 这是整条链上最大的一块:
         *   整屏 768KB 的 memcpy 本身就占了 render 的大头。EQ 那 46 行只要 1/10 的量。
         * gfx 必须平台无关 —— u32 是各平台后端的 typedef, 这里用 unsigned(两平台都 32 位)。 */
        /* 🚨 必须**同时按列裁**, 不能只按行。2026-08-14 踩过:
         *   局部重绘时这里把裁剪框覆盖的那几行**整屏宽**刷回背景,
         *   而裁剪框左右两侧的东西(比如左边那块封面砖)不会被重画 ⇒ 被抹出两条横带。
         *   旧版式恰好那几行左右没东西, 所以一直没露头 —— 换个版式立刻现形。 */
        const unsigned *a = (const unsigned*)(const void*)g_bd_cache;
        unsigned *b = (unsigned*)(void*)dst;
        int y, n2 = SCR_W/2;                          /* 每行的 u32 个数 */
        int c0 = G_CX0 >> 1, c1 = (G_CX1 + 1) >> 1;   /* 裁剪框换算成 u32 列 */
        if(c0 < 0) c0 = 0; if(c1 > n2) c1 = n2;
        for(y = G_CY0; y < G_CY1; y++){
            int off = y*n2;
            for(i=c0;i+4<=c1;i+=4){ b[off+i]=a[off+i]; b[off+i+1]=a[off+i+1];
                                    b[off+i+2]=a[off+i+2]; b[off+i+3]=a[off+i+3]; }
            for(;i<c1;i++) b[off+i]=a[off+i];
        }
    }
}

static void gfx_backdrop_compute(int r0,int g0,int b0,int r1,int g1,int b1,
                         int glow_x,int glow_y,int glow_r,int gr,int gg,int gb,
                         int vig){
    int x,y;
    for(y=0;y<SCR_H;y++){
        int t = y*256/SCR_H;
        int r = r0 + (r1-r0)*t/256, g = g0 + (g1-g0)*t/256, b = b0 + (b1-b0)*t/256;
        for(x=0;x<SCR_W;x++) gfx_px(x,y,r,g,b);
    }
    /* 暗角: 先于柔光画 —— 光必须压在暗角**上面**, 否则连光心都被压暗, 白做。
     * 用两条一维衰减相乘近似椭圆暗角: 省掉逐像素开方/除法, 而且角上压得比边上重,
     * 正好是想要的("四角沉下去, 中间浮起来")。 */
    if(vig > 0){
        static int s_fx[SCR_W], s_fy[SCR_H];
        static int s_f_ok = 0;
        if(!s_f_ok){
            int i, d;
            for(i=0;i<SCR_W;i++){ d = i*2 - SCR_W; if(d<0) d=-d; s_fx[i] = d*256/SCR_W; }
            for(i=0;i<SCR_H;i++){ d = i*2 - SCR_H; if(d<0) d=-d; s_fy[i] = d*256/SCR_H; }
            s_f_ok = 1;
        }
        for(y=0;y<SCR_H;y++){
            int fy2 = s_fy[y]*s_fy[y];
            for(x=0;x<SCR_W;x++){
                int t = (s_fx[x]*s_fx[x] + fy2) >> 9;      /* 0..256 */
                int a;
                if(t > 256) t = 256;
                a = vig * t * t / 65536;                   /* 二次 ⇒ 中心大片几乎不受影响 */
                if(a > 0) gfx_blend(x, y, 0,0,4, a);
            }
        }
    }

    /* 柔光: 严格圆形 + 平滑衰减到 0。
     * ⚠️ 早先版本用 d2*100/(r*r) 量化成 0..100 -> 边界处 a 跳变留下可见方块边, 已改成
     *   基于半径的三次衰减, 且 d>=r 直接跳过。 */
    if(glow_r>0){
        /* 🚨 两处真实开销, 都是台架级的(2026-08-05 审出来):
         *   ① rr 是运行期变量 ⇒ `d2*256/rr` 每个圆内像素都要走一次**软件除法**
         *      (SH4 无整数除法指令, 编译成 __sdivsi3_i4i 的 div1 链)。home 页一帧 36 万次。
         *      改成进函数时建一张衰减表(256 项), 之后全是查表。
         *   ② 循环范围是整个外接正方形, 但圆心常在屏边 ⇒ home 26% / radio 34% 的迭代
         *      整个落在屏外, 白算一次除法再白调一次 gfx_blend, 最后被边界检查丢掉。
         *      改成先跟屏幕求交。 */
        static int   s_lut[257];          /* lut[i] = 归一化距离 i/256 处的最终 alpha */
        static int   s_lut_ok = 0;
        int rr = glow_r*glow_r;
        /* ⚠️ 索引这一步**仍然用除法**, 不要"优化"成倒数乘法。
         * 试过 recip=(256<<16)/rr 再右移: Mac 端逐像素对拍抓到 926/384000 个像素不一致
         * (倒数取整让索引偶尔差 1)。差异肉眼看不见, 但改渲染就该逐字节等价, 不含糊。
         * 真正确定的收益是下面两条, 它们按构造就是精确的: ①屏内裁剪 ②查表省掉三次方计算。 */
        int y0 = -glow_r, y1 = glow_r, x0 = -glow_r, x1 = glow_r;
        if(y0 < -glow_y)          y0 = -glow_y;             /* 跟屏幕求交 */
        if(y1 > SCR_H-1-glow_y)   y1 = SCR_H-1-glow_y;
        if(x0 < -glow_x)          x0 = -glow_x;
        if(x1 > SCR_W-1-glow_x)   x1 = SCR_W-1-glow_x;
        if(!s_lut_ok){
            int i;
            for(i=0;i<=256;i++){
                int t2 = 256 - i;                  /* i = d2/rr 归一化到 0..256 */
                int a  = t2*t2/256; a = a*t2/256;  /* 三次衰减, 与原式逐位一致 */
                s_lut[i] = a*90/256;
            }
            s_lut_ok = 1;
        }
        for(y=y0;y<=y1;y++) for(x=x0;x<=x1;x++){
            int d2=x*x+y*y, a;
            if(d2 >= rr) continue;                 /* 圆外一律不碰 */
            a = s_lut[d2*256/rr];
            if(a > 0) gfx_blend(glow_x+x,glow_y+y,gr,gg,gb,a);
        }
    }
}


/* ============ 生成式底图(按歌曲生成的抽象"封面") ============
 * 🎨 为什么要它:
 *   所有好看的播放器都是被**封面**撑起来的。AVRCP 1.3 拿不到封面(五条独立证据),
 *   我们把那块地删了 ⇒ 整页只剩文字排版, 排得再准也只是"排得准", 画面没有主体。
 *   用户 2026-08-14 明确划界: 装饰性视觉不算"假渲染"(那条铁律管的是伪造数据/冒充原厂)。
 *   ⇒ 自己生成一张抽象底图: 它不声称代表任何东西, 就是氛围。
 *
 * 🚨 性能上的关键决定: **在 100×60 上画, 再双线性放大到 800×480**。
 *   直接在全屏上做三团柔光 = 三倍于现有单团柔光的代价(现有那团 r=380 就覆盖 45 万像素),
 *   换曲时会卡出可见的停顿。缩小到 1/8 只有 6000 个像素要算,
 *   而放大这一步本身就**天然把边缘糊开** —— 想要的柔和是免费的副产品, 不是额外开销。
 *
 * 🎯 配色不随机: 六组**手挑的暗色调**, 哈希只负责选哪一组 + 团块位置。
 *   纯随机 RGB 必然出难看的组合; 选色板保证下限。
 *   所有色都压在低亮度(最亮的分量 ≤ 112), 保证上面的文字对比度不塌。 */
#define ART_W 100
#define ART_H 60
static unsigned char g_art[ART_H * ART_W * 3];
static int g_art_seed = -0x7fffffff, g_art_vig = -1;

/* 六组暗色调: 每组两个色(主/辅)。挑的是"深夜车厢里好看"的方向, 不是鲜艳。 */
static const unsigned char ART_PAL[6][6] = {
    {  30, 34, 78,   78, 30, 88 },   /* 深蓝 → 紫 */
    {  16, 54, 58,   22, 74, 92 },   /* 墨绿 → 青 */
    {  74, 28, 38,   96, 54, 28 },   /* 酒红 → 棕金 */
    {  20, 42, 84,   46, 66,112 },   /* 靛蓝 → 钢蓝 */
    {  58, 24, 74,   98, 32, 74 },   /* 深紫 → 洋红 */
    {  26, 54, 42,   86, 70, 28 }    /* 森绿 → 暗金 */
};

/* 小图上画三团, 三次衰减 —— 和柔光同一套手感, 只是分辨率低 8 倍 */
static void art_render_small(int seed){
    unsigned h = (unsigned)seed;
    const unsigned char *pal = ART_PAL[h % 6];
    int i, x, y, k;
    int bx[3], by[3], br_[3];
    const unsigned char *cols[3];
    /* 底色: 主色压到很暗, 当整张图的地 */
    for(y = 0; y < ART_H; y++) for(x = 0; x < ART_W; x++){
        int o = (y*ART_W + x)*3;
        g_art[o]   = (unsigned char)(pal[0]/3 + 8);
        g_art[o+1] = (unsigned char)(pal[1]/3 + 10);
        g_art[o+2] = (unsigned char)(pal[2]/3 + 14);
    }
    /* 三团的位置/大小由哈希推, 但**限制在合理范围**免得出现全屏一坨或者三团重合 */
    for(k = 0; k < 3; k++){
        h = h*1664525u + 1013904223u;
        bx[k]  = (int)(h >> 16) % ART_W;
        h = h*1664525u + 1013904223u;
        by[k]  = (int)(h >> 16) % ART_H;
        h = h*1664525u + 1013904223u;
        br_[k] = 34 + (int)((h >> 16) % 30);          /* 半径 34..63(小图坐标) */
        cols[k] = (k == 1) ? pal + 3 : pal;           /* 中间那团用辅色 */
    }
    for(k = 0; k < 3; k++){
        int rr = br_[k]*br_[k];
        for(y = 0; y < ART_H; y++) for(x = 0; x < ART_W; x++){
            int dx = x - bx[k], dy = (y - by[k])*2;   /* ×2: 小图是扁的, 补回圆形 */
            int d2 = dx*dx + dy*dy, t, a, o;
            if(d2 >= rr) continue;
            t = 256 - d2*256/rr;
            a = t*t/256; a = a*t/256;                 /* 三次衰减 */
            a = a*205/256;
            o = (y*ART_W + x)*3;
            for(i = 0; i < 3; i++)
                g_art[o+i] = (unsigned char)((cols[k][i]*a + g_art[o+i]*(255-a))/255);
        }
    }
}

/* 放大进背景缓存 + 暗角。整套按 (seed, vig) 缓存, 换曲才重算。 */
static void gfx_artbg(int seed, int vig){
    u16_ *dst = G_FB;
    int x, y;
    if(seed != g_art_seed || vig != g_art_vig){
        art_render_small(seed);
        gfx_target(g_bd_cache);
        for(y = 0; y < SCR_H; y++){
            int sy = y*(ART_H-1)*256/(SCR_H-1), y0 = sy >> 8, ty = sy & 255;
            int y1 = y0+1 < ART_H ? y0+1 : ART_H-1;
            for(x = 0; x < SCR_W; x++){
                int sx = x*(ART_W-1)*256/(SCR_W-1), x0 = sx >> 8, tx = sx & 255;
                int x1 = x0+1 < ART_W ? x0+1 : ART_W-1;
                const unsigned char *p00 = &g_art[(y0*ART_W+x0)*3];
                const unsigned char *p01 = &g_art[(y0*ART_W+x1)*3];
                const unsigned char *p10 = &g_art[(y1*ART_W+x0)*3];
                const unsigned char *p11 = &g_art[(y1*ART_W+x1)*3];
                int c[3], i, vx, vy, t, a;
                for(i = 0; i < 3; i++)
                    c[i] = ( p00[i]*(256-tx)*(256-ty) + p01[i]*tx*(256-ty)
                           + p10[i]*(256-tx)*ty       + p11[i]*tx*ty ) >> 16;
                /* 上下压暗带(scrim): 刊头和控件行压在底图上, 而底图的亮团位置是哈希决定的
                 * ⇒ 总有某组配色会把亮团推到刊头下面。实测组1 刊头对比度只有 **2.87:1**
                 *   (正文线 4.5, 非文字下限 3.0), 而曲名/歌手那两条有 9~14:1 很宽裕。
                 * ⇒ 只压文字真正落的上下两条, 中段一点不动 —— 底图该亮的地方仍然亮。
                 *   这也是 Apple Music 在封面上压字的标准做法, 看着就像设计过的。 */
                /* 上带: 先**满强度**盖住刊头的墨(y 52..76), 到 y=140 才淡完。
                 * 单纯线性从 y=0 淡到 116 的话, 刊头基线处只压到 31%, 实测仍只有 3.62:1。 */
                if(y < 140){ int f = (y <= 84) ? 255 : (140 - y)*255/56; int i2;
                    for(i2=0;i2<3;i2++) c[i2] = c[i2]*(255 - f*205/255)/255; }
                else if(y > 384){ int f = (y - 384)*255/96; int i2;
                    if(f > 255) f = 255;
                    for(i2=0;i2<3;i2++) c[i2] = c[i2]*(255 - f*130/255)/255; }
                if(vig > 0){                                  /* 暗角: 四角沉下去 */
                    vx = x*2 - SCR_W; if(vx < 0) vx = -vx; vx = vx*256/SCR_W;
                    vy = y*2 - SCR_H; if(vy < 0) vy = -vy; vy = vy*256/SCR_H;
                    t = (vx*vx + vy*vy) >> 9; if(t > 256) t = 256;
                    a = vig * t * t / 65536;
                    for(i = 0; i < 3; i++) c[i] = c[i]*(255-a)/255;
                }
                gfx_px(x, y, c[0], c[1], c[2]);
            }
        }
        gfx_target(dst);
        g_art_seed = seed; g_art_vig = vig;
        g_bd_ok = 0;                       /* 让普通 backdrop 下次一定重算, 两者共用同一块缓存 */
    }
    if(dst != g_bd_cache){
        const unsigned *a2 = (const unsigned*)(const void*)g_bd_cache;
        unsigned *b2 = (unsigned*)(void*)dst;
        int n2 = SCR_W/2, i;
        int c0 = G_CX0 >> 1, c1 = (G_CX1 + 1) >> 1;   /* 同上: 按列也要裁 */
        if(c0 < 0) c0 = 0; if(c1 > n2) c1 = n2;
        for(y = G_CY0; y < G_CY1; y++){
            int off = y*n2;
            for(i=c0;i+4<=c1;i+=4){ b2[off+i]=a2[off+i]; b2[off+i+1]=a2[off+i+1];
                                    b2[off+i+2]=a2[off+i+2]; b2[off+i+3]=a2[off+i+3]; }
            for(;i<c1;i++) b2[off+i]=a2[off+i];
        }
    }
}

/* 把生成图画成一块**方形封面**(而不是铺满全屏)。圆角遮罩 + 双线性放大。
 * 有了它, "左封面 + 右信息"这种经典播放器版式才成立 —— 之前封面位是空砖所以只能删掉。 */
/* gain: 亮度增益(256 = 原样)。同一张生成图当**背景**要暗(字压在上面),
 * 当**封面砖**要亮(它自己就是主角) —— 差别很大, 所以给一个增益参数而不是两套图。 */
static void gfx_art_tile(int x0, int y0, int w, int h, int seed, int rad, int gain){
    int x, y;
    if(seed != g_art_seed){ art_render_small(seed); g_art_seed = seed; g_art_vig = -1; }
    for(y = 0; y < h; y++){
        int sy = y*(ART_H-1)*256/(h-1), yy0 = sy >> 8, ty = sy & 255;
        int yy1 = yy0+1 < ART_H ? yy0+1 : ART_H-1;
        for(x = 0; x < w; x++){
            int sx = x*(ART_W-1)*256/(w-1), xx0 = sx >> 8, tx = sx & 255;
            int xx1 = xx0+1 < ART_W ? xx0+1 : ART_W-1;
            const unsigned char *p00 = &g_art[(yy0*ART_W+xx0)*3];
            const unsigned char *p01 = &g_art[(yy0*ART_W+xx1)*3];
            const unsigned char *p10 = &g_art[(yy1*ART_W+xx0)*3];
            const unsigned char *p11 = &g_art[(yy1*ART_W+xx1)*3];
            int c[3], i, dx = 0, dy = 0, d2;
            /* 圆角遮罩: 只在四个角判距离, 直边区域零开销 */
            if(x < rad)        dx = rad - x;  else if(x >= w-rad) dx = x - (w-rad-1);
            if(y < rad)        dy = rad - y;  else if(y >= h-rad) dy = y - (h-rad-1);
            if(dx && dy){ d2 = dx*dx + dy*dy; if(d2 > rad*rad) continue; }
            for(i = 0; i < 3; i++)
                c[i] = ( p00[i]*(256-tx)*(256-ty) + p01[i]*tx*(256-ty)
                       + p10[i]*(256-tx)*ty       + p11[i]*tx*ty ) >> 16;
            for(i = 0; i < 3; i++){ c[i] = c[i]*gain/256; if(c[i] > 255) c[i] = 255; }
            gfx_px(x0+x, y0+y, c[0], c[1], c[2]);
        }
    }
}

/* 串哈希 —— 换歌换图, 同一首歌永远同一张 */
static int gfx_str_seed(const char *a, const char *b){
    unsigned h = 2166136261u;
    while(a && *a){ h = (h ^ (unsigned char)*a++) * 16777619u; }
    while(b && *b){ h = (h ^ (unsigned char)*b++) * 16777619u; }
    return (int)(h & 0x7fffffff);
}

/* ============ 带整体 alpha 的变体(弹层/淡入淡出用) ============ */
/* 圆角矩形, 但整体乘一个 alpha */
static void gfx_rrect_a(int x0,int y0,int w,int h,int rad,int r,int g,int b,int a){
    int x,y,sx,sy;
    if(a<=0) return; if(a>255) a=255;
    if(rad<1){ for(y=y0;y<y0+h;y++) for(x=x0;x<x0+w;x++) gfx_blend(x,y,r,g,b,a); return; }
    for(y=0;y<h;y++) for(x=0;x<w;x++){
        int cx=-1,cy=-1,cov=0;
        if(x<rad&&y<rad){cx=rad;cy=rad;}
        else if(x>=w-rad&&y<rad){cx=w-rad-1;cy=rad;}
        else if(x<rad&&y>=h-rad){cx=rad;cy=h-rad-1;}
        else if(x>=w-rad&&y>=h-rad){cx=w-rad-1;cy=h-rad-1;}
        if(cx<0){ gfx_blend(x0+x,y0+y,r,g,b,a); continue; }
        for(sy=0;sy<4;sy++) for(sx=0;sx<4;sx++){
            int ddx=(x*4+sx)-(cx*4+2), ddy=(y*4+sy)-(cy*4+2);
            if(ddx*ddx+ddy*ddy <= rad*rad*16) cov++;
        }
        if(cov) gfx_blend(x0+x,y0+y,r,g,b, cov*a/16);
    }
}
static void gfx_pill_a(int x,int y,int w,int h,int r,int g,int b,int a){
    int rad=h/2; if(rad>w/2) rad=w/2; if(rad<1) rad=1;
    gfx_rrect_a(x,y,w,h,rad,r,g,b,a);
}
/* 圆角矩形描边(1px)。⚠️ 早先版本只画四条直边, 四个圆角是缺口 —— 看着像"只有左右两条线"。
 * 现在四角按 1/8 圆弧补齐。 */
static void gfx_rrect_ring(int x0,int y0,int w,int h,int rad,int r,int g,int b,int a){
    int k;
    if(a<=0) return;
    if(rad<1){
        for(k=0;k<w;k++){ gfx_blend(x0+k,y0,r,g,b,a); gfx_blend(x0+k,y0+h-1,r,g,b,a); }
        for(k=0;k<h;k++){ gfx_blend(x0,y0+k,r,g,b,a); gfx_blend(x0+w-1,y0+k,r,g,b,a); }
        return;
    }
    for(k=rad;k<w-rad;k++){ gfx_blend(x0+k,y0,r,g,b,a); gfx_blend(x0+k,y0+h-1,r,g,b,a); }
    for(k=rad;k<h-rad;k++){ gfx_blend(x0,y0+k,r,g,b,a); gfx_blend(x0+w-1,y0+k,r,g,b,a); }
    /* 四个圆角: 沿 1/4 圆走点(用 x 扫描 + 整数平方根近似) */
    for(k=0;k<=rad;k++){
        int dy2 = rad*rad - k*k, dy = 0, s;
        for(s=rad; s>=0; s--) if(s*s <= dy2){ dy = s; break; }
        gfx_blend(x0+rad-k,     y0+rad-dy,     r,g,b,a);   /* 左上 */
        gfx_blend(x0+w-1-rad+k, y0+rad-dy,     r,g,b,a);   /* 右上 */
        gfx_blend(x0+rad-k,     y0+h-1-rad+dy, r,g,b,a);   /* 左下 */
        gfx_blend(x0+w-1-rad+k, y0+h-1-rad+dy, r,g,b,a);   /* 右下 */
    }
}
/* 水平线段(shell_icon 用): 在 (x,y) 画 len 高的竖条 */
static void gfx_blend_a(int x,int y,int len,int r,int g,int b,int a){
    int k; for(k=0;k<len;k++) gfx_blend(x,y+k,r,g,b,a);
}

/* ================= 文字(离线烤字模) =================
 *
 * 字库有两种来源, 绘制代码完全一样:
 *   · 默认: 编进二进制(studio_font.h)—— Mac 端用这个, 省事。
 *   · STUDIO_FONT_EXTERNAL: 运行时从文件加载 —— SH4 端用这个。
 *     原因很实在: 字库占 SH4 二进制 211KB/263KB, 但它很少变。拆出去后改代码只推 ~52KB,
 *     串口从 12 分钟降到 2.5 分钟, 而字库只推一次。
 *   二进制布局由 bake_font.py --bin 生成, 与下面的 SFontGlyph 结构逐字节一致。 */
/* ============ 图标用的小矢量光栅器(距离场) ============
 * 为什么要它: 手画的图标全是尖角硬边, 看着廉价 —— 用户拿 iMusic 的设计图对比后指出
 * 差别就是"圆润"。圆角/圆头/圆转角如果一个个手画, 既啰嗦又画不匀。
 *
 * 做法: **形状 = 骨架按半径描粗**(Minkowski 和)。
 *   coverage(p) = clamp(R + 0.5 − dist(p, 骨架))
 *   · 骨架是线段 -> 得到两端圆头的胶囊
 *   · 骨架是折线 -> 转角天然是圆的(不用算斜接)
 *   · 骨架是实心多边形(内部距离记 0)-> 得到**圆角多边形**
 *   · 边界那 1 像素的小数覆盖就是抗锯齿, 免费
 * 一个原语覆盖全部图标, 而且所有图标的圆角半径天然一致。
 *
 * 开销: 每个图标约 30×30=900 像素 × 几次浮点, 5 个图标一帧 ~5k 次 —— SH4 有 FPU, 可忽略
 * (对比: 一帧渲染本来就 27ms)。真嫌慢可以缓存成覆盖位图, 目前没必要。 */
static float gsq_(float a){ return a*a; }
static float gsqrt_(float v){                 /* 牛顿迭代, 不依赖 libm 的 sqrtf */
    float x;
    if(v <= 0.0f) return 0.0f;
    x = v > 1.0f ? v*0.5f : 1.0f;
    x = 0.5f*(x + v/x); x = 0.5f*(x + v/x);
    x = 0.5f*(x + v/x); x = 0.5f*(x + v/x);
    return x;
}
/* 点到线段的距离 */
static float gseg_(float px,float py,float ax,float ay,float bx,float by){
    float vx=bx-ax, vy=by-ay, wx=px-ax, wy=py-ay;
    float L=vx*vx+vy*vy, t;
    if(L <= 0.0001f) return gsqrt_(gsq_(wx)+gsq_(wy));
    t = (vx*wx+vy*wy)/L;
    if(t < 0.0f) t = 0.0f; else if(t > 1.0f) t = 1.0f;
    return gsqrt_(gsq_(wx-t*vx) + gsq_(wy-t*vy));
}
/* 折线/多边形描粗。pts = x0,y0,x1,y1,...; closed=1 首尾相连;
 * fill=1 则多边形内部算距离 0(得到**实心圆角多边形**), fill=0 只描边(得到圆角线条)。 */
static void gfx_shape(const float *pts, int n, float R, int closed, int fill,
                      int r, int g, int b, int a){
    int i, x, y, x0, y0, x1, y1;
    float minx=pts[0], maxx=pts[0], miny=pts[1], maxy=pts[1];
    for(i=1;i<n;i++){
        if(pts[i*2] < minx) minx = pts[i*2];
        if(pts[i*2] > maxx) maxx = pts[i*2];
        if(pts[i*2+1] < miny) miny = pts[i*2+1];
        if(pts[i*2+1] > maxy) maxy = pts[i*2+1];
    }
    x0=(int)(minx-R-1); x1=(int)(maxx+R+2);
    y0=(int)(miny-R-1); y1=(int)(maxy+R+2);
    if(!gfx_hit(x0,y0,x1,y1)) return;                 /* 裁剪外整个跳过 */
    if(x0 < G_CX0) x0 = G_CX0; if(x1 > G_CX1) x1 = G_CX1;   /* 再把循环范围收进裁剪框 */
    if(y0 < G_CY0) y0 = G_CY0; if(y1 > G_CY1) y1 = G_CY1;
    for(y=y0;y<y1;y++) for(x=x0;x<x1;x++){
        float px=(float)x+0.5f, py=(float)y+0.5f, d=1e9f, cov;
        int inside=0, cross=0, j;
        for(i=0;i<n;i++){
            int k=(i+1)%n; float dd;
            if(!closed && k==0) break;
            dd = gseg_(px,py, pts[i*2],pts[i*2+1], pts[k*2],pts[k*2+1]);
            if(dd < d) d = dd;
        }
        if(fill && closed){                    /* 奇偶规则判内部 */
            for(i=0, j=n-1; i<n; j=i++){
                float yi=pts[i*2+1], yj=pts[j*2+1], xi=pts[i*2], xj=pts[j*2];
                if((yi>py) != (yj>py) && px < (xj-xi)*(py-yi)/(yj-yi)+xi) cross=!cross;
            }
            inside = cross;
        }
        if(inside) d = 0.0f;
        cov = R + 0.5f - d;
        if(cov <= 0.0f) continue;
        if(cov > 1.0f) cov = 1.0f;
        gfx_blend(x, y, r,g,b, (int)(a*cov));
    }
}


#ifdef STUDIO_FONT_EXTERNAL
typedef struct { int cp; short w,h,xo,yo,adv,_pad; int off; } SFontGlyph;
static const SFontGlyph  *SF_G    = 0;
static const unsigned char *SF_DATA = 0;
static int SF_NG = 0, SF_ASC = 24, SF_PX = 22;
/* 把 bake_font.py --bin 出来的整块内容交给它。返回 1 = 认得。 */
static int gfx_font_use_blob(const unsigned char *b, int len){
    unsigned ng, asc, px, dlen;
    if(!b || len < 20) return 0;
    if(b[0]!='S'||b[1]!='F'||b[2]!='N'||b[3]!='T') return 0;
    ng  =(unsigned)b[4] |((unsigned)b[5]<<8) |((unsigned)b[6]<<16) |((unsigned)b[7]<<24);
    asc =(unsigned)b[8] |((unsigned)b[9]<<8) |((unsigned)b[10]<<16)|((unsigned)b[11]<<24);
    px  =(unsigned)b[12]|((unsigned)b[13]<<8)|((unsigned)b[14]<<16)|((unsigned)b[15]<<24);
    dlen=(unsigned)b[16]|((unsigned)b[17]<<8)|((unsigned)b[18]<<16)|((unsigned)b[19]<<24);
    if(20 + ng*20 + dlen > (unsigned)len) return 0;      /* 长度对不上 = 传坏了 */
    SF_G = (const SFontGlyph*)(const void*)(b + 20);
    SF_DATA = b + 20 + ng*20;
    SF_NG = (int)ng; SF_ASC = (int)asc; SF_PX = (int)px;
    return 1;
}
#define SFONT_NG   SF_NG
#define SFONT_G    SF_G
#define SFONT_DATA SF_DATA
#define SFONT_PX   SF_PX
#define SFONT_ASC  SF_ASC
#else
#include "studio_font.h"   /* Studio 专属烤字库(studio/tools/bake_font.py 生成) */
#endif

/* UTF-8 -> codepoint */
static int gfx_utf8(const char *s,int *cp){
    unsigned char c=(unsigned char)s[0];
    if(c<0x80){*cp=c;return 1;}
    if((c>>5)==6){*cp=((c&31)<<6)|(s[1]&63);return 2;}
    if((c>>4)==14){*cp=((c&15)<<12)|((s[1]&63)<<6)|(s[2]&63);return 3;}
    *cp=((c&7)<<18)|((s[1]&63)<<12)|((s[2]&63)<<6)|(s[3]&63);return 4;
}
/* 在烤好的字库里找字形(二分查找: 字库按 cp 升序) */
static const SFontGlyph *gfx_glyph(int cp){
    int lo=0, hi=SFONT_NG-1;
    if(hi < 0) return 0;              /* 字库还没加载 -> 画占位框, 别崩 */
    while(lo<=hi){
        int mid=(lo+hi)/2, c=SFONT_G[mid].cp;
        if(c==cp) return &SFONT_G[mid];
        if(c<cp) lo=mid+1; else hi=mid-1;
    }
    return 0;
}
/* 画一行文字, 返回结束 x。scale=1 原尺寸, 2=双倍(整数放大, 覆盖度插值保持软边) */
/* 画一行文字。y = 文字顶部。scale=1 原尺寸, 2=双倍。
 * 字库里没有的字 -> 画个空心占位框(而不是静默消失, 免得"字没了"debug 半天)。 */
/* ============ 文字缩放: 从"整数倍"泛化成 1/16 定点 ============
 * 🚨 为什么(2026-08-14 用户要求"歌曲名 36px 就够了"):
 *   字库烤在 24px, 原来只支持整数倍 ⇒ 只有 24 / 48 / 72 三档。36px 落在 1 和 2 中间,
 *   以前只能重烤一份 36px 字库(多 3.9MB), 或者凑合用 48px。
 *   而放大路径本来就是**双线性重采样 + 边缘重建**(不是像素复制), 它对分数倍天然成立 ——
 *   缺的只是把 scale 从整数改成定点。
 * 📐 约定: sc16 = 16 表示 1.0×, 24 = 1.5×(36px), 32 = 2.0×, 48 = 3.0×。
 * ✅ 整数倍必须**逐像素等于**改造前的输出 —— 这是这次改造的验收判据, 已实测通过。 */
#define SC16(x) ((x)*16)
static int gfx_text_s(int x,int y,const char *s,int sc16,int r,int g,int b);
static int gfx_text(int x,int y,const char *s,int scale,int r,int g,int b){
    return gfx_text_s(x, y, s, SC16(scale), r,g,b);
}
static int gfx_text_s(int x,int y,const char *s,int sc16,int r,int g,int b){
    int scale = sc16 / 16;          /* 仅用于占位框/空格这些粗略量 */
    int i=0,cp;
    while(s[i]){
        const SFontGlyph *gl;
        i += gfx_utf8(s+i,&cp);
        if(cp==' '){ x += 6*sc16/16; continue; }
        gl = gfx_glyph(cp);
        if(!gl){
            int bw = (cp>0x7f? SFONT_PX : SFONT_PX/2)*sc16/16, bh = SFONT_PX*sc16/16, k;
            for(k=0;k<bw;k++){ gfx_blend(x+k, y+2, r,g,b, 90); gfx_blend(x+k, y+bh-2, r,g,b, 90); }
            for(k=0;k<bh;k++){ gfx_blend(x, y+k, r,g,b, 90); gfx_blend(x+bw-1, y+k, r,g,b, 90); }
            x += bw + (sc16/16 ? sc16/16 : 1); continue;
        }
        /* 裁剪外的字整个跳过。
         * 🚨 光靠 gfx_blend 里的逐像素裁剪**不够**: scale>1 的路每个目标像素要做 4 次采样 + 增益,
         *   一行 10 个汉字 scale3 就是 72×72×10 ≈ 5.2 万次算完再全部丢掉。
         *   局部重绘(只重画进度条那一小条)时这笔白工比要画的东西还贵。 */
        {   int bw = gl->w*sc16/16, bh = gl->h*sc16/16;
            int bx = x + gl->xo*sc16/16, by = y + gl->yo*sc16/16;
            if(!gfx_hit(bx, by, bx+bw, by+bh)){ x += gl->adv*sc16/16; continue; } }
        if(sc16 == 16){
            int gx,gy;
            for(gy=0;gy<gl->h;gy++) for(gx=0;gx<gl->w;gx++){
                int cov = SFONT_DATA[gl->off + gy*gl->w + gx];
                if(cov) gfx_blend(x+gl->xo+gx, y+gl->yo+gy, r,g,b,cov);
            }
            x += gl->adv;
        } else {
            /* 放大文字: **双线性 + 边缘重建**, 不是像素复制。
             * 🚨 为什么不能直接复制像素(旧做法): 一个源像素变成 scale×scale 个同值像素
             *   ⇒ 24px 字库放大到 48px 就是硬邦邦的方块台阶, 用户反馈"糊/不清晰"就是这个。
             * 🔑 关键认识: 抗锯齿字模的覆盖值**不是灰度, 是亚像素边缘位置的编码**
             *   —— 边缘附近它近似一个距离场。所以:
             *     ① 双线性插值 = 在更高分辨率上重建那个距离场
             *     ② 再对 0.5 做增益 = 在新分辨率上**重新阈值化**, 把被插值抹平的边缘拉回陡峭
             *   只做①会糊(实测边缘陡峭度 41, 原生 48px 是 101);①+②做完是 111, **和原生同档**。
             * 📊 三种画法的字模我逐像素比过(见 2026-08-13 记录): 复制=方块; 双线性=软;
             *   ①+②=实心白笔画、边缘平滑, 与原生 48px 的差别只剩笔画粗细和末端细节。
             * 💰 代价: 每个目标像素 4 次采样, 只在 scale>1 时走 —— 全屏只有标题一行。 */
            int gw = gl->w, gh = gl->h;
            int dw = gw*sc16/16, dh = gh*sc16/16;
            int dx, dy;
            const unsigned char *src = &SFONT_DATA[gl->off];
            for(dy=0; dy<dh; dy++){
                /* 源坐标(1/256 定点): 目标像素中心映回源, 再减半像素对齐 */
                int sy8 = ((dy*2+1)*128*16)/sc16 - 128;
                int y0 = sy8 >> 8, ty = sy8 & 255, y1;
                if(y0 < 0){ y0 = 0; ty = 0; }
                y1 = y0+1 < gh ? y0+1 : gh-1;
                if(y0 > gh-1) y0 = gh-1;
                for(dx=0; dx<dw; dx++){
                    int sx8 = ((dx*2+1)*128*16)/sc16 - 128;
                    int x0 = sx8 >> 8, tx = sx8 & 255, x1, v, u;
                    if(x0 < 0){ x0 = 0; tx = 0; }
                    x1 = x0+1 < gw ? x0+1 : gw-1;
                    if(x0 > gw-1) x0 = gw-1;
                    v = ( src[y0*gw+x0]*(256-tx)*(256-ty) + src[y0*gw+x1]*tx*(256-ty)
                        + src[y1*gw+x0]*(256-tx)*ty      + src[y1*gw+x1]*tx*ty ) >> 16;
                    /* 边缘重建: 以 128 为中心做 2.6 倍增益(666/256), 然后钳位 */
                    u = 128 + (((v-128)*666) >> 8);
                    if(u <= 0) continue;
                    if(u > 255) u = 255;
                    gfx_blend(x+gl->xo*sc16/16+dx, y+gl->yo*sc16/16+dy, r,g,b,u);
                }
            }
            x += gl->adv*sc16/16;
        }
    }
    (void)scale;
    return x;
}
/* 文字 + 整体 alpha */
static int gfx_text_a(int x,int y,const char *s,int scale,int r,int g,int b,int a){
    int i=0,cp;
    if(a<=0) return x; if(a>255) a=255;
    while(s[i]){
        const SFontGlyph *gl;
        i += gfx_utf8(s+i,&cp);
        if(cp==' '){ x += 6*scale; continue; }
        gl = gfx_glyph(cp);
        if(!gl){   /* 缺字: 画占位框(跟 gfx_text 一致) —— 静默消失会让人以为是排版 bug */
            int bw=(cp>0x7f?SFONT_PX:SFONT_PX/2)*scale, bh=SFONT_PX*scale, k;
            for(k=0;k<bw;k++){ gfx_blend(x+k,y+2,r,g,b,a*35/100); gfx_blend(x+k,y+bh-2,r,g,b,a*35/100); }
            for(k=0;k<bh;k++){ gfx_blend(x,y+k,r,g,b,a*35/100); gfx_blend(x+bw-1,y+k,r,g,b,a*35/100); }
            x += bw + scale; continue;
        }
        {
            int gx,gy,sx,sy;
            for(gy=0;gy<gl->h;gy++) for(gx=0;gx<gl->w;gx++){
                int cov = SFONT_DATA[gl->off + gy*gl->w + gx];
                if(!cov) continue;
                for(sy=0;sy<scale;sy++) for(sx=0;sx<scale;sx++)
                    gfx_blend(x+gl->xo*scale+gx*scale+sx, y+gl->yo*scale+gy*scale+sy, r,g,b, cov*a/255);
            }
            x += gl->adv*scale;
        }
    }
    return x;
}
static int gfx_text_w_s(const char *s,int sc16){
    int i=0,cp,w=0;
    while(s[i]){
        const SFontGlyph *gl;
        i+=gfx_utf8(s+i,&cp);
        if(cp==' '){ w += 6*sc16/16; continue; }
        gl=gfx_glyph(cp);
        w += gl ? gl->adv*sc16/16
                : ((cp>0x7f?SFONT_PX:SFONT_PX/2)*sc16/16 + (sc16/16 ? sc16/16 : 1));
    }
    return w;
}
static int gfx_text_w(const char *s,int scale){ return gfx_text_w_s(s, SC16(scale)); }
/* 限宽绘制: 放不下就截断并补省略号。返回实际画到的 x。
 * 🚨 为什么必须有它(2026-08-13 设计评审揪出来的**真 bug**, 不是观感问题):
 *   `gfx_text` 没有任何宽度上限, 而 `gfx_blend` 只在 x>=SCR_W 时**逐像素静默丢弃** ——
 *   于是超长曲名会被**切在笔画中间**, 没有任何提示, 看起来像渲染坏了。
 *   实测 CJK 步进 48.67px/字, 右栏 COL_W=368 只装得下 **7.6 个字**;
 *   而 `PcmState.title[96]` 是从 PCM3Reload 内存**直拷**的, 显示侧全链路零截断。
 *   台架上当天就出现过 `Anthem (The 2002 FIFA World Cup Official Anthem) (JS16 Remix) [Radio Edit]`。
 * ⛔ **绝不做跑马灯** —— 驾驶场景里移动的文本比截断更糟(会持续抓走视线)。
 * 📌 省略号用 U+2026, 四个已烤字库里都有(adv 22~24), 不用重烘。 */
static int gfx_text_lim_s(int x,int y,const char *s,int sc16,int max_w,int r,int g,int b){
    int i, cp, w = 0, cut = -1, ell;
    char buf[192];
    if(max_w <= 0 || gfx_text_w_s(s, sc16) <= max_w) return gfx_text_s(x,y,s,sc16,r,g,b);
    ell = gfx_text_w_s("\xe2\x80\xa6", sc16);          /* … 的宽度 */
    /* 逐字累加, 找到"再加一个字就放不下省略号"的位置 */
    for(i = 0; s[i]; ){
        int adv, n = gfx_utf8(s+i, &cp);
        const SFontGlyph *gl = (cp==' ') ? 0 : gfx_glyph(cp);
        adv = (cp==' ') ? 6*sc16/16
                        : (gl ? gl->adv*sc16/16
                              : ((cp>0x7f?SFONT_PX:SFONT_PX/2)*sc16/16 + (sc16/16 ? sc16/16 : 1)));
        if(w + adv + ell > max_w){ cut = i; break; }
        w += adv; i += n;
    }
    if(cut < 0) cut = i;
    if(cut > (int)sizeof buf - 4) cut = (int)sizeof buf - 4;
    for(i = 0; i < cut; i++) buf[i] = s[i];
    buf[cut] = 0;
    x = gfx_text_s(x, y, buf, sc16, r,g,b);
    return gfx_text_s(x, y, "\xe2\x80\xa6", sc16, r,g,b);
}
static int gfx_text_lim(int x,int y,const char *s,int scale,int max_w,int r,int g,int b){
    return gfx_text_lim_s(x, y, s, SC16(scale), max_w, r,g,b);
}
/* 数字(用烤好的大号数字字模, 比例字体的数字小) */
static void gfx_num(int x,int y,int v,int digits,int scale,int r,int g,int b){
    char buf[16]; int i,p=1;
    for(i=1;i<digits;i++) p*=10;
    for(i=0;i<digits;i++){ buf[i]='0'+((v/p)%10); p/=10; }
    buf[digits]=0;
    gfx_text(x,y,buf,scale,r,g,b);
}

#endif /* STUDIO_GFX_C */
