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
