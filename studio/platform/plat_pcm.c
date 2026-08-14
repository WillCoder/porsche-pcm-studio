/*
 * plat_pcm.c — PCM Studio · 真机(台架/车)平台后端
 *
 * 跟 plat_mac.c 是同一个接口的两个实现 ⇒ **studio/sys 和 studio/scenes 一行都不用改**。
 *
 * 【显示】走独立 gf 硬件层。零刷写 —— 挂层/画/改层序全是运行时, 断电即恢复, 不碰 flash。
 *
 * 🚨 层选择(2026-08-04 隔壁全层普查 + 真车实证, 推翻了之前"用 gf5"):
 *   · **gf1(硬件 L6)= 唯一空闲 RGB 层**, 首选。
 *   · **gf5(硬件 L2)不是空闲层**, 是原厂过渡/叠加层 —— 翻页时原厂把 L3 的全屏 surface 同时写进 L2
 *     把我们顶掉; 而且任何第二个 gf 客户端调 set_layer_order 都会**重置我们层的格式影子**
 *     (被按 pitch=800/h=480 扫 -> 斜切噪点带)。所以 gf5 只当兜底。
 *   · **gf6(硬件 L1)禁用**: 视频采集层, 驱动往 Reserved 位写值 -> 高 6 位死, 红色永远出不来。
 *   · ⚠️ 层分布**随车型而变**(911 上 gf1 是 PDC 雷达图形层!台架 Panamera 没 ParkAssist 所以证不了)
 *     -> 所以**必须配让出协议**(见下), 不能靠"选一个没人用的层"赌。
 *   零刷写 —— 挂层/画/改层序全是运行时, 断电即恢复, 不碰 flash。
 *   ⚠️ 这些是踩出来的血泪, 改之前先看 memory gf-independent-hwlayer-overlay-path-2026-07-19:
 *     · **必须用 gf 层 5**(驱动把层号反转 hw=7−gf; gf6 其实是硬件 L1 视频采集层, 高6位死=红色出不来)
 *     · 像素格式 **RGBA5551**(gf 报的 0x1710 是谎报, 硬件按驱动写死的 LnEC=10 扫) —— 与 studio/scenes/gfx.c 一致
 *     · dst viewport **宽高都是 x2-x1+1**(旧的"高无 +1"已被 08-04 差分实测推翻)
 *     · **每次 set_surfaces 之后必须重申 blending + 两个 viewport + 层序** —— set_surfaces 会冲掉它们
 *     · **哪怕不需要混合也必须显式 set_blending 复位** —— 层配置按硬件层存、不归还,
 *       会继承前一个客户端的 M1_MAP alpha 绑定 -> α≈0 恒不可见(08-05 实锤, 白烧两天)
 *     · gf_dev_attach 前**必须**先 devctl LM_CHECKVER 握手, 否则阻塞死
 *     · order[8] 必须整体 identity 初始化(库无条件读满 8 个)
 *     · **绝不能在紧循环里 gf_layer_update** —— 会 REPLY-block 在 gdcServerCarmine。安全约 1 秒
 *     · 永不调 gf_layer_set_chroma(驱动按错格式打包透明色)
 *     · 设备上没有 libgf.so.1, 链接要用真名 libgdcApiCarmine.so
 *
 * 【状态】读 PCM3Root 内存(adump 法: /proc/<pid>/as 只读 + lseek + read)。
 *   ⚠️ 绝不 poll 共享 IPC/IOC 通道(那条把车和台架都挂死过)。
 *
 * 【输入/命令】⏳ 还没打通(见 memory grand-vision: 输入是最大技术风险)。
 *   现在返回"无事件"/"未实现", 系统照常显示, 只是不能交互。补上后场景代码不用改。
 */
#ifndef PLAT_PCM_C
#define PLAT_PCM_C

#include "../sys/pcm_sys.h"

typedef unsigned int  u32;
typedef unsigned long size_t;   /* freestanding: 没有 stddef.h, gf_defs.h 要用 */

#include "../../coexist-app/mvp/gf_defs.h"
/* 傀儡事件 cave 的地址表 + 信箱字段布局。
 * 🚨 **自动生成, 别手改**: dev/build_flash_bench_puppet.py 打刷写包时和 cave 同一次生成 ——
 *   flash 里那份 cave 和这个二进制共用一个真源。8-06 手抄常量白跑两趟, 不再靠人记。 */
#include "puppet_addr.h"

#define O_RDONLY 0x000
#define O_RDWR   0x002
#define O_WRONLY 0x001
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x008

extern int  open(const char *p, int f, int m);
extern int  read(int fd, void *b, unsigned n);
extern int  write(int fd, const void *b, unsigned n);
extern int  close(int fd);
extern long lseek(int fd, long o, int w);
extern int  usleep(unsigned us);
/* 信号: studio 自己声明(不用系统头)。**号码抄自隔壁 coexist_pop.c** ——
 * 那份在这台机器上实际用过并生效, 比我按 POSIX 猜可靠。 */
#define SIGHUP 1
#define SIGINT 2
#define SIGILL 4
#define SIGFPE 8
#define SIGBUS 10
#define SIGSEGV 11
#define SIGTERM 15
#define SIGALRM 14
#define SIGQUIT 3
#define SIGABRT 6
extern unsigned alarm(unsigned sec);
extern void (*signal(int, void (*)(int)))(int);
extern void exit(int);   /* 桩 libc 里没有 _exit, 隔壁 coexist_pop 也是用 exit */
extern int  devctl(int fd, int cmd, void *d, unsigned n, unsigned *i);
extern int  clock_gettime(int id, void *ts);

#define LM_DEV      "/dev/layermanager"
#define LM_FLAGS    0x2002
#define LM_CHECKVER 0xc00c0506u

#define LOGPATH "/tmp/studio.log"
#define PIDFILE "/tmp/p3pid"

/* ================= 日志 ================= */
static int g_logfirst = 1;
static unsigned p_slen(const char *s){ unsigned n=0; while(s[n])n++; return n; }
void plat_log(const char *s){
    int fd = open(LOGPATH, g_logfirst?(O_WRONLY|O_CREAT|O_TRUNC):(O_WRONLY|O_CREAT|O_APPEND), 0644);
    g_logfirst = 0;
    if(fd>=0){ write(fd,s,p_slen(s)); close(fd); }
}
static void p_logd(int v){ char b[13]; int i=12,neg=0; b[12]=0;
    if(v<0){neg=1;v=-v;} if(!v)b[--i]='0'; else while(v){b[--i]=(char)('0'+v%10);v/=10;}
    if(neg)b[--i]='-'; plat_log(b+i); }
static void p_logh(u32 v){ const char *h="0123456789abcdef"; char b[11]; int i;
    b[0]='0'; b[1]='x'; for(i=0;i<8;i++) b[2+i]=h[(v>>(28-i*4))&0xf]; b[10]=0; plat_log(b); }

const char *plat_name(void){ return "pcm"; }

/* ================= 时钟 ================= */
static unsigned g_t0 = 0;
static unsigned p_now_raw(void){
    u32 ts[2]={0,0};
    if(clock_gettime(2,ts)!=0 && clock_gettime(0,ts)!=0) return 0;
    return ts[0]*1000u + ts[1]/1000000u;
}
unsigned plat_now_ms(void){ return p_now_raw() - g_t0; }

/* ================= 显示: 独立 gf 层 ================= */
static gf_dev_t     g_dev   = 0;
static gf_display_t g_disp  = 0;
static gf_layer_t   g_layer = 0;
/* ============ 双缓冲 ============
 * 用户 2026-08-06 在真机上看到切页时"从上往下扫描刷新"。根因不是慢, 是**能看见写入过程**:
 * 我们往显存灌 65ms 的像素, 而显示器同时在扫描输出。
 * (转圈动画救不了 —— 那 65ms 是一次不可打断的写入, 中间没有空隙留给动画。)
 *
 * 解法: 两块 surface 轮流当前台, 后台画完再一次 update 原子换页 ⇒ 永远看不到中间态。
 * 🚨 关键设计: **每块 surface 配自己的一份影子**。
 *   因为后台那块停留在"上上帧", 差异必须是"当前帧 vs 这块自己的上一帧", 不是 vs 上一帧。
 *   这样搬运量跟单缓冲时一样(只搬变化), 但换页是原子的 —— 不用付双倍代价。
 *
 * 🚨 gf_layer_update 是有前科的地方(紧循环里会 REPLY-block 死锁), 所以:
 *   · 用 GF_LAYER_UPDATE_NO_WAIT_VSYNC, 不等垂直同步
 *   · 换页仍然受"最快多久一次"节流, 跟原来的重申频率同源
 *   · 建不出第二块就自动退回单缓冲, 不是致命错误 */
static gf_surface_t g_surf  = 0;          /* 当前正在画的那块(后台) */
static gf_surface_t g_surf2 = 0;          /* 另一块 */
static u16_        *g_va1 = 0, *g_va2 = 0;
static u16_        *g_shadow2 = 0;        /* 第二块的影子; 指向 g_shadowB */
static int          g_dbuf = 0;           /* 1 = 双缓冲生效 */
static int          g_bank = 0;           /* 当前**画**的是第几块 */
static gf_surface_t g_front = 0;          /* 当前**显示**的是哪一块(周期重申要用它, 不能写死第一块) */
static u16_        *g_vaddr = 0;      /* 层 surface 的可写像素(RGBA5551) */
static int          g_stride_px = SCR_W;
static int          g_lidx = 5;       /* gf 层 5 = 硬件 L2 */
static unsigned     g_order[8] = {0,1,2,3,4,5,6,7};
static int          g_norder = 8;
static unsigned     g_last_reassert = 0;

/* 系统画到这里(始终 800×480 连续), present 时按 stride 拷进层 surface。
 * 这样场景代码永远不用管 stride 对齐(驱动会 align64)。 */
static u16_ g_fb[SCR_W * SCR_H] __attribute__((aligned(4)));
/* 上一帧影子(普通 RAM, 走缓存): present 时逐行比对, 只把变了的段写进显存。
 * 见下面 present_diff() 的大段说明 —— 显存写是 3MB/s 的慢总线, RAM 比对几乎免费。 */
extern unsigned g_render_ms;      /* pcm_shell.c 量的场景 render 耗时 */
static u16_ g_shadow[SCR_W * SCR_H] __attribute__((aligned(4)));
static u16_ g_shadowB[SCR_W * SCR_H] __attribute__((aligned(4)));   /* 第二块 surface 的影子 */
/* 🚨 这是**计数**不是布尔。双缓冲下两块 surface 各自独立, 一次"整屏重刷"必须两块都做一遍,
 *   否则后画的那块里还留着上电时的垃圾(它的影子是全零, 对不上真实内容)。
 *   置 2 = 两块都刷; 每 present 一次减一。 */
static int  g_force_full = 2;
/* 🚨 u32 宽路径的运行时守卫。blit_span/present_diff 直接把 u16_* 强转成 u32*,
 *   SH4 上 mov.l 地址不对齐 = address error = SIGBUS = 进程死。
 *   而进程死 = **最后一帧永久冻在屏上**(gdc 不归还层), 原厂再也盖不回来, 只能重启。
 *   所以宁可退回慢的 u16 路径, 也不能赌对齐。 */
static int  g_wide = 1;
static int  g_bench_done = 0;

/* ★ 分级启动 —— 第一次上台架别一上来整屏, 黑一片啥也看不出。
 *   mode 0 = 自检: 层里画彩色测试图(四原色块+边框), 不管场景。证明"层拿到了+能画+颜色对"。
 *   mode 1 = 整屏: 正常跑系统。
 *   mode 2 = **镜像模式**: 只读原厂状态并打日志, **完全不占屏**(原厂界面照常显示)。
 *            用途: 标定"页id/源slot/源app"到底对应哪个真实页面/音源 —— 人得看得见原厂界面
 *            才知道自己切到哪儿了。2026-08-05 用户实测反馈: 盲操作根本分不清 FM 还是 AUX。
 *   串口: echo 0 > /tmp/studio_mode  /  echo 1 > /tmp/studio_mode  (下一帧生效)
 *   默认 0(自检) —— 宁可第一次多一步, 也别拿黑屏猜。 */
static int g_mode = 0;
/* 🚨 必须在**主循环每拍**调, 不能放在 plat_present 里面 ——
 *   外壳是"只有 dirty 才重画", 首页画完一次就不脏了, present 从此不再被调用,
 *   于是模式开关永远读不到(2026-08-05 台架实测: 写了 /tmp/studio_mode 屏幕纹丝不动,
 *   日志里连一条 [计时] 都没有 = present 总共只跑了几次)。
 * 返回 1 = 模式变了, 调用方必须强制重画一帧。 */
static int read_mode(void){
    int fd = open("/tmp/studio_mode", O_RDONLY, 0);
    char c = 0; int old = g_mode;
    if(fd < 0) return 0;
    if(read(fd,&c,1) == 1 && (c>='0' && c<='2')) g_mode = c - '0';
    close(fd);
    return g_mode != old;
}
/* 自检图: 四角原色块 + 中间白十字 + 边框。眼睛一看就知道颜色通道对不对、几何有没有偏。 */
static void draw_selftest(u16_ *fb, unsigned t){
    int x,y;
    for(y=0;y<SCR_H;y++) for(x=0;x<SCR_W;x++){
        u16_ c;
        if(x<4 || y<4 || x>=SCR_W-4 || y>=SCR_H-4)          c = 0xFFFF;                 /* 白边框 */
        else if(x<200 && y<200)                              c = (u16_)((31<<11)|1);     /* 左上 红 */
        else if(x>=SCR_W-200 && y<200)                       c = (u16_)((31<<6)|1);      /* 右上 绿 */
        else if(x<200 && y>=SCR_H-200)                       c = (u16_)((31<<1)|1);      /* 左下 蓝 */
        else if(x>=SCR_W-200 && y>=SCR_H-200)                c = 0xFFFF;                 /* 右下 白 */
        else if((x>SCR_W/2-3&&x<SCR_W/2+3) || (y>SCR_H/2-3&&y<SCR_H/2+3)) c = 0xFFFF;    /* 十字 */
        else c = (u16_)((6<<11)|(7<<6)|(10<<1)|1);                                        /* 深灰底 */
        fb[y*SCR_W+x] = c;
    }
    /* 走动的方块: 证明帧在更新(不是冻住的一张图) */
    {
        int bx = 240 + (int)((t/120) % 300), by = SCR_H/2 - 20;
        for(y=0;y<40;y++) for(x=0;x<40;x++)
            if(bx+x<SCR_W) fb[(by+y)*SCR_W + bx+x] = (u16_)((31<<11)|(22<<6)|(9<<1)|1);   /* 暖色 */
    }
}

/* ============ 让出协议(2026-08-04 真车验证通过的优先级模型) ============
 * 用户提的模型: **原厂开始用这层, 我们就自动挂起; 这层空闲了再恢复。**
 * 比"找一块没人用的层"根本性地好 —— 不需要知道任何车型的层分布, 自动适配。
 *
 * 🚨🚨 让出时**绝对不能** disable/go_dark。
 *   血的教训(真车 PDC 全黑): 原厂是"功能启动时点亮一次"的模式, 若让出恰好发生在它点亮**之后**,
 *   我们这最后一次 disable 就把原厂的层**关死了**, 之后再也不碰 -> 永远黑。
 *   检测到位移时屏上显示的本来就是原厂的内容, **纯停手**就绝不可能是它变黑的原因。
 *
 * 检测: 每拍读 gdc 共享内存里我们这层的记录, 与我们建层时的指纹比对。
 *   记录被改(格式/尺寸/surface 地址变了)= 原厂在用 -> 让出。
 *   ⚠️ 必须**每拍**都看(不只在显示期) —— v20 只在显示期看, 正是漏掉哑火期的原因。 */
/* 🚨 2026-08-05 修正: 原来这里写的是 "/dev/shmem/gdc_shm_layers_info" + 96B表头/88B每条 ——
 *   **对象名和布局都是编的**(shmdump.c 明写这个对象我们从没读过; 96/88 全项目零出处)。
 *   open() 必然失败 -> read_layer_fp 恒返 0 -> 让出协议**全程没生效过**,
 *   而"层到底绑到谁"这个唯一能一锤定音的测量也从来没真做过。
 *   真布局(references/TOOLBOX.md:214; gftest/coexist_pop/l2watch 一致):
 *     shm_open("/gdc_shm_inform") + mmap -> base + 0xe28 + disp*0x5a0 + 硬件层*120
 *     u32[1]=字节/像素  u32[3]=pitch  u32[4]=height  u32[5]=物理地址(低28位) */
extern int   shm_open(const char *n, int oflag, int mode);
extern void *mmap(void *a, unsigned len, int prot, int flags, int fd, long off);
#define PROT_READ    0x100
#define PROT_NOCACHE 0x800
#define MAP_SHARED   1
#define SHM_LEN      0x1aa0
#define SHM_RECBASE  0xe28
#define SHM_LSTRIDE  120

static int  g_yield = 0;              /* 1 = 让出态(原厂在用这层, 我们纯停手) */
static int  g_idle_ticks = 0;         /* 让出态下该层连续空闲的拍数 */
static u32  g_fp_fmt = 0, g_fp_sz = 0;/* 我们建层时的记录指纹 */
static int  g_hwidx = -1;             /* 硬件层号 = 7 - gf层号(驱动反转) */

static unsigned char *g_shm = 0; static int g_shm_tried = 0;
static unsigned char *shm_map(void){
    int fd;
    if(g_shm_tried) return g_shm;
    g_shm_tried = 1;
    fd = shm_open("/gdc_shm_inform", O_RDONLY, 0);
    if(fd < 0) return 0;
    { unsigned char *B = (unsigned char*)mmap(0, SHM_LEN, PROT_READ|PROT_NOCACHE, MAP_SHARED, fd, 0);
      close(fd);
      if((long)B == -1 || !B) return 0;
      g_shm = B; }
    return g_shm;
}
static volatile u32 *rec_of_hw(int hw){
    unsigned char *B = shm_map();
    if(!B || hw < 0 || hw > 11) return 0;
    return (volatile u32*)(B + SHM_RECBASE + hw*SHM_LSTRIDE);
}
/* 指纹: fmt 位置放 bpp<<16|pitch, sz 位置放 height<<16|物理地址高位 —— 三个字段都是
 * "谁在扫这层"的直接证据(隔壁 coexist_pop 判据: bpp/pitch/height/paddr 任一变 = 被抢) */
static int read_layer_fp(u32 *fmt, u32 *sz){
    volatile u32 *r;
    if(g_hwidx < 0) return 0;
    r = rec_of_hw(g_hwidx);
    if(!r) return 0;
    *fmt = (r[1] << 16) | (r[3] & 0xffffu);
    *sz  = (r[4] << 16) | ((r[5] >> 12) & 0xffffu);
    return 1;
}
/* 🚨🚨 2026-08-13 修的一个**自己咬自己**的 bug:
 *   sz 的低 16 位是 (物理地址 >> 12) —— 而我们是**双缓冲**, 每次翻页物理地址就变一次。
 *   基线只在首次推送后采了一次(= bank 0), 于是第一次翻到 bank 1 就判成"原厂在用这层" -> 让出;
 *   让出后我们停止绘制 -> 不再翻页 -> 地址永远停在 bank 1 ≠ 基线 -> **永远恢复不了**。
 *   现象: 画面冻在最后一帧(进度条不动、换歌不刷新), 而状态读取一切正常 —— 极具迷惑性。
 *   实测证据: 基线 sz=0x01e025cc, 让出时 sz=0x01e02511, fmt **完全相同**;
 *             低半差 0xbb 页 = 765952 字节, 而 800×480×2 = 768000 ⇒ **正好是我们两块的间距**。
 *   修: 两个 bank 各学一份指纹 —— **刚推完的那一帧读到的, 按定义就是我们自己的**。
 *       两份都对不上才算被抢。fmt 仍然逐位比(格式/pitch/高度变了才是真被抢)。 */
static u32 g_fp_sz_bank[2] = {0, 0};
static void fp_learn_bank(int bank){
    u32 fmt=0, sz=0;
    if(bank < 0 || bank > 1) return;
    if(!read_layer_fp(&fmt, &sz)) return;
    if(fmt != g_fp_fmt) return;                    /* 格式都变了, 那不是我们的, 不学 */
    if(g_fp_sz_bank[bank] != sz){
        g_fp_sz_bank[bank] = sz;
        plat_log("[让出协议] 学到 bank"); p_logd(bank);
        plat_log(" 指纹 sz="); p_logh(sz); plat_log("\n");
    }
}
static int fp_is_ours(u32 sz){
    return sz == g_fp_sz || sz == g_fp_sz_bank[0] || sz == g_fp_sz_bank[1];
}
static void yield_check(void){
    u32 fmt=0, sz=0;
    if(!read_layer_fp(&fmt, &sz)) return;          /* 读不到就别乱判 */
    if(!g_yield){
        if(fmt != g_fp_fmt || !fp_is_ours(sz)){
            g_yield = 1; g_idle_ticks = 0;
            plat_log("[让出] 原厂在用这层 -> 纯停手(不 disable!) fmt=");
            p_logh(fmt); plat_log(" sz="); p_logh(sz); plat_log("\n");
        }
    } else {
        if(fmt == g_fp_fmt && fp_is_ours(sz)){
            /* 🚨 注释以前写"连续 2 秒空闲(10fps)" —— **错的**。这个函数由 plat_tick_watch
             *   驱动, 而主循环是 usleep(25000)(main_pcm.c) ⇒ 20 拍 = **0.5 秒**, 不是 2 秒。
             *   那条注释是主循环还在 100ms 一拍的年代留下的, 循环变快之后没跟着改。 */
            if(++g_idle_ticks >= 20){              /* 20 拍 × 25ms = 0.5 秒空闲 */
                g_yield = 0; g_idle_ticks = 0;
                plat_log("[恢复] 该层已空闲 2 秒, 重新接管\n");
            }
        } else g_idle_ticks = 0;
    }
}

u16_ *plat_framebuf(void){ return g_fb; }

/* ======== 🚨🚨 层推送: 唯一被实证过的调用序列, 所有地方都走这一个函数 ========
 *
 * 【根因·2026-08-05 台架实锤】我原来**一次 gf_layer_set_blending 都没调过**,
 *   注释里还写着"不需要混合所以不调" —— 这是致命的误解。
 *   层配置是 gdc 服务端按【硬件层】存的、无归属、后写者赢, 且 KB 已实证
 *   gf_layer_detach 是零 IPC 空操作、进程被 kill 也不归还层
 *   ⇒ 前一个用这块层的客户端留下的混合配置会被我们**原样继承**。
 *   台架上隔壁 coexist_pop 的 ui.def 写着 panel_alpha=240 (<255) ⇒ 它给 gf1(hwL6) 设了
 *   M1_MAP alpha 平面混合 (mode=0x00080102), 收起/退出时**只清 alpha 平面, 从不复位 mode**。
 *   我们接手后, 硬件拿着一张全零(或已释放)的 alpha 平面给我们的像素取 M1 ⇒ α≈0,
 *   **整层恒不可见**, 而所有返回码照样 0、自己 surface 里的像素照样是对的。
 *   ⇒ 不管需不需要混合, **都必须显式复位**。
 *
 * 【顺序·台架实证 2026-08-04】gf_layer_set_surfaces 会**冲掉层上的其它绑定**
 *   (只调一次 set_blending 就出横向条纹, 就是这么发现的)
 *   ⇒ 凡调 set_surfaces, 后面 blending / 两个 viewport / 层序 **全都要重申**。
 *   固定序列: set_surfaces -> set_blending -> src_vp -> dst_vp -> order -> enable -> update */
static unsigned char g_alphablk[64];    /* 全零 gf_alpha_t: mode=0, 在驱动 9 值白名单内 */
/* 🐛🐛 2026-08-13 定案的抖动真因就在这个函数里。
 *   `g_front` 声明处的注释写着"周期重申要用它, **不能写死第一块**" —— 但代码从来没读过 g_front,
 *   `set_surfaces` 一直传 `&g_surf`(= bank0)。而本函数**每秒被 plat_present 尾部调用一次,
 *   且就在 swap_bank 之后** ⇒ 换页刚把 bank1 顶上前台, 重申立刻把它拽回 bank0,
 *   而 bank0 装的是**上一次 present 写进去的旧帧**(present_diff 只写当前后台块, 从不清另一块)
 *   ⇒ 屏上出现 A → B → A ⇒ 用户看到的"进度条往回倒一点"。
 *   单缓冲下 swap_bank 一次都不跑 ⇒ g_front 恒 0 ⇒ 推的就是唯一那块 ⇒ 完全幂等 ⇒ 不抖。
 *   **这正好解释了"双缓冲抖 / 单缓冲不抖"的 A/B 实测**, 也与"数值不可能倒退"相容。
 * 🚨 诊断开关 `/tmp/studio_oldfront`: 恢复旧行为(写死第一块)。**存在的唯一理由是拿基线** ——
 *   修好之后 g_reassert_flip 结构上必然是 0, 那是个恒成立的断言, 不能当证据。
 *   有了开关才能在同一次上机里测出 "旧=非0 / 新=0" 的对照。 */
static unsigned g_reassert_flip = 0;    /* 重申**改变了**前台的次数 = bug 复现计数 */
static int      g_oldfront = 0;         /* 1 = 用旧行为(仅诊断) */
static void push_layer(void){
    gf_surface_t cur;
    if(!g_layer || !g_surf) return;
    cur = (g_front && !g_oldfront) ? g_front : g_surf;
    if(g_front && cur != g_front) g_reassert_flip++;
    gf_layer_set_surfaces(g_layer, &cur, 1);
    gf_layer_set_blending(g_layer, (gf_alpha_t*)g_alphablk);
    gf_layer_set_src_viewport(g_layer, 0, 0, SCR_W-1, SCR_H-1);
    /* ⚠️ 宽高**都是** x2-x1+1。旧注释"高 = y2-y1 无 +1"是错的 ——
     *   2026-08-04 隔壁用 pcmshot 差分实测: 传 y2=y+h 会多渲染一行。整屏时更糟:
     *   在 480 行的屏上请求 481 行。 */
    gf_layer_set_dst_viewport(g_layer, 0, 0, SCR_W-1, SCR_H-1);
    gf_display_set_layer_order(g_disp, g_order, 0);
    gf_layer_enable(g_layer);
    gf_layer_update(g_layer, 0);
    g_front = cur;          /* 维护不变量: g_front 永远 = 当前真正在前台的那块 */
}

int plat_init(void){
    int lmfd, r, ci;
    static unsigned char devinfo[256], dispinfo[256], sinfo[96];
    gf_display_info_t *di;
    gf_surface_info_t *si;
    int pref[3] = {1,5,7};            /* gf1(硬件L6)=唯一空闲RGB层, 首选; gf5/gf7 兜底。**绝不用 gf6**(视频采集层, 红色死) */

    g_t0 = p_now_raw();
    plat_log("=== PCM Studio (真机后端) ===\n");

    /* 1. LM CHECKVER 握手 —— 不做这步 gf_dev_attach 会阻塞死 */
    lmfd = open(LM_DEV, LM_FLAGS, 0);
    plat_log("lm fd="); p_logd(lmfd); plat_log("\n");
    if(lmfd >= 0){ int cv[3]={0,0,0}; devctl(lmfd, (int)LM_CHECKVER, cv, 12, 0); }

    /* 2. gf 设备/显示 */
    { int i; for(i=0;i<256;i++){ devinfo[i]=0; dispinfo[i]=0; } }
    if(gf_dev_attach(&g_dev, GF_DEVICE_INDEX(0), (gf_dev_info_t*)devinfo) != GF_ERR_OK){
        plat_log("ABORT gf_dev_attach\n"); return -1; }
    if(gf_display_attach(&g_disp, g_dev, 0, (gf_display_info_t*)dispinfo) != GF_ERR_OK){
        plat_log("ABORT gf_display_attach\n"); return -1; }
    di = (gf_display_info_t*)dispinfo;
    plat_log("display "); p_logd((int)di->xres); plat_log("x"); p_logd((int)di->yres);
    plat_log("  nlayers="); p_logd((int)di->nlayers); plat_log("\n");

    /* 3. 抢池外空闲层(5 优先) */
    for(ci=0; ci<3; ci++){
        int c = pref[ci];
        if(di->nlayers && c >= (int)di->nlayers) continue;
        r = gf_layer_attach(&g_layer, g_disp, c, GF_LAYER_ATTACH_PASSIVE);
        plat_log("gf_layer_attach("); p_logd(c); plat_log(",PASSIVE) r="); p_logd(r); plat_log("\n");
        if(r == GF_ERR_OK){ g_lidx = c; break; }
        g_layer = 0;
    }
    if(!g_layer){ plat_log("ABORT 没抢到层\n"); return -1; }

    /* 🚨 接管前先把这层**真关一次** —— 清掉上一个实例(崩溃/被杀/看门狗退出)留下的残留。
     *   gdc 不会在客户端死亡时关层, 新实例 attach 上来硬件侧还挂着旧配置。
     *   ⚠️ 此刻还没建 surface, 所以只 disable+update, **不碰几何**(在无 surface 的层上设视口
     *      是没人验证过的路径, 还会给"层塌成 1×1"多备一个来源)。 */
    gf_layer_disable(g_layer);
    gf_layer_update(g_layer, 0);
    plat_log("[清残留] 已 disable+update 真关一次\n");

    /* 4. 建整屏 surface。格式常量传 0x1710(它只决定 bpp=2, 硬件模式由驱动写死 RGBA5551) */
    r = gf_surface_create_layer(&g_surf, &g_layer, 1, 0, SCR_W, SCR_H, GF_FORMAT_PACK_ARGB1555, (void*)0, 0);
    plat_log("gf_surface_create_layer("); p_logd(SCR_W); plat_log("x"); p_logd(SCR_H);
    plat_log(") r="); p_logd(r); plat_log("\n");
    if(r != GF_ERR_OK){ plat_log("ABORT surface\n"); return -1; }

    { int i; for(i=0;i<96;i++) sinfo[i]=0; }
    gf_surface_get_info(g_surf, (gf_surface_info_t*)sinfo);
    si = (gf_surface_info_t*)sinfo;
    g_vaddr = (u16_*)(size_t)si->vaddr;
    g_stride_px = (int)si->stride / 2;          /* 16bpp */
    plat_log("surf vaddr="); p_logh((u32)(size_t)si->vaddr);
    plat_log(" stride="); p_logd((int)si->stride);
    plat_log(" (px="); p_logd(g_stride_px); plat_log(")\n");
    if(!g_vaddr){ plat_log("ABORT vaddr=0\n"); return -1; }

    /* 🚨 层序 = **把我们排到数组末尾 = 最顶**(2026-08-05 台架真截图 A/B 实证)。
     *   同一次运行、单一变量的两张 pcmshot 对照(区域 150,140 500x200):
     *     恒等 {0,1,..,7}    -> 我们那块全黑, 只有原厂内容  ❌
     *     {0,2,3,4,5,6,7,1}  -> 五色条 5/5 全中              ✅
     *   然后整屏 800x480: 采样 24000 点全部是我们的颜色, **零个原厂像素** = 完全覆盖。
     *   跟能显示的 coexist_pop 做法一致(除自己外顺序排, 自己放最后)。 */
    { int k,w=0,no=(int)di->nlayers; if(no<1||no>8) no=8;
      for(k=0;k<no;k++) if(k!=g_lidx) g_order[w++]=(unsigned)k;
      if(w<8) g_order[w++]=(unsigned)g_lidx;
      while(w<8){ g_order[w]=(unsigned)w; w++; }
      g_norder = 8; }
    g_hwidx = 7 - g_lidx;   /* 硬件层号 = 7 - gf层号(驱动三处 neg/add#7 反转) */
    { int i; for(i=0;i<64;i++) g_alphablk[i] = 0; }   /* mode=0 = 不混合; 不靠 BSS 清零 */
    g_force_full = 2; g_bench_done = 0;
    { int f = open("/tmp/studio_oldfront", O_RDONLY, 0);
      if(f >= 0){ close(f); g_oldfront = 1;
          plat_log("⚠ 诊断: 重申写死第一块(旧行为), 仅用于拿基线\n"); } }              /* 同理, 不靠 BSS 清零 */
    /* ---- 第二块 surface: 双缓冲。建不出来就退回单缓冲, 不是致命错误 ---- */
    g_va1 = g_vaddr; g_bank = 0; g_dbuf = 0;
    { static unsigned char si2[96];
      int r2 = gf_surface_create_layer(&g_surf2, &g_layer, 1, 1, SCR_W, SCR_H,
                                       GF_FORMAT_PACK_ARGB1555, (void*)0, 0);
      /* 逃生开关: 台架上 `>/tmp/studio_nodbuf` 就退回单缓冲 —— 万一双缓冲挂了不用重推二进制 */
      { int nf = open("/tmp/studio_nodbuf", O_RDONLY, 0);
        if(nf >= 0){ close(nf); plat_log("双缓冲: 关 (/tmp/studio_nodbuf 存在)\n"); r2 = -1; } }
      if(r2 == GF_ERR_OK){
          gf_surface_info_t *s2;
          int i2; for(i2=0;i2<96;i2++) si2[i2]=0;
          gf_surface_get_info(g_surf2, (gf_surface_info_t*)si2);
          s2 = (gf_surface_info_t*)si2;
          g_va2 = (u16_*)(size_t)s2->vaddr;
          /* 两块的 stride 必须一致, 否则 blit 的行距算错 —— 不一致就不用双缓冲 */
          if(g_va2 && (int)s2->stride/2 == g_stride_px){
              g_dbuf = 1;
              plat_log("双缓冲: 开 (第二块 va="); p_logh((u32)(size_t)g_va2); plat_log(")\n");
          } else {
              plat_log("双缓冲: 关 (第二块 stride 不一致)\n");
              if(g_surf2){ gf_surface_free(g_surf2); g_surf2 = 0; }
          }
      } else {
          plat_log("双缓冲: 关 (第二块建不出来 r="); p_logd(r2); plat_log(")\n");
          g_surf2 = 0;
      } }
    /* 🚨 宽路径(u32)对齐守卫: 三个指针和行距都必须 4 字节对齐, 否则 SH4 上 mov.l 会 address error。
     *   守卫不过就退回 u16 —— 慢 2 倍, 但绝不会因为 SIGBUS 把最后一帧永久冻在屏上。 */
    g_wide = ((((unsigned)(size_t)g_vaddr) | ((unsigned)(size_t)g_fb) |
               ((unsigned)(size_t)g_shadow) | (unsigned)(g_stride_px*2)) & 3u) == 0;
    plat_log(g_wide ? "[上屏] 宽路径 u32\n" : "[上屏] ⚠对齐不满足 -> 退回 u16 窄路径\n");
    push_layer();
    plat_log("首次推送完成(含 set_blending 复位) 层="); p_logd(g_lidx); plat_log("\n");
    g_last_reassert = plat_now_ms();
    if(read_layer_fp(&g_fp_fmt, &g_fp_sz)){
        plat_log("[让出协议] 已就绪 hw L"); p_logd(g_hwidx);
        plat_log(" 指纹 fmt="); p_logh(g_fp_fmt); plat_log(" sz="); p_logh(g_fp_sz); plat_log("\n");
    } else plat_log("[让出协议] ⚠ 读不到层记录, 让出保护不可用\n");
    return 0;
}

/* 🚨 优雅归还这块硬件层。**没有这条路就绝不能上台架** ——
 *   KB 实证: gf_layer_detach 是零 IPC 空操作, 进程被 kill 也不归还层(硬件 enable 位仍是 1),
 *   所以一旦只能靠 slay 停, 我们最后那一帧就**永久冻在屏上**, 原厂再也盖不回来。
 *   顺序照隔壁 coexist_pop 的收起路径: 先关层, 再清缓冲(反过来会闪一下黑框)。 */
void plat_puppet_park(void);     /* 见文件后半"傀儡事件协议": 退出时把 LEVEL 归零 */
/* 事件订阅: 0=没订上(退回纯轮询) 1=订上了。定义在文件靠后, 这里前向声明。 */
static int g_ev_on;
static int  mme_reg_events(int on);
static void mme_drain_events(void);

void plat_shutdown(void){
    /* 🚨🚨 **第一件事就是摘触摸门**, 而且必须在下面 `if(!g_layer) return` 之前。
     *   `touch /tmp/studio.stop` 是文档里唯一推荐的停法, 它走这里、**一个信号都不产生** ——
     *   漏了这一句, 正常停一次 studio 就把原厂触摸永久留死。这比崩溃的概率高一个数量级。 */
    plat_ts_disarm();
    /* 显式注销事件。close(fd)/进程死时服务端也会 ntfy_freeClient 回收(逐指令查过),
     * 但正常退出走这条更干净 —— 双保险。同样必须在 `if(!g_layer) return` 之前。 */
    if(g_ev_on) mme_reg_events(0);
    plat_puppet_park();          /* studio 走了就别留着武装态 —— LEVEL=0 = cave 惰性 */
    if(!g_layer) return;
    { int i; for(i=0;i<64;i++) g_alphablk[i]=0; }
    gf_layer_set_blending(g_layer,(gf_alpha_t*)g_alphablk);  /* 别把混合绑定留给下一个人 */
    gf_layer_disable(g_layer);
    gf_layer_update(g_layer, 0);                             /* 只有 disable+update 才真关 */
    plat_log("[退出] 已复位混合 + disable+update 真归还层\n");
}
/* 🚨🚨 让出协议必须**主循环每拍**跑, 不能挂在 plat_present 里 ——
 *   present 受 dirty 门控(pcm_shell.c), **静止画面下根本不被调用**,
 *   于是"原厂开始用这层"这件事我们永远发现不了。这是真车安全问题, 不是性能问题:
 *   911 上 gf1 是 PDC 雷达层, 漏检就等于跟原厂抢层。
 *   跟 read_mode 是同一类坑(那个只是模式切不动, 这个会影响真车)。
 *   成本: 4 次非缓存 u32 读/拍, shm 映射有缓存, 稳态零 syscall。 */
static int mme_get_speed(int *out);   /* MME 只读探针, 定义在文件靠后 */
/* 随机/重复的缓存。-1 = 还没读到 ⇒ UI 画灰的(**不是画成"关"** —— 两者用户看到的含义不同) */
static int g_shuffle = -1, g_repeat = -1;
/* 蓝牙设备名。出处 = `/fs/avrcp0/.FS_info./info.xml` 的 <name>, 明文 XML(台架实读):
 *     <serial>64:31:35:28:FE:9A</serial>
 *     <name>Will&apos;s iPhone</name>
 * 🚨 **只 open 指定路径, 绝不遍历 /fs/avrcp0** —— 目录遍历会让 io-fs-media SIGSEGV(KB 实证)。
 *    直接 open 一个具体文件不是遍历, 是安全的。挂载点序号可能不是 0, 所以按 0/1/2 逐个**直接试开**。
 * 🚨 **必须解 XML 实体** —— 撇号在文件里是 `&apos;`, 不解的话屏上就显示成 "Will&apos;s iPhone"。
 * 📌 这也是 2026-08-06 "设备名候选判据不唯一、宁可不显示" 那条的收口:
 *    现在出处唯一且明文, 可以显示了; 但读不到仍然留空(UI 退回"蓝牙"), 不许编。 */
static void read_bt_device_name(char *out, int cap){
    static const char *paths[] = {
        "/fs/avrcp0/.FS_info./info.xml",
        "/fs/avrcp1/.FS_info./info.xml",
        "/fs/avrcp2/.FS_info./info.xml", 0 };
    char b[640];
    int i, n, fd = -1, s = -1, e = -1, k, o = 0;
    out[0] = 0;
    for(i = 0; paths[i]; i++){ fd = open(paths[i], O_RDONLY, 0); if(fd >= 0) break; }
    if(fd < 0) return;
    n = read(fd, b, (unsigned)(sizeof b - 1)); close(fd);
    if(n <= 0) return;
    b[n] = 0;
    for(k = 0; k + 6 <= n; k++)
        if(b[k]=='<'&&b[k+1]=='n'&&b[k+2]=='a'&&b[k+3]=='m'&&b[k+4]=='e'&&b[k+5]=='>'){ s = k+6; break; }
    if(s < 0) return;
    for(k = s; k + 7 <= n; k++)
        if(b[k]=='<'&&b[k+1]=='/'&&b[k+2]=='n'&&b[k+3]=='a'&&b[k+4]=='m'&&b[k+5]=='e'&&b[k+6]=='>'){ e = k; break; }
    if(e < 0) return;
    for(k = s; k < e && o < cap-1; ){
        if(b[k] == '&'){
            if(e-k >= 6 && b[k+1]=='a'&&b[k+2]=='p'&&b[k+3]=='o'&&b[k+4]=='s'&&b[k+5]==';'){ out[o++]='\''; k+=6; continue; }
            if(e-k >= 6 && b[k+1]=='q'&&b[k+2]=='u'&&b[k+3]=='o'&&b[k+4]=='t'&&b[k+5]==';'){ out[o++]='"';  k+=6; continue; }
            if(e-k >= 5 && b[k+1]=='a'&&b[k+2]=='m'&&b[k+3]=='p'&&b[k+4]==';'){ out[o++]='&'; k+=5; continue; }
            if(e-k >= 4 && b[k+1]=='l'&&b[k+2]=='t'&&b[k+3]==';'){ out[o++]='<'; k+=4; continue; }
            if(e-k >= 4 && b[k+1]=='g'&&b[k+2]=='t'&&b[k+3]==';'){ out[o++]='>'; k+=4; continue; }
        }
        out[o++] = b[k++];
    }
    out[o] = 0;
}
/* 曲目文本重读闸门: 四个 HB 串指针(S+0x6c..0x7b)的指纹。**别用 S+0x54, 它换曲时不变** */
static unsigned char g_meta_fp[16] = {0};
static int g_meta_dirty = 1;
static int mme_get_mode(int rep, int *out);  /* rep=0 随机 / 1 重复 —— 只读 */
static int mme_set_mode(int rep, int mode);  /* 同上, 写;mode 在里面钳 0..4 */
void plat_peek(void);
void plat_post_cmd(void);
/* ============ 覆盖 / 让开 ============
 * 用户 2026-08-06 定的产品行为:
 *   · 媒体类页面(FM/蓝牙/AUX/音源)-> **我们的 UI**
 *   · 车辆设置这类 -> **沿用原厂**, 我们让开
 * 我们的层是独立硬件层, "让开"就是把它真关掉(disable+update), 原厂的层自然露出来 ——
 * 这条路镜像模式已经验证过。⚠️ 只"不画"不够, 层还开着会一直扫我们上一帧。
 *
 * 🚨 绝不 disable 原厂的层, 只关我们自己这一块(gf1)。 */
static int g_cover = 1;                  /* 1 = 我们盖着; 0 = 让给原厂 */
int  plat_ts_disarm(void);               /* 前置声明: 让开之前要先摘触摸门 */
int  plat_ts_arm(void);                  /* 前置声明: 盖上之后要立刻装门 */
extern int g_ts_armed_pub;               /* = g_ts_armed, 给这里手用(定义在触摸门那一节之后) */
static void set_cover(int on){
    if(on == g_cover) return;            /* 只在变化时动, 别每拍折腾 gdc */
    /* 🚨🚨 **让开之前必须先摘触摸门**(复核 F2/F4)。
     *   顺序反了会出现"原厂界面露着、但触摸是死的" —— 用户看得见点不动, 最难受的状态。
     *   而且这条路是**自动触发**的(页面白名单/让出协议), 不需要任何人操作。 */
    if(!on && g_ts_armed_pub) plat_ts_disarm();
    g_cover = on;
    if(on){
        g_force_full = 2;                /* 让开期间原厂画过, 两块都要重搬 */
        push_layer();
        plat_log("[覆盖] 接管屏幕\n");
        /* 🔒 **不变式: 我们盖着 ⟺ 原厂收不到触摸。** 这两件事只在这一个函数里维护。
         * 🚨 2026-08-14 用户定的形态。为什么必须绑在一起而不是靠人发命令装门:
         *   ① 靠记得发命令 ⇒ 一定有忘的时候, 而"页面盖着但原厂还在收触摸"就是穿透本身;
         *   ② 两个状态分开维护 ⇒ 一定会出现不一致, 而且不一致时没人知道。
         *   反方向(让开前先摘门)上面已经有了 —— 现在两个方向都由 set_cover 独占。
         * 🚑 逃生: `>/tmp/studio_nolock` 就不自动装门(万一门本身把台架搞坏, 串口还能救);
         *   另外硬键不受门影响, 用户按 SOURCE/MEDIA 切到非白名单页 ⇒ 自动让开 ⇒ 自动摘门。 */
        {   int nl = open("/tmp/studio_nolock", O_RDONLY, 0);
            if(nl >= 0){ close(nl); plat_log("[触摸门] /tmp/studio_nolock 存在 -> 不自动装\n"); }
            else if(!g_ts_armed_pub){
                plat_log(plat_ts_ensure(1) ? "[触摸门] 随覆盖自动装上 ✓\n"
                                           : "[触摸门] ⚠ 自动装失败(原厂仍能收到触摸)\n");
            }
        }
    } else {
        gf_layer_disable(g_layer);
        gf_layer_update(g_layer, 0);
        plat_log("[覆盖] 让开, 显示原厂\n");
    }
}
int plat_is_covering(void){ return g_cover; }

void plat_tick_watch(void){
    static int last_mode = -1;
    if(!g_vaddr) return;
    /* 🔒 **不变式维护: 盖着 ⇒ 触摸门装着。**
     * 🚨 2026-08-14 实测教训: 原来只把装门写在 set_cover(1) 的转换里, 而 `g_cover` 的
     *   **初值就是 1** ⇒ set_cover(1) 在 `if(on == g_cover) return;` 处直接返回,
     *   装门那段**一次都没执行**, 日志里连一行都没有, 症状是"原厂照常响应触摸"。
     *   **转换式的强制只管得住变化, 管不住初始状态。** 不变式要周期性维护, 不能只在边沿做。
     * 💰 成本: 1 秒一次, 只在"该装而没装"时才真去写。 */
    {   static unsigned last_chk = 0;
        unsigned now = plat_now_ms();
        if(g_cover && g_mode != 2 && (last_chk == 0 || now - last_chk >= 1000)){
            last_chk = now;
            if(!g_ts_armed_pub){
                int nl = open("/tmp/studio_nolock", O_RDONLY, 0);
                if(nl >= 0) close(nl);
                else {
                    static int tries = 0;
                    if(tries < 5){                    /* 装不上就别每秒刷屏 */
                        if(plat_ts_ensure(1)){ plat_log("[触摸门] 不变式维护: 已装上 ✓\n"); tries = 0; }
                        else { plat_log("[触摸门] ⚠ 装不上, 原厂仍能收到触摸\n"); tries++; }
                    }
                }
            }
        }
    }
    /* 进/出镜像模式时真开关层 —— 只是"不画"不够, 层还开着会一直扫我们上一帧的内容。
     * 用 disable+update(唯一真关的办法), 出来时靠 push_layer 重新开。 */
    if(g_mode == 2 && last_mode != 2){
        gf_layer_disable(g_layer); gf_layer_update(g_layer, 0);
        plat_log("[镜像模式] 已让出屏幕, 原厂界面可见; 只读状态并打日志\n");
    } else if(g_mode != 2 && last_mode == 2){
        g_force_full = 2; push_layer();   /* 🚨 双缓冲下必须 2 —— 两块各要一次全搬, 写 1 的话另一块永远留着旧像素 */
        plat_log("[镜像模式] 退出, 重新接管屏幕\n");
    }
    last_mode = g_mode;
    if(g_mode != 2) yield_check();
}

/* 串口停止开关: `touch /tmp/studio.stop`。**停 studio 一律用它, 别 slay。** */
int plat_should_stop(void){
    int fd = open("/tmp/studio.stop", O_RDONLY, 0);
    if(fd < 0) return 0;
    close(fd); return 1;
}

/* 把 800×480 连续缓冲拷进层 surface(按 stride), 再推一帧。
 * ⚠️ 保活重申最快 1 秒一次 —— 更快会打满单线程 gdc 服务端。 */
/* ================= 上屏: 影子缓冲 + 只搬变了的段 =================
 *
 * 【为什么必须这么做】台架实测: 整屏 768000 字节逐个 u16 写进显存 = **258ms**(约 3MB/s)。
 *   显存是非缓存慢总线, 每个 16 位存储都是一次独立总线事务。258ms ⇒ 满屏重画上限只有 ~4fps,
 *   交互根本不可能跟手。
 *
 * 【两条杠杆, 都用上】
 *   ① 写得更少: 留一份"上一帧"影子(普通 RAM, **走缓存所以比对很便宜**),
 *      逐行比对, 只把真正变了的那一段写进显存。时钟跳一分钟只搬几十行的几十个像素。
 *   ② 写得更宽: 用 u32 一次搬两个像素, 总线事务数减半。
 *      (stride 1600 字节 / 每行 800 像素都是偶数, base 也 4 字节对齐, 所以 u32 安全。)
 *
 * 【正确性】影子只有在"显存内容确实等于影子"时才可信。以下情况必须整屏强制重搬:
 *   首帧 / 让出态恢复后(那期间原厂在画) / 换过 surface。 */
/* 把第 y 行 [x0,x1) 这段搬进显存, 用 u32 */
static void blit_span(int y, int x0, int x1){
    const u32 *s;
    u32       *d;
    if(!g_wide){                                  /* 对齐守卫没过 -> 退回慢但绝对安全的 u16 */
        const u16_ *ss = g_fb + y*SCR_W; u16_ *dd = g_vaddr + y*g_stride_px;
        int k; for(k=x0;k<x1;k++) dd[k]=ss[k];
        return;
    }
    s = (const u32*)(const void*)(g_fb     + y*SCR_W       + x0);
    d = (u32*)      (void*)      (g_vaddr  + y*g_stride_px + x0);
    int n = (x1 - x0) >> 1;                 /* u32 个数; x0/x1 已对齐到偶数 */
    int i = 0;
    for(; i + 8 <= n; i += 8){              /* 展开 8 次, 摊薄循环开销 */
        d[i]=s[i]; d[i+1]=s[i+1]; d[i+2]=s[i+2]; d[i+3]=s[i+3];
        d[i+4]=s[i+4]; d[i+5]=s[i+5]; d[i+6]=s[i+6]; d[i+7]=s[i+7];
    }
    for(; i < n; i++) d[i] = s[i];
}

/* 返回本帧实际搬了多少行。
 * 双缓冲下 g_vaddr/g_shadow 已经指向"当前正在画的那块"和它自己的影子, 逻辑完全一样。 */
static int present_diff(void){
    int y, rows = 0;
    const int NW = SCR_W >> 1;              /* 每行的 u32 个数 = 400 */
    u16_ *shadow = g_dbuf && g_bank ? g_shadowB : g_shadow;
    for(y = 0; y < SCR_H; y++){
        const u32 *a = (const u32*)(const void*)(g_fb   + y*SCR_W);
        u32       *b = (u32*)      (void*)      (shadow + y*SCR_W);
        int i, first = -1, last = -1;
        if(g_force_full > 0){ first = 0; last = NW - 1; }
        else {
            for(i = 0; i < NW; i++) if(a[i] != b[i]){ first = i; break; }
            if(first < 0) continue;                       /* 整行没变 */
            for(i = NW - 1; i > first; i--) if(a[i] != b[i]) break;
            last = i;
        }
        blit_span(y, first << 1, (last + 1) << 1);
        for(i = first; i <= last; i++) b[i] = a[i];       /* 同步这一块自己的影子 */
        rows++;
    }
    return rows;
}

/* 画完之后把刚画好的那块推上前台, 并把 g_vaddr 指向另一块准备画下一帧。
 * 🚨 换页必须是"整块一次提交" —— 这正是双缓冲的意义: 屏上要么是旧帧要么是新帧, 没有中间态。 */
static unsigned g_last_swap = 0;
static void swap_bank(void){
    gf_surface_t cur = g_bank ? g_surf2 : g_surf;
    /* 🚨 换页频率必须限住。gf_layer_update 是有前科的地方(紧循环里 REPLY-block 死锁,
     *   2026-08-05 亲自踩过, pidin 显示 REPLY 4104)。原来是 1 秒一次, 现在每帧都要换页 ——
     *   所以限到 20Hz, 而且用 NO_WAIT_VSYNC 不等垂直同步。静止画面本来就不换(rows==0)。 */
    unsigned now = plat_now_ms();
    if(now - g_last_swap < 50) return;         /* 太快就先不换, 下一帧再说(内容已经在后台备好) */
    g_last_swap = now;
    gf_layer_set_surfaces(g_layer, &cur, 1);
    /* set_surfaces 会冲掉其它绑定, 全部重申(这条是 08-05 实证的铁律) */
    gf_layer_set_blending(g_layer, (gf_alpha_t*)g_alphablk);
    gf_layer_set_src_viewport(g_layer, 0, 0, SCR_W-1, SCR_H-1);
    gf_layer_set_dst_viewport(g_layer, 0, 0, SCR_W-1, SCR_H-1);
    gf_display_set_layer_order(g_disp, g_order, 0);
    gf_layer_enable(g_layer);
    gf_layer_update(g_layer, GF_LAYER_UPDATE_NO_WAIT_VSYNC);
    g_front = cur;                                        /* 记住前台, 周期重申要用 */
    /* swap_bank 本身就是完整的重申序列(set_surfaces->blending->两个 viewport->order->enable->update),
     * 刚跑完没必要 1ms 后再来一遍 —— 给重申计时续期。
     * ⚠️ 必须放在上面 50ms 早退**之后**: 被限速吞掉时不续期, 尾部 1Hz 重申照常兜底。 */
    g_last_reassert = plat_now_ms();
    fp_learn_bank(g_bank);   /* ← 刚推上去的就是这一块, 现在读到的指纹按定义是我们的 */
    g_bank = !g_bank;                                     /* 下一帧画另一块 */
    g_vaddr = g_bank ? g_va2 : g_va1;
}

/* 🔬 一次性基准: 同一块数据, 几种写法各跑几遍, 让数字说话而不是猜。
 *   触发: 台架上 `>/tmp/studio_bench`。只跑一次(结果进 /tmp/studio.log)。 */
static void bench_copy(void){
    int rep, y, x;
    unsigned t;
    plat_log("=== 拷贝基准 (768000 字节/次, 各 3 遍取平均) ===\n");

    t = plat_now_ms();
    for(rep=0; rep<3; rep++)
        for(y=0;y<SCR_H;y++){
            const u16_ *s = g_fb + y*SCR_W; u16_ *d = g_vaddr + y*g_stride_px;
            for(x=0;x<SCR_W;x++) d[x] = s[x];
        }
    plat_log("  ① u16 逐像素(原做法) = "); p_logd((int)((plat_now_ms()-t)/3)); plat_log(" ms\n");

    t = plat_now_ms();
    for(rep=0; rep<3; rep++)
        for(y=0;y<SCR_H;y++){
            const u32 *s = (const u32*)(const void*)(g_fb + y*SCR_W);
            u32 *d = (u32*)(void*)(g_vaddr + y*g_stride_px);
            for(x=0;x<(SCR_W>>1);x++) d[x] = s[x];
        }
    plat_log("  ② u32 一次两像素     = "); p_logd((int)((plat_now_ms()-t)/3)); plat_log(" ms\n");

    t = plat_now_ms();
    for(rep=0; rep<3; rep++)
        for(y=0;y<SCR_H;y++) blit_span(y, 0, SCR_W);
    plat_log("  ③ u32 + 展开8次      = "); p_logd((int)((plat_now_ms()-t)/3)); plat_log(" ms\n");

    /* 纯 RAM 比对成本: 影子与当前帧完全相同 -> 一行都不搬, 量的就是比对本身 */
    { int i; for(i=0;i<SCR_W*SCR_H;i++) g_shadow[i] = g_fb[i]; }
    t = plat_now_ms();
    for(rep=0; rep<3; rep++){ g_force_full = 0; present_diff(); }
    plat_log("  ④ 只比对不搬(纯RAM)  = "); p_logd((int)((plat_now_ms()-t)/3)); plat_log(" ms\n");
    plat_log("  ⇒ 稳态一帧成本 ≈ ④ + 变化部分的 ③\n");
    g_force_full = 2;
}

void plat_present(void){
    unsigned now, t_copy;
    static unsigned acc = 0, cnt = 0, accrow = 0;
    int rows;
    if(!g_vaddr) return;
    if(g_mode == 2){ g_force_full = 2; return; }   /* 镜像模式: 一个像素都不上屏 */
    if(!g_cover){ g_force_full = 2; return; }      /* 让开态: 屏幕是原厂的, 我们一个像素都不写 */
    if(g_yield){ g_force_full = 2; return; }  /* 让出期间原厂在画 -> 回来两块都要重搬 */
    if(g_mode == 0) draw_selftest(g_fb, plat_now_ms());

    if(!g_bench_done){
        int fd = open("/tmp/studio_bench", O_RDONLY, 0);
        if(fd >= 0){ close(fd); g_bench_done = 1; bench_copy(); }
    }

    now = plat_now_ms();
    rows = present_diff();
    if(g_dbuf && rows > 0) swap_bank();       /* 有变化才换页; 没变化换了也是白费一次 update */
    if(g_force_full > 0) g_force_full--;
    t_copy = plat_now_ms() - now;
    acc += t_copy; cnt++; accrow += (unsigned)rows;
    if(cnt >= 25){
        plat_log("[计时] 上屏="); p_logd((int)(acc/25));
        plat_log("ms 渲染="); p_logd((int)g_render_ms);
        plat_log("ms 搬行="); p_logd((int)(accrow/25));
        plat_log("/480 模式="); p_logd(g_mode);
        /* 🔍 修好的量化判据: **重申改前台次数必须恒为 0**。
         *   修之前(或 touch /tmp/studio_oldfront)它应该每 ~2 秒 +1 —— 那是可对照的基线。
         *   ⚠️ 只看到 0 不算数, 必须先拿到非 0 的基线, 否则等于拿恒成立的断言当证据。 */
        plat_log(" 重申改前台="); p_logd((int)g_reassert_flip); plat_log("\n");
        acc = 0; cnt = 0; accrow = 0;
    }
    /* 🚨🚨 gf_layer_update **绝不能每帧调** —— 它默认等 vsync, 在紧循环里会 REPLY-block 在
     *   gdcServerCarmine, 整个进程死锁(画一帧后屏幕不再更新, 连命令都不读了)。
     *   2026-08-05 我在探针上亲自踩了一次: pidin 显示 `REPLY 4104`, 就是这个。
     *   隔壁 2026-07-20 早记过"安全频率约 1 秒", 我第一版没照做。
     *   ⇒ 像素随便写(纯内存, 不阻塞), 但**推屏 1 秒最多一次**。 */
    /* 🚨 重申必须是**完整序列**(push_layer): set_surfaces 会冲掉 blending 和几何绑定,
     *   只补 enable/set_surfaces/order/update 等于每秒把层打回缺绑定态。 */
    now = plat_now_ms();
    if(now - g_last_reassert >= 1000){
        push_layer();                           /* ← 只在这里, 1 秒一次 */
        g_last_reassert = now;
    }
}

/* ================= 状态: 读 PCM3Root 内存(adump 只读) ================= */
/* V4 音量链(唯一有因果活体证据的一条, 见 memory floating-volume-osd):
 *   堆里三重校验找 V -> P = u32@(V+0x168) -> vol = u8@(P+0x7c) */
#define V4_VVT   0x085c76fcu
#define V4_OVT   0x085c4c5cu
#define HEAP_LO  0x0866e200u
#define HEAP_HI  0x08a00000u
#define SCAN_CHUNK 0x10000

static int g_as = -1;
static u32 g_V = 0, g_P = 0;
static unsigned char g_scan[SCAN_CHUNK];

static int fd_rd(int fd, u32 va, void *b, int n){
    if(fd < 0) return -1;
    if(lseek(fd,(long)va,0) != (long)va) return -1;
    return read(fd,b,n);
}
static int as_rd(u32 va, void *b, int n){ return fd_rd(g_as, va, b, n); }
static int rd32(u32 va, u32 *o){ u32 v=0; if(as_rd(va,&v,4)!=4) return -1; *o=v; return 0; }
static int rd8 (u32 va, u8_ *o){ u8_ v=0; if(as_rd(va,&v,1)!=1) return -1; *o=v; return 0; }

/* 🚨 只读打开。原来有 `if(fd<0) fd=open(p,O_RDWR,0)` 兜底 —— 对**活体原厂进程**持有可写 fd
 *   毫无必要, 却把"某处笔误写进原厂 HMI 内存"这条路留着。零成本去掉。 */
static int open_as_pidfile(const char *pidfile){
    char p[40]; const char *a="/proc/", *b="/as";
    char num[12], t[12]; int i=0,j,k=0,n=0,pid=0,fd;
    fd = open(pidfile,O_RDONLY,0);
    if(fd>=0){ char bb[16]; int nn=read(fd,bb,15); close(fd);
        for(j=0;j<nn && bb[j]>='0' && bb[j]<='9';j++) pid = pid*10 + (bb[j]-'0'); }
    if(pid<=0) return -1;      /* 别写死 PID —— 变了就静默读到别的进程 */
    while(a[i]){ p[i]=a[i]; i++; }
    { int q=pid; if(!q) num[n++]='0'; else { while(q){ t[k++]=(char)('0'+q%10); q/=10; } while(k) num[n++]=t[--k]; } }
    for(j=0;j<n;j++) p[i++]=num[j];
    j=0; while(b[j]) p[i++]=b[j++]; p[i]=0;
    return open(p,O_RDONLY,0);          /* 只读, 绝不 O_RDWR */
}
static int open_as(void){ return open_as_pidfile(PIDFILE); }
/* 同上, 但 O_RDWR。**只给触摸门用** —— 它要写 PCM3Reload 里那一个 u32。
 * ⚠️ 上面那个是**故意只读**的(读状态的路径绝不该有写能力); 这里单开一个, 别把两者合并。 */
static int open_as_pidfile_rw(const char *pidfile){
    char p[40]; const char *a="/proc/", *b="/as";
    char num[12], t[12]; int i=0,j,k=0,n=0,pid=0,fd;
    fd = open(pidfile,O_RDONLY,0);
    if(fd>=0){ char bb[16]; int nn=read(fd,bb,15); close(fd);
        for(j=0;j<nn && bb[j]>='0' && bb[j]<='9';j++) pid = pid*10 + (bb[j]-'0'); }
    if(pid<=0) return -1;
    while(a[i]){ p[i]=a[i]; i++; }
    { int q=pid; if(!q) num[n++]='0'; else { while(q){ t[k++]=(char)('0'+q%10); q/=10; } while(k) num[n++]=t[--k]; } }
    for(j=0;j<n;j++) p[i++]=num[j];
    j=0; while(b[j]) p[i++]=b[j++]; p[i]=0;
    return open(p,O_RDWR,0);
}

/* ============ 命令信箱: 直接写 PCM3Root 的一个死 RW 字 ============
 *
 * 【为什么是这个方案】cave 跑在原厂一个时间敏感的线程上(它同时在写 OnOff 的 IPC 通道)。
 *   2026-08-06 实测: cave 在那个线程里做文件 I/O, 原厂立刻报
 *   `CGOnOffDevCtrl.cpp:221 ASSERTION failed` + `IPC-OnOff: Error! mIPCChannel.write()`。
 *   **零调用的那一级也崩** ⇒ 病因是 syscall 数量, 不是"调原厂函数"。
 *   隔壁已验证安全的 cave 只有 4 次 syscall, 我那版武装态下有 10~14 次。
 *   ⇒ 信箱改成**一个内存字**: cave 每拍只做一次 load, **零 syscall**。
 *
 * 【信箱地址怎么选的 —— 三重证明, 与当初选代码 cave 同一套功夫】
 *   0x0865acc8 起 8192 字节:
 *     ① 原厂镜像里全零
 *     ② **真车 + 台架活体 dump 里恒为零**(两份 16MB 快照逐字节验)
 *     ③ **代码段里 0 处池字引用**(对比另一候选段有 3338 处)
 *   而且它落在可写段 0x08643cc8..0x0866e1fc 内, 4 字节对齐。
 *
 * 【为什么写得进去】memory cow-wall-runtime-code-injection-impossible 那条记的是
 *   "/proc/as 只写得了 RW, 写不了 RO code" —— 我一直当限制记, 它同时是**能力**:
 *   RW 段是可以写的。信箱正在 RW 段里。
 *
 * 🚨 只写信箱那几十个字节, 绝不写任何原厂数据。写之前先校验它还是全零/我们上次写的值。
 *
 * 【2026-08-07 换成"傀儡事件"协议】老的 4 字白名单版是 cave **在发帖方线程里直接 jsr 原厂函数**,
 *   08-06 阶梯实测到"合法参数真切音源 -> PCM3Root 崩"—— 线程亲和性从风险变成了实测结论。
 *   新协议改成: cave 只把一个**假事件**投进 AudioCtrlThread 的队列, 真正的调用发生在
 *   **原厂自己的 dispatch 循环里**(假 vtable 的 vt[4])。信箱布局见 puppet_addr.h,
 *   那个头文件由 dev/build_flash_bench_puppet.py 与 flash 里的 cave **同一次生成** ——
 *   刷写包和 studio 二进制共用一个真源, 不会再出现 8-06 那种"手抄常量对不上"。 */

static int g_as_w = -1;               /* PCM3Root 的**可写**句柄; 只给信箱用 */

static int open_as_rw(void){
    char p[40]; const char *a="/proc/", *b="/as";
    char num[12], t[12]; int i=0,j,k=0,n=0,pid=0,fd;
    fd = open(PIDFILE,O_RDONLY,0);
    if(fd>=0){ char bb[16]; int nn=read(fd,bb,15); close(fd);
        for(j=0;j<nn && bb[j]>='0' && bb[j]<='9';j++) pid = pid*10 + (bb[j]-'0'); }
    if(pid<=0) return -1;
    while(a[i]){ p[i]=a[i]; i++; }
    { int q=pid; if(!q) num[n++]='0'; else { while(q){ t[k++]=(char)('0'+q%10); q/=10; } while(k) num[n++]=t[--k]; } }
    for(j=0;j<n;j++) p[i++]=num[j];
    j=0; while(b[j]) p[i++]=b[j++]; p[i]=0;
    return open(p, O_RDWR, 0);
}

static int      g_v4_fail = 0;      /* 连续定位失败次数 */
static unsigned g_v4_last = 0;      /* 上次重扫时刻 */
static void locate_v4(void){
    u32 va, t1, t2;
    g_V = g_P = 0;
    for(va=HEAP_LO; va<HEAP_HI; va += SCAN_CHUNK-4){
        int got = as_rd(va,g_scan,SCAN_CHUNK), j;
        if(got < 8) continue;
        for(j=0;j+3<got;j+=4){
            u32 w = (u32)g_scan[j] | ((u32)g_scan[j+1]<<8) | ((u32)g_scan[j+2]<<16) | ((u32)g_scan[j+3]<<24);
            u32 X = va + (u32)j;
            if(w != V4_VVT) continue;
            if(rd32(X+0x160,&t1)==0 && t1 == X-0x218 &&
               rd32(X-0x218,&t2)==0 && t2 == V4_OVT){
                g_V = X; rd32(g_V+0x168,&g_P);
                plat_log("V4: V="); p_logh(g_V); plat_log(" P="); p_logh(g_P); plat_log("\n");
                return;
            }
        }
    }
    plat_log("V4: 没定位到\n");
}

/* ============ 当前页 id: 锚定 PCM3Reload 的 CHBMenuManagerImpl ============
 *
 * 【为什么能这么读】我们的 UI 盖住了屏幕, 但**原厂仍然在解释用户输入**
 *   (按键/旋钮 → IOC → SPHKeyInput → MoCCA), 它照样切自己的页。
 *   所以我们不用去抢那条事件流(2026-07-01 实测: 抢 /dev/ipc/ioc/chN 把真车挂了),
 *   只要**只读地镜像原厂的结果**就行 —— 原厂当输入解释器, 我们当表现层。
 *
 * 【四条件锚定】单靠 vtable 会命中 5 个, 其中 3 个是 rodata 里的构造 vtable
 *   (三份 dump 里字节完全相同)—— 正是 R6 那次误命中的坑。加上后三条筛掉:
 *     u32@obj+0x00 == 0x0932fbbc   主 vptr
 *     u32@obj+0x04 == 0x0932fc5c   第二 vtable(多继承)
 *     u32@obj+0x0c == 0x0001009c   主 HMI(0x0001009f 是仪表 HMI, 不要它)
 *     obj >= 0x09600000            排除 rodata 常量池
 *   离线复验: 3 份 dump 各**恰好命中 1 个**; 蓝牙页 id=375, 真车 FM 页 id=855。
 *
 * ⚠️ id 是 **u16**(反汇编是 mov.w)。KB 里"菜单 id 是 8 位"是误读 ——
 *    `%02X` 是最小字段宽度不是截断, 0x177 照样打成 177。375/855 本身就超 u8。
 * ⚠️ KB 里"页 id 硬钳在 [100,899]"的出处是**图文电视页号校验**(CHN 911 没 TV 调谐器),
 *    跟 HMI 页无关, 不要拿它当过滤器。 */
#define MM_VPTR1  0x0932fbbcu
#define MM_VPTR2  0x0932fc5cu
#define MM_HMI    0x0001009cu
#define MM_LO     0x09600000u
#define MM_HI     0x0a600000u
#define MM_OFF_ID 0x9a

static int  g_rl = -1;          /* PCM3Reload 的 /proc/as fd(只读) */
static u32  g_mm = 0;           /* 命中的 CHBMenuManagerImpl 对象地址 */
static int  g_mm_fail = 0;
static unsigned g_mm_last = 0;

static u32 le32(const unsigned char *p){
    return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}
static void locate_menumgr(void){
    u32 va; int hits = 0;
    g_mm = 0;
    for(va = MM_LO; va < MM_HI; va += SCAN_CHUNK - 16){
        int got = fd_rd(g_rl, va, g_scan, SCAN_CHUNK), j;
        if(got < 16) continue;
        for(j = 0; j + 15 < got; j += 4){
            if(le32(g_scan+j)   != MM_VPTR1) continue;
            if(le32(g_scan+j+4) != MM_VPTR2) continue;
            if(le32(g_scan+j+12)!= MM_HMI)   continue;
            { u32 X = va + (u32)j;
              if(X < MM_LO) continue;
              if(!g_mm) g_mm = X;
              hits++; }
        }
    }
    plat_log("菜单管理器: 命中 "); p_logd(hits);
    if(g_mm){ plat_log(" 个, 用 "); p_logh(g_mm); }
    /* 命中数必须是 1。>1 说明判据还不够紧, **别猜**, 直接当失败处理。 */
    if(hits != 1){ plat_log("  ⚠命中数不是1 -> 判据不可信, 不用它"); g_mm = 0; }
    plat_log("\n");
}
static int read_page_id(void){
    unsigned char b[2];
    if(g_mm && fd_rd(g_rl, g_mm + MM_OFF_ID, b, 2) == 2)
        return (int)b[0] | ((int)b[1] << 8);
    return -1;
}

/* ============ 触摸: 只读镜像 PCM3Reload 的 CHBKey2MSMEventMapper ============
 *
 * 【为什么是这个对象】它一次给全 type + x + y + 有效位, 而 CHBTSInputDispatcher 那条只给 x/y ——
 *   连点两次同一个位置就分不出来了。这里靠 type 的 3→2 沿判定"新的一次按下"。
 *
 * 【坐标已经是像素】原厂做完 TouchCalib 变换才存进来, 直接就是 800×480 屏幕坐标,
 *   我们不用再管标定文件。⚠️ 但原厂公式**没有钳位**, 靠边按会外推出界 -> 我们自己 clamp。
 *
 * 【全程只读】open("/proc/<pid>/as", O_RDONLY) + lseek + read —— 跟页 id/音源/音量同一条路,
 *   不碰 /dev/ipc/ioc/chN, 不消费原厂任何数据。
 *
 * 🚨🚨 **我们的层不拦截触摸**(硬件叠加层)。用户点我们按钮的同一下, **原厂也收到了**,
 *   并会去命中它自己在那个位置的控件。⇒ 自由布局 = 一次点击可能触发两个动作。
 *   两条出路: ①"对齐式换肤"(我们的按钮画在原厂可点控件正上方, 让原厂自己去切)——零风险;
 *             ②自由布局 + 刷 flash 装 cave 直调原厂函数 —— 台架限定。
 *   在没选定之前, 我们只用触摸做**我们自己页面之间的导航**(那部分不需要原厂配合)。 */
#define KM_V0 0x091b5f0cu
#define KM_V1 0x091b5f34u
#define KM_V2 0x091b5f44u
#define KM_V3 0x091b5f58u
#define KM_OFF_P     0x98
#define KM_OFF_TYPE  0x78
#define KM_OFF_X     0x7c
#define KM_OFF_Y     0x7e
#define KM_OFF_STATE 0x8c

static u32 g_km = 0, g_kmP = 0;
static int g_km_fail = 0;
static unsigned g_km_last = 0;
static int g_ts_down = 0;      /* 1 = 当前手指还按着(必须见到抬起才接受下一次按下) */

static void locate_keymapper(void){
    u32 va; int hits = 0;
    g_km = g_kmP = 0;
    for(va = MM_LO; va < MM_HI; va += SCAN_CHUNK - 48){
        int got = fd_rd(g_rl, va, g_scan, SCAN_CHUNK), j;
        if(got < 48) continue;
        for(j = 0; j + 47 < got; j += 4){
            if(le32(g_scan+j)      != KM_V0) continue;
            if(le32(g_scan+j+0x10) != KM_V1) continue;
            if(le32(g_scan+j+0x18) != KM_V2) continue;
            if(le32(g_scan+j+0x28) != KM_V3) continue;
            if(!g_km) g_km = va + (u32)j;
            hits++;
        }
    }
    plat_log("按键/触摸映射器: 命中 "); p_logd(hits);
    if(hits != 1){ plat_log(" 个 ⚠不是1 -> 判据不可信, 不用它\n"); g_km = 0; return; }
    { unsigned char b[4];
      if(fd_rd(g_rl, g_km + KM_OFF_P, b, 4) == 4) g_kmP = le32(b); }
    plat_log(" 个, X="); p_logh(g_km); plat_log(" P="); p_logh(g_kmP); plat_log("\n");
}

/* 读一次触摸。返回 1 = 有**新的按下**(3->2 沿), ev 已填好。
 * 一次读回 24 字节消除撕裂(别分三次读, 中间可能被改)。 */
static int read_touch(PcmEvent *ev){
    unsigned char b[24];
    u32 type, state; int x, y;
    if(!g_kmP) return 0;
    if(fd_rd(g_rl, g_kmP + KM_OFF_TYPE, b, 24) != 24) return 0;
    type  = le32(b + 0);
    x     = (int)(short)(b[KM_OFF_X - KM_OFF_TYPE] | (b[KM_OFF_X - KM_OFF_TYPE + 1] << 8));
    y     = (int)(short)(b[KM_OFF_Y - KM_OFF_TYPE] | (b[KM_OFF_Y - KM_OFF_TYPE + 1] << 8));
    state = le32(b + (KM_OFF_STATE - KM_OFF_TYPE));
    if(state != 2){ g_ts_down = 0; return 0; }            /* 不是 DATA_OK, 整帧作废并复位按压态 */
    if(x < 0) x = 0; if(x > SCR_W-1) x = SCR_W-1;         /* 原厂公式不钳位, 我们自己来 */
    if(y < 0) y = 0; if(y > SCR_H-1) y = SCR_H-1;
    /* 🚨 判"新的一次按下"不能只看 `type!=2 -> type==2`。
     *   2026-08-06 台架实测: 手指按住时会微抖, 原厂在按压过程中会穿插报中间态,
     *   于是一次按压被判成 2~4 次(实录: 34,14 / 34,13 / 34,16 / 34,14)。
     *   正解: **必须先看到明确的抬起(type 3), 下一次按下才算新的一次。**
     *   type: 2=按下 3=抬起 0=拖动/中间态。 */
    if(type == 3){                                        /* 抬起 */
        if(g_ts_down){
            g_ts_down = 0;
            ev->type = EV_TOUCH_UP; ev->which = 0; ev->arg = 0; ev->x = x; ev->y = y;
            return 1;
        }
        return 0;
    }
    if(type == 2 && !g_ts_down){                          /* 抬起之后的第一次按下 */
        g_ts_down = 1;
        ev->type = EV_TOUCH_DOWN; ev->which = 0; ev->arg = 0; ev->x = x; ev->y = y;
        plat_log("[触摸] 按下 x="); p_logd(x); plat_log(" y="); p_logd(y); plat_log("\n");
        return 1;
    }
    return 0;
}

/* ============ 当前音源: CPSoundPresCtrl(V 已经在 locate_v4 里找到了) ============
 * 离线在真车 dump 上复验通过: OBJ = V - 0x218, u32@OBJ == 0x085c4c5c(=V4_OVT)。
 *   +0x860 = 源 slot(实测 FM=11)   +0x870 = 源 app(实测 Tuner=1)
 * ⚠️ +0x86c **不是**"持久化的 desired 源": 真车 FM 态实测=1, 台架 BT 态=7,
 *    KB 里记的 "BT=10" 只是 childchain 那一次特定状态的巧合。别据此写补丁。 */
#define SND_OFF_SLOT 0x860
#define SND_OFF_APP  0x870

static int s_page = -1, s_slot = -1, s_app = -1;
static int s_page_last = -2, s_slot_last = -2, s_app_last = -2;

/* ═══════════════ 傀儡事件协议(studio 侧) ═══════════════
 *
 * 地址与字段偏移**全部**来自 platform/puppet_addr.h —— 那个文件由
 * dev/build_flash_bench_puppet.py 在打刷写包时**从 dev/build_puppet_cave.py 读出来**生成,
 * 和 flash 里那份 cave 出自同一次构建。**这里一个地址都不许写死。**
 *
 * 【为什么是"傀儡"】8-06 阶梯实测: cave 在**发帖方线程**里直接调 PresCtrl -> PCM3Root 崩
 * (线程亲和性从风险变成了结论)。所以 cave 现在只把一个假事件 post 进 AudioCtrlThread 的
 * 队列, 真正的调用发生在**原厂自己的 dispatch 循环里**(假 vtable 的 vt[4])。
 *
 * 【信箱三块】CTL(控制/回执) + EV(假事件) + VT(假 vtable) —— 全在死 RW 区 0x0865ACC8..AD48。
 *
 * 【阶梯】(上机顺序见 references/puppet-bringup-checklist.md)
 *   级 0  诊断读   纯读, **一个字节都不写**。cave 装没装/池字指哪/OBJ 锚到没有,
 *                  外加 F2 那个**没收敛**的候选 *(0x0864b500) —— 离线永远判不了, 只能上机读。
 *   级 1  空跑     整条链跑通(hook 投递 -> AudioCtrlThread 派发 -> vt[4] 落地),
 *                  但 vt[4] 只回写两个线程索引就返回, **一个原厂字段都不碰**(XRC=10)。
 *                  ⇒ 这一级直接拿到 8-06 想要的两个数: 我们原来在哪条线程、傀儡把我们送到了哪条。
 *   级 2  真切源   vt[4] 过完 OBJ 三道门后真调 entertSourceChanged(XRC=1)。
 *   级 8  解卡     BUSY 写 0(一次字节写)。通道卡住时用, 不用重启。
 *   级 9  清零     整个信箱抹零 -> LEVEL=0 -> cave 回到完全惰性。
 *
 * 【惰性保证】hook 的第一件事就是读 LEVEL 那**一个字节**, 是 0 就直通。
 *   我们不写信箱, 信箱就全零 = 级 0 = 与原厂零差别; 重启后 RAM 从 ELF 重载, 又是全零。
 *   ⇒ "什么都不做"永远是安全默认态。 */

#define PUP_TIMEOUT_TICKS 80          /* 25ms/拍 -> 2 秒 */

static int      g_pup_wait = 0;       /* >0 = 正在等回执 */
static unsigned g_pup_ack0 = 0;       /* 武装前的 ACK 值 */
static int      g_pup_level = 0;
static int      g_pup_op = 0;         /* ★ 这一发用的白名单序号(回执解读要用) */
static const u32 g_pup_vt[PUP_VT_SLOTS] = PUP_VT_INIT;

/* ★ 白名单每一行的 (期望 vptr, tid, flags) —— 全部来自自动生成的 puppet_addr.h。
 * 表本体在 flash 里是**只读**的, studio 拿不到也改不了; 这三张小表只是让 studio 知道
 * "这一行要哪个单例 / 会落到哪条线程 / arg0 走 CMD 还是走 ARG0"。
 * 🚨 一个地址都不许在这里手抄 —— 手抄就会和 flash 漂。 */
static const u32 g_row_vptr[PUP_NROWS]  = PUP_ROW_VPTR_INIT;
static const int g_row_tid[PUP_NROWS]   = PUP_ROW_TID_INIT;
static const u32 g_row_flags[PUP_NROWS] = PUP_ROW_FLAGS_INIT;

static int mbwr(u32 va, u32 v){
    if(g_as_w < 0) return -1;
    if(lseek(g_as_w, (long)va, 0) != (long)va) return -1;
    return (write(g_as_w, &v, 4) == 4) ? 0 : -1;
}
/* 单字节写 —— BUSY / LEVEL 是同一个字里的两个字节, **绝不能读改写整字**:
 * 那会跟 cave 的 tas.b 抢, 有机会把它刚拿到的锁抹掉(正是 F3 那类竞态)。 */
static int mbwr8(u32 va, unsigned char v){
    if(g_as_w < 0) return -1;
    if(lseek(g_as_w, (long)va, 0) != (long)va) return -1;
    return (write(g_as_w, &v, 1) == 1) ? 0 : -1;
}
static int mbopen(void){
    if(g_as_w < 0){
        g_as_w = open_as_rw();
        plat_log("[傀儡] 可写句柄 fd="); p_logd(g_as_w); plat_log("\n");
    }
    return g_as_w;
}
static void pup_dump(const char *tag){
    int i; u32 w;
    plat_log(tag); plat_log(" CTL:");
    for(i = 0; i * 4 < PUP_CTL_SPAN; i++){
        if(rd32(PUP_CTL + (u32)i*4, &w) == 0){ plat_log(" "); p_logh(w); } else plat_log(" ????????");
    }
    plat_log("\n         EV:");
    for(i = 0; i * 4 < PUP_EV_SPAN; i++){
        if(rd32(PUP_EV + (u32)i*4, &w) == 0){ plat_log(" "); p_logh(w); } else plat_log(" ????????");
    }
    plat_log("\n         VT:");
    for(i = 0; i < PUP_VT_SLOTS; i++){
        if(rd32(PUP_VT + (u32)i*4, &w) == 0){ plat_log(" "); p_logh(w); } else plat_log(" ????????");
    }
    plat_log("\n");
}
/* 信箱现在的样子: 0=全零(干净)  1=我们装的  -1=不认得/读不到 -> 立刻停手。
 * 🚨 原厂哪天真把这块用起来了, 我们要能立刻发现并停手, 而不是照写不误。 */
static int pup_known(void){
    u32 w; int i, allz = 1;
    for(i = 0; i * 4 < PUP_CTL_SPAN + PUP_EV_SPAN; i++){
        if(rd32(PUP_CTL + (u32)i*4, &w) != 0) return -1;
        if(w) allz = 0;
    }
    for(i = 0; i < PUP_VT_SLOTS; i++){
        if(rd32(PUP_VT + (u32)i*4, &w) != 0) return -1;
        if(w) allz = 0;
    }
    if(allz) return 0;
    if(rd32(PUP_VT + 16, &w) == 0 && w == PUP_EXEC &&
       rd32(PUP_EV, &w) == 0 && w == PUP_VT) return 1;
    return -1;
}
/* 活体锚定 CPSoundPresCtrl 单例。**绝不写死** —— 台架 disconnected/AUX 是 086EC01C,
 * 但 BT_playing 快照是 086ED694, 真车又是 086EF01C。按 vptr 认对象, 不认地址。
 * 🚨 顺序: **先对齐门 -> 再范围门 -> 最后才解引用**(与 cave 侧 exec 的三道门同序)。 */
static u32 pup_obj(void){
    u32 obj, vp;
    if(!g_V) return 0;
    obj = g_V - 0x218u;
    if(obj & 3u) return 0;                                  /* ① 对齐 */
    if(obj < PUP_OBJ_LO || obj >= PUP_OBJ_HI) return 0;     /* ② 范围 */
    if(rd32(obj, &vp) != 0) return 0;                       /* ③ 读得到吗 */
    if(vp != PUP_VPTR) return 0;                            /*    vptr 对不对 */
    return obj;
}
/* ★ 按**行要的 vptr** 挑单例。台架的 Tuner 和真车的 Sound 都是 0x086EF01C ——
 * 同一个地址在两台机器上是不同的类, 所以认 vptr 不认地址, 而且两边都是活体扫出来的:
 *   Sound = locate_v4() 的 g_V - 0x218;  Tuner = locate_root2() 的 g_TUN(三条件 + 命中数==1)。
 * 认不出来的 vptr 一律返回 0 = 拒绝武装, 不猜。 */
static u32 g_TUN;                     /* 前向: 定义在下面的收音机那一节(三条件 + 命中数==1) */
static void locate_root2(void);       /* 同上 */
static u32 pup_obj_for(int op){
    u32 want, obj, vp;
    if(op < 0 || op >= PUP_NROWS) return 0;
    if(g_row_flags[op] & PUP_F_NOTHIS) return 0;   /* 这一行本来就不吃 this */
    want = g_row_vptr[op];
    if(want == PUP_VPTR_SOUND)      obj = pup_obj();
    else if(want == PUP_VPTR_TUNER) obj = g_TUN;
    else                            return 0;
    if(!obj) return 0;
    if(obj & 3u) return 0;                                  /* 与 cave 侧 exec 同序 */
    if(obj < PUP_OBJ_LO || obj >= PUP_OBJ_HI) return 0;
    if(rd32(obj, &vp) != 0 || vp != want) return 0;
    return obj;
}
/* 🚨🚨 "没有在飞的事件"的**精确不变式**: 入队数 == 回执数, 且假事件不挂在任何队列上。
 *
 * 这道门是 F3 的真正防线, 别用 BUSY 代替它。cave 里的 `tas.b` 只保证**两条原厂线程**
 * 不会同时投, 它拦不住"studio 自己把 BUSY 清了再投一次"。那一投会重写 EV+0x08,
 * 把**排在假事件后面的原厂事件从链上摘掉** —— 离线复现过:
 *     count 继续加, 但链上只剩 1 个节点 -> 队列永远抽不干
 *     -> AudioCtrlThread 从此收不到任何事件(不是砖, 但只能断电)
 * 形状是"跑 N 次都好, 某一次车里音频突然全哑"。**单线程模拟器结构上看不见这个。**
 *
 * ⚠️ 危险方向是 **NPOST > ACK**(投了没落地)。反过来 ACK > NPOST 是计数异常, 也一样拦。
 *
 * ⚠️ **别高估 EV+8 那一项**(反汇编 pushBack 0x080943B4 查清的):
 *   它入队时 `node->next = *tailp`, 而 `*tailp` 恒为 0(那条 CHBAssert 就是在保证这个)
 *   ⇒ **刚入队、排在队尾的假事件, EV+8 本来就是 0**。所以这一项抓不到常见的"在飞",
 *   它只抓"我们后面又被挂了一个原厂事件" —— 恰好是摘链要害的那一种, 所以留着有用,
 *   但**真正扛事的是 NPOST == ACK**。
 *   (好消息: popFront 0x08094428 摘节点时会 `mov #0,r1; mov.l r1,@(8,r0)` 清 next,
 *    所以 EV+8 会自愈, 不会因为一次正常派发就把这道门永久卡住。)
 *   🚨 下一个人: **别因为"EV+8 已经覆盖了"就去删调用方的 BUSY 门** —— 一删 F3 就复活。 */
static int pup_inflight(void){
    u32 npost = 0, ack = 0, nxt = 0;
    if(rd32(PUP_CTL + PUP_O_NPOST, &npost) != 0) return 1;   /* 读不到 -> 一律当成在飞 */
    if(rd32(PUP_CTL + PUP_O_ACK,   &ack)   != 0) return 1;
    if(rd32(PUP_EV  + PUP_O_EV_NEXT, &nxt) != 0) return 1;
    if(npost != ack){
        plat_log("[傀儡] ⚠ 入队数="); p_logd((int)npost);
        plat_log(" 回执数=");        p_logd((int)ack);
        plat_log(" 对不上 -> 有事件在飞, 拒绝动信箱\n");
        return 1;
    }
    if(nxt != 0){
        plat_log("[傀儡] ⚠ 有原厂事件挂在我们后面(EV+8="); p_logh(nxt);
        plat_log(") -> 拒绝动信箱(动了就摘断它)\n");
        return 1;
    }
    return 0;
}
/* 装信箱。**不写 LEVEL / 不写 CMD** —— 装完 cave 仍然完全惰性(LEVEL 还是 0)。
 * ★ op/arg0/arg2 也在这里落地(ARM 不在这里写 —— 武装位是单独一条命令, 见 pup_setarm)。 */
static int pup_install(u32 obj, u32 mode, u32 op, u32 arg0, u32 arg2){
    int i, st; u32 w;
    if(mbopen() < 0) return -1;
    st = pup_known();
    if(st < 0){ plat_log("[傀儡] ⚠ 信箱内容不认得 -> 拒绝写入(先跑级 0 看一眼)\n"); return -1; }
    if(st == 1){                                   /* 已经装过: 上一条必须跑完 */
        unsigned char busy = 0;
        /* 🚨 这道门必须在最前面, 而且**不能被级 8 绕过** —— 下面那句 EV+0x08=0
         *   是摘链的凶器, 只有确认没有在飞的事件才允许执行到那里。 */
        if(pup_inflight()){
            plat_log("[傀儡] ⚠ 有事件在飞 -> 拒绝重复武装(**别跑级 8**, 它不解决这个)\n");
            return -1;
        }
        if(rd8(PUP_CTL + PUP_O_BUSY, &busy) == 0 && busy != 0){
            plat_log("[傀儡] ⚠ BUSY 非零, 上一条还没落地 -> 不重复投递\n");
            return -1;
        }
        if(rd32(PUP_CTL + PUP_O_CMD, &w) == 0 && w != 0){
            plat_log("[傀儡] ⚠ 上一条 CMD 还没被消费 -> 不覆盖\n");
            return -1;
        }
    }
    /* 假 vtable */
    for(i = 0; i < PUP_VT_SLOTS; i++)
        if(mbwr(PUP_VT + (u32)i*4, g_pup_vt[i])) return -1;
    /* 假事件: vptr / 0 / next / 0 / 哨兵 / 保留 */
    if(mbwr(PUP_EV + 0x00, PUP_VT)) return -1;
    if(mbwr(PUP_EV + 0x04, 0)) return -1;
    if(mbwr(PUP_EV + PUP_O_EV_NEXT, 0)) return -1;
    if(mbwr(PUP_EV + 0x0c, 0)) return -1;
    if(mbwr(PUP_EV + 0x10, PUP_EV_SENTINEL)) return -1;
    for(i = 0x14; i < PUP_EV_SPAN; i += 4) if(mbwr(PUP_EV + (u32)i, 0)) return -1;
    /* 控制块。第一次装才清计数器 —— **重复武装时 ACK/NPOST 要累加**,
     * 那两个数对上不上正是 F3(重复入队)会露馅的地方, 每次清零就等于把证据擦了。 */
    if(st == 0){
        if(mbwr(PUP_CTL + 0, 0)) return -1;    /* BUSY=0 LEVEL=0 rsv=0, 一次写全 */
        if(mbwr(PUP_CTL + PUP_O_RC,    0)) return -1;
        if(mbwr(PUP_CTL + PUP_O_XRC,   0)) return -1;
        if(mbwr(PUP_CTL + PUP_O_ACK,   0)) return -1;
        if(mbwr(PUP_CTL + PUP_O_TIDP,  0)) return -1;
        if(mbwr(PUP_CTL + PUP_O_TIDX,  0)) return -1;
        if(mbwr(PUP_CTL + PUP_O_NPOST, 0)) return -1;
    }
    if(mbwr(PUP_CTL + PUP_O_CMD,   0)) return -1;
    if(mbwr(PUP_CTL + PUP_O_OBJ,   obj)) return -1;
    if(mbwr(PUP_CTL + PUP_O_MODE,  mode)) return -1;
    if(mbwr(PUP_CTL + PUP_O_SRC,   0)) return -1;
    if(mbwr(PUP_CTL + PUP_O_OP,    op)) return -1;      /* ★ 白名单序号 */
    if(mbwr(PUP_CTL + PUP_O_ARG0,  arg0)) return -1;    /* ★ 非 ARGCMD 行的 arg0(可为 0) */
    if(mbwr(PUP_CTL + PUP_O_ARG2,  arg2)) return -1;    /* ★ 第三个参数格 */
    /* 回读确认真写进去了 —— 写 /proc/as 失败是静默的, 不回读等于没验 */
    if(rd32(PUP_VT + 16, &w) != 0 || w != PUP_EXEC) return -1;
    if(rd32(PUP_EV, &w) != 0 || w != PUP_VT) return -1;
    if(rd32(PUP_CTL + PUP_O_OBJ, &w) != 0 || w != obj) return -1;
    if(rd32(PUP_CTL + PUP_O_OP,  &w) != 0 || w != op) return -1;
    pup_dump("[傀儡] 已装 ");
    return 0;
}
/* ★ 运行时武装位图。**单独一条命令**, 故意跟"投递"分开:
 *   重启后 ARM=0 ⇒ 除了序号 0, 白名单里所有行都是死的。猜错的代价 = 重启一次。
 *   studio 自己不会自动置位, 一定要人手敲 `7 <bits>`。 */
static int pup_setarm(u32 bits){
    u32 w = 0;
    int i;
    if(mbopen() < 0) return -1;
    /* 🚨 2026-08-12 上机踩到: **ARM 不能是写进信箱的第一个字**。
     *   只写 ARM 会让信箱变成"半装"(既非全零、VT/EV 又没装), `pup_known()` 判 -1,
     *   下一条命令直接被 "信箱内容不认得 -> 拒绝写入" 挡掉, 表现是"武装了却什么都跑不了"。
     *   ⇒ 全零时先把 VT/EV 装上再写 ARM。装信箱本身**不写 LEVEL/CMD**, cave 仍然完全惰性。 */
    if(pup_known() == 0 && pup_install(0, 0, 0, 0, 0) != 0){
        plat_log("[傀儡] ⚠ 装信箱失败 -> 不写 ARM\n"); return -1;
    }
    if(mbwr(PUP_CTL + PUP_O_ARM, bits)) return -1;
    if(rd32(PUP_CTL + PUP_O_ARM, &w) != 0 || w != bits){
        plat_log("[傀儡] ⚠ ARM 回读对不上 -> 没写进去\n"); return -1;
    }
    plat_log("[傀儡] ARM="); p_logh(w); plat_log("  已武装序号:");
    for(i = 0; i < PUP_NROWS; i++) if((w | 1u) & PUP_ARM_BIT(i)){ plat_log(" "); p_logd(i); }
    plat_log("\n        (序号 0 恒武装; 重启即清)\n");
    return 0;
}
/* 武装: 先 LEVEL(开惰性门), 最后 CMD(真正触发)。顺序反了会漏拍。
 *
 * ★ 加宽后统一走这一个函数, op 就是白名单序号:
 *     · F_ARGCMD 的行(只有序号 0): CMD **就是** arg0 ⇒ 不许为 0
 *     · 其余行:  CMD 只当触发, arg0 走 ARG0 字段 ⇒ **允许 0**(requestScan 就必须传 0)
 *     · F_KEYSYNTH 的行(B4): arg0 = w1 = key|source<<16, arg1 = w2 = status|slider<<16,
 *       两个都是**值**不是指针; 不吃 this。 */
static int pup_arm_op(int level, int op, u32 arg0, u32 mode, u32 arg2){
    u32 obj = 0, w = 0, cmd, fl;
    if(op < 0 || op >= PUP_NROWS){
        plat_log("[傀儡] ⚠ 序号越界(0.."); p_logd(PUP_NROWS - 1); plat_log(") -> 拒绝\n");
        return -1;
    }
    fl = g_row_flags[op];
    if(fl & PUP_F_ARGCMD){
        if(!arg0){ plat_log("[傀儡] ⚠ 这一行 arg0 走 CMD, 不能是 0(CMD=0 就是惰性) -> 拒绝\n"); return -1; }
        cmd = arg0;
    } else {
        cmd = 1u;                       /* 只当触发; 真正的 arg0 在 ARG0 字段里 */
    }
    /* 没武装就投 = 白跑一趟(cave 会回 RC=7)。提前说清楚, 别让人对着 RC 猜。
     * 序号 0 恒武装(cave 侧用的是 ARM|1), 其余必须先敲 `7 <bits>`。 */
    if(op != 0){
        if(rd32(PUP_CTL + PUP_O_ARM, &w) != 0 || !(w & PUP_ARM_BIT(op))){
            plat_log("[傀儡] ⚠ 序号 "); p_logd(op);
            plat_log(" 没武装(ARM="); p_logh(w);
            plat_log(") -> 先敲 `7 <bits>`; 现在投只会拿到 RC=7\n");
            return -1;
        }
    }
    if(level == PUP_LEVEL_REAL && !(fl & PUP_F_NOTHIS)){
        obj = pup_obj_for(op);
        /* 🚨 级 2 必须有活体锚定的 OBJ。锚不到就**不许上**, 别拿野指针赌 ——
         *   cave 的门会把它挡下来(XRC=-3/-4), 但那是最后一道网, 不是我们乱送的理由。 */
        if(!obj){
            plat_log("[傀儡] ⚠ 这一行要的单例(vptr="); p_logh(g_row_vptr[op]);
            plat_log(")没锚到/校验没过 -> 拒绝武装级 2(先跑级 0)\n");
            return -1;
        }
    } else {
        obj = 0;      /* 级 1 的 vt[4] 根本不看 OBJ(只记线程索引), 送 0 最干净 */
    }
    g_pup_op = op;
    if(pup_install(obj, mode, (u32)op, arg0, arg2) != 0) return -1;
    /* 🚨 ACK 基线读不到就**不许武装**。原来这里是 `失败 -> ack0=0`, 那是个假阳性工厂:
     *   下一拍 pup_poll 读到真实 ACK(比如上一轮留下的 3)!= 0, 立刻打印
     *   "✅ 落地 ACK=3 XRC=10 落地线程=41" —— 全是陈值, 而清单正是拿这几个数放行级 2。 */
    if(rd32(PUP_CTL + PUP_O_ACK, &w) != 0){
        plat_log("[傀儡] ⚠ 读不到 ACK 基线 -> 拒绝武装(回执判据会失真)\n");
        return -1;
    }
    g_pup_ack0 = w;
    if(mbwr8(PUP_CTL + PUP_O_LEVEL, (unsigned char)level)) return -1;   /* ① 开门 */
    if(mbwr(PUP_CTL + PUP_O_CMD, cmd)) return -1;                        /* ② 触发 */
    g_pup_level = level; g_pup_wait = PUP_TIMEOUT_TICKS;
    plat_log("[傀儡] 武装 级="); p_logd(level);
    plat_log(" 序号="); p_logd(op);
    plat_log(" arg0="); p_logd((int)arg0);
    plat_log(" arg1="); p_logd((int)mode);
    plat_log(" arg2="); p_logd((int)arg2);
    plat_log(" OBJ="); p_logh(obj);
    plat_log(" 期望落地线程="); p_logd(g_row_tid[op]);
    plat_log(level == PUP_LEVEL_REAL ? "  (真调)\n" : "  (空跑: 只回写线程索引, 原厂零接触)\n");
    return 0;
}
/* 旧接口: 序号 0(entertSourceChanged), 与加宽前的清单一字不差。 */
static int pup_arm(int level, u32 src, u32 mode){
    return pup_arm_op(level, PUP_OP_ENTERT_SOURCE_CHANGED, src, mode, 0);
}
static int g_unstick_pend = 0;  /* 解卡两拍确认: 第一拍只预约 */
static int pup_unstick(void);   /* 前向声明: 解卡的第二拍在 pup_poll 里落地 */
/* 每拍轮询回执。三种结局都要说清楚, 别只报"没反应"。 */
static void pup_poll(void){
    u32 ack = 0, xrc = 0, rc = 0, cmd = 0, tidp = 0, tidx = 0, npost = 0;
    unsigned char busy = 0;
    /* 解卡的第二拍在这里落地(pup_unstick 只预约, 见那里的说明)。
     * 放在 g_pup_wait 判断**之前** —— 卡住的时候 g_pup_wait 早就归零了。 */
    if(g_unstick_pend) pup_unstick();
    if(!g_pup_wait) return;
    g_pup_wait--;
    if(rd32(PUP_CTL + PUP_O_ACK, &ack) != 0) return;
    if(ack != g_pup_ack0){
        rd32(PUP_CTL + PUP_O_XRC,  &xrc);
        rd32(PUP_CTL + PUP_O_RC,   &rc);
        rd32(PUP_CTL + PUP_O_TIDP, &tidp);
        rd32(PUP_CTL + PUP_O_TIDX, &tidx);
        rd32(PUP_CTL + PUP_O_NPOST,&npost);
        rd8 (PUP_CTL + PUP_O_BUSY, &busy);
        plat_log("[傀儡] ✅ 落地  ACK="); p_logd((int)ack);
        plat_log(" RC="); p_logd((int)rc);
        plat_log(" XRC="); p_logd((int)xrc);
        plat_log(" 发帖线程="); p_logd((int)tidp);
        plat_log(" 落地线程="); p_logd((int)tidx);
        plat_log(" 入队数="); p_logd((int)npost);
        plat_log(" BUSY="); p_logd((int)busy);
        plat_log("\n        ");
        if((int)xrc == PUP_XRC_CALLED){
            plat_log("XRC=1  真调了白名单序号 "); p_logd(g_pup_op); plat_log("\n");
        }
        else if((int)xrc == PUP_XRC_LVL1)     plat_log("XRC=10 级1 空跑, 原厂零接触\n");
        else if((int)xrc == PUP_XRC_BADALIGN) plat_log("XRC=-2 OBJ 没对齐(门①挡下, 没解引用)\n");
        else if((int)xrc == PUP_XRC_BADRANGE) plat_log("XRC=-3 OBJ 不在区间(门②挡下, 没解引用)\n");
        else if((int)xrc == PUP_XRC_BADVPTR)  plat_log("XRC=-4 *(OBJ) 不是**这一行**要的 vtable(门③挡下)\n");
        else if((int)xrc == PUP_XRC_BADVPTR2) plat_log("XRC=-5 *(this) 不是这一行要的子对象 vtable(门④挡下)\n");
        else if((int)xrc == PUP_XRC_BADOP)    plat_log("XRC=-6 序号越界(exec 自己那道硬门)\n");
        else if((int)xrc == PUP_XRC_NOTARMED) plat_log("XRC=-7 ARM 里这一位没置(post 之后被清了?)\n");
        else if((int)xrc == PUP_XRC_BADFUNC)  plat_log("XRC=-8 行里 FUNC==0(兜底; 正常走不到)\n");
        else if((int)xrc == PUP_XRC_NOPROXY)
            plat_log("XRC=-9 B4: KeyInput 服务没起, acquire 返回 0 -> **一个字节都没发**(零行为)\n");
        else                                  plat_log("XRC 是没见过的值 -> 别继续, 先看信箱\n");
        if((int)tidx != g_row_tid[g_pup_op]){
            plat_log("        ⚠ 落地线程不是 "); p_logd(g_row_tid[g_pup_op]);
            plat_log(" (这一行期望的那条) —— 线程亲和性的前提不成立, 级 2 先别上\n");
        }
        g_pup_wait = 0;
        return;
    }
    if(!g_pup_wait){
        rd32(PUP_CTL + PUP_O_CMD, &cmd);
        rd32(PUP_CTL + PUP_O_RC,  &rc);
        rd8 (PUP_CTL + PUP_O_BUSY, &busy);
        plat_log("[傀儡] ⏱ 超时 2 秒没回执。CMD="); p_logh(cmd);
        plat_log(" RC="); p_logd((int)rc);
        plat_log(" BUSY="); p_logd((int)busy); plat_log("\n        ");
        if(cmd != 0 && rc == 0)
            plat_log("CMD 没被消费、RC 也没写 -> hook 根本没跑(池字没生效? 该快路径没被走到?)\n");
        else if((int)rc == PUP_RC_NOLOCK)
            plat_log("RC=2 没抢到锁, 这条出口**不清 BUSY**。\n"
                     "        先看**入队数 vs 回执数**: 相等才可以级 8 解卡;\n"
                     "        不相等 = 有事件真在飞, **绝不能解卡**(会摘断原厂事件链) -> 等, 或重启\n");
        else if((int)rc == PUP_RC_POSTFAIL)
            plat_log("RC=5 原厂拒绝入队(post 返回 0) -> 事件没进队列, 原厂零接触\n");
        else if(busy != 0)
            plat_log("已入队但 vt[4] 没落地: 事件没被派发(线程号/队列不对) -> 别再武装\n");
        else
            plat_log("CMD 消费了、BUSY 也清了, 但 ACK 没动 -> 对不上, 先跑级 0\n");
    }
}
/* 解卡: 只写 BUSY 一个字节。用于 RC=2 那条出口(没抢到锁, 不清 BUSY)。
 *
 * 🚨🚨 **必须先过 pup_inflight()**。这一句原来是无条件写的 —— 那样它就把
 *   pup_install() 的"BUSY 非零就拒绝"那道门整个废掉了, F3 从 cave 里被搬到了这里。
 *   区分两种 BUSY=1:
 *     · NPOST == ACK  -> 锁字被卡住但**没有**在飞的事件(cave 在 tas.b 和 post 之间
 *                        被打断, 或 studio 半途死了)。清它是安全的。
 *     · NPOST != ACK  -> **有事件真在飞**。这时候清 BUSY 再武装 = 摘链, 会饿死
 *                        AudioCtrlThread。正确动作是**等**, 等不到就重启, 不是解卡。 */
/* 🚨 **两拍确认**。inflight 还剩一个真窗口: post 已经返回成功(事件进队了),
 *   但 cave 里 `NPOST++` 那三条指令还没跑完。窗口内 NPOST==ACK、EV+8==0 —— 门看不出来。
 *   而发帖线程恰恰在 post 尾部走 sem_post 唤醒 AudioCtrlThread, **在这里被抢占是常态**。
 *   解法不用测那个窗口有多宽: 要求**连续两拍(相隔 25ms)都判定没有在飞**才真清。
 *   25ms 足够让发帖线程把 NPOST++ 跑完。 */
static int pup_unstick(void){
    if(pup_inflight()){                     /* 只读, 不需要 mbopen 的写句柄 */
        plat_log("[傀儡] ⛔ 有事件在飞, **拒绝解卡** —— 清 BUSY 会摘断原厂事件链。\n"
                 "        等它落地; 一直不落地就重启台架(信箱在 RAM, 重启即全零)。\n");
        g_unstick_pend = 0;
        return -1;
    }
    if(!g_unstick_pend){                    /* 第一拍: 只预约 */
        g_unstick_pend = 1;
        plat_log("[傀儡] 解卡已预约, 下一拍复核 入队数/回执数 后才执行\n");
        return 0;
    }
    g_unstick_pend = 0;                     /* 第二拍: 两次采样都干净, 才动手 */
    if(mbopen() < 0) return -1;
    if(mbwr8(PUP_CTL + PUP_O_BUSY, 0)) return -1;
    plat_log("[傀儡] BUSY 已清零(解卡; 连续两拍 入队数==回执数)\n");
    return 0;
}
/* 抹回全零 = cave 完全惰性。只在确认没有在飞的事件时才做。
 *
 * ⚠️ 更正(原注释的安全论证是错的): 先写 LEVEL=0 **并不能**防住"抹 vptr 时事件还挂在队上"。
 *   LEVEL 只关**我们的 hook**; 已经入队的假事件, 原厂 dispatcher 照样会对它跑
 *   `mov.l @r8,r1; mov.l @(16,r1),r1; jsr @r1` —— vptr 被抹成 0 就是跳 *(0+16)。
 *   真正起作用的是下面那四道检查(在飞 / CMD / BUSY / EV+8)。先关 LEVEL 仍然有意义
 *   (少一拍新投递), 但它不是防线。 */
static int pup_clear(void){
    u32 w; int i; unsigned char busy = 0;
    if(mbopen() < 0) return -1;
    if(mbwr8(PUP_CTL + PUP_O_LEVEL, 0)) return -1;      /* ① 先关门(减少新投递, 不是防线) */
    if(pup_inflight()){ plat_log("[傀儡] 有事件在飞, 只关了门, 不抹\n"); return -1; }
    if(rd32(PUP_CTL + PUP_O_CMD, &w) != 0 || w != 0){ plat_log("[傀儡] CMD 还没被消费, 只关了门, 不抹\n"); return -1; }
    if(rd8(PUP_CTL + PUP_O_BUSY, &busy) != 0 || busy != 0){ plat_log("[傀儡] BUSY 非零, 只关了门, 不抹(级 8 解卡)\n"); return -1; }
    if(rd32(PUP_EV + PUP_O_EV_NEXT, &w) != 0 || w != 0){ plat_log("[傀儡] 假事件还挂在队列上, 只关了门, 不抹\n"); return -1; }
    for(i = 0; i * 4 < PUP_CTL_SPAN + PUP_EV_SPAN; i++) if(mbwr(PUP_CTL + (u32)i*4, 0)) return -1;
    for(i = 0; i < PUP_VT_SLOTS; i++) if(mbwr(PUP_VT + (u32)i*4, 0)) return -1;
    plat_log("[傀儡] 信箱已抹零 -> cave 回到完全惰性\n");
    return 0;
}
/* 停 studio 时把惰性门关上就够了(LEVEL=0 -> hook 第一条指令就直通)。
 * 🚨 这里**不做**全抹零: 万一那一刻假事件正挂在原厂队列上, 抹了 vptr 会让 dispatch
 *   去调 *(0+16), 直接打死 PCM3Root。要全清走级 9(它有三道前置检查)。 */
void plat_puppet_park(void){
    unsigned char lv = 0;
    if(g_as_w < 0) return;                       /* 从来没写过信箱 -> 本来就是惰性 */
    if(rd8(PUP_CTL + PUP_O_LEVEL, &lv) == 0 && lv != 0){
        mbwr8(PUP_CTL + PUP_O_LEVEL, 0);
        plat_log("[傀儡] 退出: LEVEL 已归零(cave 惰性)\n");
    }
}
/* 级 0 诊断读 —— **一个字节都不写**, 台架接上后第一件事就跑它。
 * 顺便直接判 F2 那个"没收敛"的候选: *(0x0864b500) 到底是不是我们锚到的单例。
 * (KB 里说"垃圾 this 已排除"是证据倒置: ELF 静态值 0 对运行期取值零信息量,
 *  而且它落在 .as 采集空洞里, 四份快照都没采到 -> 离线永远判不了, 只能上机读。) */
static void pup_diag(void){
    u32 w = 0, obj;
    plat_log("── 傀儡诊断(纯读, 零风险) ──\n");
    plat_log("  构建标识 cksum="); p_logh(PUP_BUILD_CKSUM);
    plat_log("  (与刷写包 cksum 对得上才是同一版)\n");
    plat_log("  白名单 表版本="); p_logd(PUP_TABLE_REV);
    plat_log(" 行数="); p_logd(PUP_NROWS);
    plat_log(" 表@"); p_logh(PUP_TABLE); plat_log("\n");
    if(g_as < 0){ g_as = open_as(); plat_log("  PCM3Root as fd="); p_logd(g_as); plat_log("\n"); }
    if(g_as < 0){ plat_log("  ⚠ 读不到 PCM3Root 内存, 下面全免谈\n"); return; }
    plat_log("  池字 "); p_logh(PUP_POOL); plat_log(" = ");
    if(rd32(PUP_POOL, &w) == 0) p_logh(w); else { w = 0; plat_log("????????"); }
    plat_log(w == PUP_CAVE ? "  ⇒ cave 已生效 ✓\n" : "  ⇒ **不是我们的 cave**(没刷/刷的是别版) ✗\n");
    plat_log("  cave 首字 "); p_logh(PUP_CAVE); plat_log(" = ");
    if(rd32(PUP_CAVE, &w) == 0) p_logh(w); else plat_log("????????");
    plat_log("\n  尾跳目标(PDC 桩B) "); p_logh(PUP_CHAIN); plat_log(" = ");
    if(rd32(PUP_CHAIN, &w) == 0) p_logh(w); else plat_log("????????");
    plat_log("\n");
    if(!g_V) locate_v4();
    if(!g_TUN) locate_root2();          /* Tuner 单例: 序号 4..12 那些行要它 */
    obj = pup_obj();
    plat_log("  V="); p_logh(g_V);
    plat_log("  锚定 OBJ="); p_logh(obj);
    if(!obj) plat_log("  ⚠ 没锚到 -> 级 2 不许跑");
    plat_log("\n");
    /* ★ F2 判据: 运行期的 *(0x0864b500)。08-06 的崩因三个候选**一个都没收敛**,
     *   这一个字读下来就能砍掉/坐实其中一个。 */
    if(rd32(0x0864b500u, &w) == 0){
        plat_log("  *(0x0864b500)="); p_logh(w);
        if(!obj)          plat_log("  (OBJ 没锚到, 没法比)\n");
        else if(w == obj) plat_log("  ⇒ 与锚定 OBJ **相同** = 那个候选可以排除 ✓\n");
        else if(w == 0)   plat_log("  ⇒ **运行期就是 0** = 垃圾 this 坐实, 8-06 崩因找到 ★\n");
        else              plat_log("  ⇒ 非零但**不是**我们的单例 = 也是个雷, 记下来 ★\n");
    } else plat_log("  *(0x0864b500) 读不到\n");
    /* ★ 当前武装位图 —— 重启后必须是 0(只有序号 0 恒武装)。不是 0 就说明有人置过。 */
    if(rd32(PUP_CTL + PUP_O_ARM, &w) == 0){
        plat_log("  ARM="); p_logh(w);
        plat_log(w ? "  ⇒ 有序号被武装过(重启即清)\n" : "  ⇒ 只有序号 0 能跑(重启默认) ✓\n");
    }
    /* Tuner 单例(序号 4..12 那些行要它)。⚠ 与 Sound 一样是活体扫出来的。 */
    plat_log("  Tuner OBJ="); p_logh(g_TUN);
    if(!g_TUN) plat_log("  ⚠ 没锚到 -> Tuner 那些序号的级 2 不许跑");
    plat_log("\n");
    pup_dump("  信箱 ");
    plat_log("── 诊断完(全程零写入) ──\n");
}

/* 只读内存查看器 —— 整个阶梯都要用它验证(cave 装没装、池字指哪、函数首字节对不对)。
 * 用法(台架): echo 8074690 > /tmp/studio_peek     # 十六进制, 不带 0x
 *   -> 日志打出该地址起 8 个 u32(从 PCM3Root 的 /proc/as 只读)
 * 纯读, 零风险。地址读不到就说读不到, 不猜。 */
/* 串口驱动的投递入口: echo "<命令> ..." > /tmp/studio_post
 *     0                          诊断读(纯读, 零风险) —— 台架接上后**第一件事**
 *     1 26                       级1 空跑(序号 0): 链跑通但只回写线程索引(XRC=10, 原厂零接触)
 *     2 26                       级2 真切源(序号 0; 源号 = 原厂 slot: 11=FM 26=AUX 40=BT)
 *     7 <bits>                   ★ 写 ARM 武装位图(十进制)。重启即清。序号 0 恒武装。
 *     3 <序号> [a0] [a1] [a2]    ★ 级1 空跑指定序号 —— **每个新序号上机第一步必做**
 *     4 <序号> [a0] [a1] [a2]    ★ 级2 真调指定序号
 *     8                          解卡: BUSY 写 0(RC=2 那条出口不清 BUSY)
 *     9                          清零: 关惰性门 + 抹信箱
 *
 *   参数怎么落到原厂函数上(小端, 一格通吃 u8/u16/u32):
 *     普通行:  r5=&a0  r6=&a1  r7=&a2      例: 调频 `4 4 93900 1 0`(freq/steps/sid)
 *     B4 那行: r5=a0(=key|source<<16)  r6=a1(=status|slider<<16), 都是**值**
 *              例: ESCAPE 按下 `4 13 11 0 0`, 松开 `4 13 11 3 0`
 *   ⚠ 序号 0 那一行 a0 走 CMD ⇒ 不能是 0; 其余行 a0 走 ARG0 字段 ⇒ **允许 0**
 *     (requestScan 就必须传 0 才有效)。
 * ⚠️ **文件读发生在 studio 自己的进程/线程里**, 想读多少次都行 ——
 *   原厂那个时间敏感的线程每拍只读一个字节。这正是这套方案的关键: 把 I/O 挪到我们这边。 */
void plat_post_cmd(void){
    int fd; char b[72]; int n, i = 0, f[6] = {0,0,0,0,0,0}, k = 0, has = 0;
    u32 v = 0;
    pup_poll();                                  /* 每拍都要跑, 回执才收得到 */
    fd = open("/tmp/studio_post", O_RDONLY, 0);
    if(fd < 0) return;
    n = read(fd, b, 71); close(fd);
    { int z = open("/tmp/studio_post", O_WRONLY|O_TRUNC, 0644); if(z>=0) close(z); }
    if(n <= 0) return;
    b[n] = 0;
    for(i = 0; i <= n && k < 6; i++){
        if(b[i] >= '0' && b[i] <= '9'){ v = v*10u + (u32)(b[i]-'0'); has = 1; }
        else if(has){ f[k++] = (int)v; v = 0; has = 0; }
    }
    if(k < 1) return;
    switch(f[0]){
        case 0: pup_diag(); break;
        /* 旧两条一字不改: 序号 0 = entertSourceChanged, 源号就是 CMD */
        case PUP_LEVEL_DRYRUN:
        case PUP_LEVEL_REAL:  pup_arm(f[0], (u32)f[1], (u32)f[2]); break;
        /* ★ 加宽: 指定白名单序号。3=先空跑(必做), 4=真调 */
        case 3: pup_arm_op(PUP_LEVEL_DRYRUN, f[1], (u32)f[2], (u32)f[3], (u32)f[4]); break;
        case 4: pup_arm_op(PUP_LEVEL_REAL,   f[1], (u32)f[2], (u32)f[3], (u32)f[4]); break;
        case 7: pup_setarm((u32)f[1]); break;
        case 8: pup_unstick(); break;
        case 9: pup_clear(); break;
        case 6: plat_touchgate_probe(); break;   /* 触摸门纯读探测 */
        case 12: { int sp=0;   /* MME 只读探针: 读当前播放速率 */
            if(mme_get_speed(&sp)) plat_log("[MME] 读速率失败\n");
            else { plat_log("[MME] 当前速率="); p_logd(sp);
                   plat_log(sp==0 ? "  (0=暂停)\n" : (sp==1000 ? "  (1000=正常播放)\n" : "  ⚠ 没见过的值\n")); }
          } break;
        case 10: plat_ts_arm(); break;      /* 触摸门: 装 */
        case 11: plat_ts_disarm(); break;   /* 触摸门: 摘 */
        case 13: { int rnd = -1, rep = -1;  /* MME 只读探针: 随机 + 重复。**改之前先跑它记下原值** */
            if(mme_get_mode(0, &rnd)) plat_log("[MME] 读随机失败\n");
            else { plat_log("[MME] 随机="); p_logd(rnd);
                   plat_log("  (0=关 1=全部 2=专辑 3=文件夹 4=子文件夹)\n"); }
            if(mme_get_mode(1, &rep)) plat_log("[MME] 读重复失败\n");
            else { plat_log("[MME] 重复="); p_logd(rep);
                   plat_log("  (0=关 1=单曲 2=全部 3=文件夹 4=子文件夹)\n"); }
          } break;
        /* 写: `14 <0..4>` 随机 / `15 <0..4>` 重复。**唯一会改原厂持久设置的两条**, 值域自己钳。 */
        case 14: if(mme_set_mode(0, f[1])==0){ plat_log("[MME] 已设随机="); p_logd(f[1]); plat_log("\n"); } break;
        case 15: if(mme_set_mode(1, f[1])==0){ plat_log("[MME] 已设重复="); p_logd(f[1]); plat_log("\n"); } break;
        default:
            plat_log("[傀儡] 不认识的级别 "); p_logd(f[0]);
            plat_log("  (0=诊断 1=空跑序号0 2=真调序号0"
                     " 3=空跑<序号> 4=真调<序号> 7=写ARM 8=解卡 9=清零)\n");
    }
}

/* ============ 触摸门(方案 A)—— 本轮只做**纯读干跑**, 一个字节都不写 ============
 *
 * 原厂自己就有一条触摸 early-return, 我们只是把它拨到原厂支持的位置:
 *   0x085D20B4:  q = *(mapper+0x40);  r0 = 0x085D1EAA(q);  **xor #1,r0**
 *   0x085D1EAA:  S = *(q+0x28);  return (*(S+0x1a0)==2) ? *(S+0x9c) : 0
 * ⇒ 写 `*(S+0x9c) = 1` 触摸整帧丢掉, **硬键不受影响**(那是另一条路)。
 * 🚨 门是 `xor #1` **不是逻辑取反** —— 写 2 会变成 3(非零)照样放行。**只能写奇数, 规范写 1。**
 * 🚨 最大未知: S 有 20 个持有者, `+0x9c` 的语义未知, 其它读者**离线枚举不了**
 *   (0x9c 在代码段有 330 处常量加载, 还能用 `add #124` 拼)。**只能靠上机行为观测。**
 *   所以这一版**只读不写**: 把整条链解出来打日志, 人看过再决定要不要写。 */
#define TSQ_OFF    0x40u          /* mapper + 0x40 -> q */
#define TSQ_VPTR   0x091ba254u
#define TSS_OFF    0x28u          /* q + 0x28 -> S */
#define TSS_VPTR   0x0911c3acu    /* CSPHMISettingsProxy */
#define TSS_VALID  0x1a0u         /* == 2 门才生效 */
#define TSS_GATE   0x9cu          /* 写 1 = 丢触摸 */
void plat_touchgate_probe(void){
    u32 q = 0, qv = 0, S = 0, sv = 0, valid = 0, gate = 0;
    plat_log("── 触摸门探测(纯读, 零写入) ──\n");
    if(!g_km || g_rl < 0){ plat_log("  ⚠ mapper 没锚到 / 没有 PCM3Reload 句柄 -> TSX_E_NOMAPPER\n"); return; }
    plat_log("  mapper X="); p_logh(g_km); plat_log("\n");
    if(fd_rd(g_rl, g_km + TSQ_OFF, &q, 4) != 4){ plat_log("  ⚠ 读 X+0x40 失败\n"); return; }
    q = le32((const unsigned char*)&q);
    plat_log("  q = *(X+0x40) = "); p_logh(q);
    if(!q || fd_rd(g_rl, q, &qv, 4) != 4){ plat_log("   ⚠ 读不到 *q -> TSX_E_BADQ\n"); return; }
    qv = le32((const unsigned char*)&qv);
    plat_log("   *q="); p_logh(qv);
    plat_log(qv == TSQ_VPTR ? "  ✓\n" : "  ✗ 期望 0x091ba254 -> TSX_E_BADQ\n");
    if(qv != TSQ_VPTR) return;
    if(fd_rd(g_rl, q + TSS_OFF, &S, 4) != 4){ plat_log("  ⚠ 读 q+0x28 失败\n"); return; }
    S = le32((const unsigned char*)&S);
    plat_log("  S = *(q+0x28) = "); p_logh(S);
    if(!S || fd_rd(g_rl, S, &sv, 4) != 4){ plat_log("   ⚠ 读不到 *S -> TSX_E_BADS\n"); return; }
    sv = le32((const unsigned char*)&sv);
    plat_log("   *S="); p_logh(sv);
    plat_log(sv == TSS_VPTR ? "  ✓ CSPHMISettingsProxy\n" : "  ✗ 期望 0x0911c3ac -> TSX_E_BADS\n");
    if(sv != TSS_VPTR) return;
    if(fd_rd(g_rl, S + TSS_VALID, &valid, 4) == 4){
        valid = le32((const unsigned char*)&valid);
        plat_log("  *(S+0x1a0)="); p_logd((int)valid);
        plat_log(valid==2 ? "" : "  🚨 ≠2 ⇒ 此刻装门也会被静默旁路!");
        plat_log(valid == 2 ? "  ✓ 门生效\n" : "  ✗ 门本来就不生效 -> TSX_E_NOTVALID\n");
    }
    if(fd_rd(g_rl, S + TSS_GATE, &gate, 4) == 4){
        gate = le32((const unsigned char*)&gate);
        plat_log("  *(S+0x9c)="); p_logh(gate);
        plat_log(gate == 0 ? "  ✓ 原值为 0, 可以装(但本版不写)\n"
                           : "  🚨 原值非 0 = 原厂自己在用 -> TSX_E_ORIGNOT0, **不许装**\n");
    }
    plat_log("── 探测完(TSX_DRYRUN, 一个字节都没写) ──\n");
}
/* ---- 触摸门: 装 / 摘 ----
 * 🚨 三条铁律:
 *   ① 只写**一个 u32**, 而且写完立刻回读(QNX 跨进程写失败是**静默**的)
 *   ② 装之前原值必须是 0 —— 非 0 说明原厂自己在用这个状态, 立刻拒绝
 *   ③ 任何异常出口都要能把它写回去。studio 死了还装着 = **触摸永久失效**,
 *      所以装信号处理器, 用**预先算好的地址**直接写回(handler 里绝不扫描/绝不 malloc)。
 * 逃生阀: 硬键没被吞 ⇒ 用户按 SOURCE/MEDIA 原厂自己切页 ⇒ 我们看到非白名单页自动摘 + 让开。
 * 最后兜底: 改动是**纯 RAM**, 断电即回原厂。 */
static int g_rl_w   = -1;      /* PCM3Reload 的 /proc/as fd(可写) */
static u32 g_ts_S   = 0;       /* 解析好的 S(信号处理器要用, 不能现算) */
static u32 g_ts_orig= 0;
static int g_ts_armed = 0;     /* 实际状态: 门确实是我们按下去的 */
static int g_ts_want  = 0;     /* 意图: 我们**希望**门是按下的 */
int g_ts_armed_pub = 0;        /* set_cover 在前面, 要看这个 */
/* 🚨 为什么要拆成两个(复核 F3): 摘除写失败时 g_ts_armed 若仍是 1,
 *   下一拍 watch 读到 0 就把门**重新按住** —— 一次瞬时失败就变成"门被我们自己永久按住",
 *   而且再没人会调 disarm。所以 watch 只在 **want==1** 时才重写 1;
 *   want==0 而门还是奇数, 它要**继续尝试写回原值**。 */

static int rl_wr32(u32 va, u32 v){
    if(g_rl_w < 0) return -1;
    if(lseek(g_rl_w, (long)va, 0) != (long)va) return -1;
    return (write(g_rl_w, &v, 4) == 4) ? 0 : -1;
}
static int rl_rd32(u32 va, u32 *o){
    u32 w;
    if(g_rl < 0 || fd_rd(g_rl, va, &w, 4) != 4) return -1;
    *o = le32((const unsigned char*)&w); return 0;
}
/* 解析并校验整条链, 成功时把 S/orig 存起来。返回 0 = 可以用。 */
static int ts_resolve(void){
    u32 q, qv, S, sv, valid, gate;
    g_ts_S = 0;
    if(!g_km || g_rl < 0) return -2;                                  /* NOMAPPER */
    if(rl_rd32(g_km + TSQ_OFF, &q) || !q || rl_rd32(q, &qv) || qv != TSQ_VPTR) return -3;   /* BADQ */
    if(rl_rd32(q + TSS_OFF, &S) || !S || rl_rd32(S, &sv) || sv != TSS_VPTR) return -4;      /* BADS */
    if(rl_rd32(S + TSS_VALID, &valid) || valid != 2) return -5;       /* NOTVALID */
    if(rl_rd32(S + TSS_GATE, &gate)) return -4;
    g_ts_S = S; g_ts_orig = gate;
    return 0;
}
/* 信号处理器: 只做一件事 —— 把门写回去。绝不扫描、绝不分配。 */
/* 🚨 handler 里**先校验 vptr 再写**。SIGSEGV 本身就意味着状态可能已经烂了;
 *   若 S 已被释放/复用, 盲写就从"触摸没了"升级成"把原厂 HMI 写崩"。
 *   read/write/lseek 都是 async-signal-safe, 成本一次 syscall, 值。 */
static void ts_panic_restore(int sig){
    if(g_ts_armed && g_ts_S && g_rl_w >= 0){
        u32 vp = 0, z = g_ts_orig;
        if(lseek(g_rl_w, (long)g_ts_S, 0) == (long)g_ts_S &&
           read(g_rl_w, &vp, 4) == 4 && le32((const unsigned char*)&vp) == TSS_VPTR &&
           lseek(g_rl_w, (long)(g_ts_S + TSS_GATE), 0) == (long)(g_ts_S + TSS_GATE))
            write(g_rl_w, &z, 4);
    }
    (void)sig; exit(3);
}
/* 🚨 **不变式专用入口**: 确保门处于 want 指定的状态, 返回**执行后它到底装没装**。
 * 为什么要它(2026-08-14 踩过): plat_ts_arm() 成功返回 **1**、失败返回负数,
 * 而本项目大部分函数是 0=成功 —— 我按 `r == 0` 判成功, 于是门明明装上了却报"装不上",
 * 还继续重试。**混乱的返回约定就是坑, 别指望调用方记得住。**
 * 维护不变式的正确写法是: **动作之后验状态**, 不看返回码。 */
int plat_ts_ensure(int want){
    if(want && !g_ts_armed) plat_ts_arm();
    if(!want && g_ts_armed) plat_ts_disarm();
    return g_ts_armed;                      /* 唯一权威: 它现在到底装没装 */
}

int plat_ts_arm(void){
    u32 back = 0; int rc;
    if(g_ts_armed){ plat_log("[触摸门] 已经装着了\n"); return 1; }
    if((rc = ts_resolve()) != 0){ plat_log("[触摸门] ⚠ 链路校验失败 TSX_E="); p_logd(rc); plat_log("\n"); return rc; }
    if(g_ts_orig != 0){
        plat_log("[触摸门] 🚨 原值非 0 ("); p_logh(g_ts_orig);
        plat_log(") = 原厂自己在用 -> 拒绝装(TSX_E_ORIGNOT0)\n"); return -6;
    }
    if(g_ts_down){ plat_log("[触摸门] ⚠ 手指还按着 -> 拒绝装(TSX_E_MIDPRESS)\n"); return -9; }
    if(!g_kmP){ plat_log("[触摸门] ⚠ 触摸源没锚到 -> g_ts_down 恒 0, 上面那道守卫是空的 -> 拒绝装\n"); return -9; }
    if(g_rl_w < 0){ plat_log("[触摸门] ⚠ 没有 PCM3Reload 可写句柄(TSX_E_NOFD)\n"); return -1; }
    /* 🚨 门是 `xor #1`, **只能写奇数**。写 2 会变成 3(非零)= 照样放行。 */
    if(rl_wr32(g_ts_S + TSS_GATE, 1)){ plat_log("[触摸门] ⚠ 写失败\n"); return -7; }
    if(rl_rd32(g_ts_S + TSS_GATE, &back) || back != 1){
        plat_log("[触摸门] ⚠ 回读="); p_logh(back); plat_log(" 不是 1 -> TSX_E_WRITEFAIL\n"); return -7;
    }
    g_ts_armed = 1; g_ts_want = 1; g_ts_armed_pub = 1;
    signal(SIGSEGV, ts_panic_restore); signal(SIGBUS,  ts_panic_restore);
    signal(SIGILL,  ts_panic_restore); signal(SIGFPE,  ts_panic_restore);
    signal(SIGTERM, ts_panic_restore); signal(SIGINT,  ts_panic_restore);
    signal(SIGHUP,  ts_panic_restore); signal(SIGQUIT, ts_panic_restore);
    signal(SIGABRT, ts_panic_restore);
    /* 🚨 挂死**不产生任何信号** —— 而本项目最有前科的死法就是挂死
     *   (gf_layer_update 会 REPLY-block 把整个进程锁死, 08-05 踩过)。
     *   所以再加一道 alarm 看门狗: 主循环每拍 alarm(3) 续命, 真卡住 3 秒 -> SIGALRM -> 摘门退出。
     *   这个模式抄自隔壁 coexist_pop.c, 已在本机验过。 */
    signal(SIGALRM, ts_panic_restore); alarm(3);
    plat_log("[触摸门] ✅ 已装 TSX_ARMED  S="); p_logh(g_ts_S);
    plat_log("  原厂 HMI 收不到触摸了(硬键不受影响)\n");
    return 1;
}
int plat_ts_disarm(void){
    u32 back = 0, cur = 0;
    g_ts_want = 0;                     /* ★ 先落意图 —— 之后 watch 绝不会再把门按回去 */
    if(!g_ts_armed) return 2;
    if(g_ts_S && rl_rd32(g_ts_S + TSS_GATE, &cur) == 0 && cur != 1){
        plat_log("[触摸门] 🚨 当前值="); p_logh(cur);
        plat_log(" 不是我们写的 1 -> **不写回**(TSX_E_FOREIGN)\n");
        g_ts_armed = 0; g_ts_armed_pub = 0; return -11;
    }
    if(rl_wr32(g_ts_S + TSS_GATE, g_ts_orig) ||
       rl_rd32(g_ts_S + TSS_GATE, &back) || back != g_ts_orig){
        plat_log("[触摸门] 🚨🚨 摘除回读失败 TSX_E_RESTOREFAIL —— 触摸可能还是死的, 断电恢复\n");
        return -8;
    }
    g_ts_armed = 0; g_ts_armed_pub = 0; alarm(0);   /* 门摘了就撤看门狗 */
    plat_log("[触摸门] ✅ 已摘 TSX_DISARMED, 触摸还给原厂\n");
    return 2;
}
/* 每拍: armed 期间盯着它有没有被刷回去 / 有没有第三种值 */
void plat_ts_watch(void){
    u32 v = 0;
    u32 vp = 0;
    if(!g_ts_armed || !g_ts_S) return;
    if(rl_rd32(g_ts_S, &vp) || vp != TSS_VPTR){        /* 对象没了/被复用 -> 绝不再写 */
        plat_log("[触摸门] 🚨 *S 不再是 CSPHMISettingsProxy -> 放手, 不写\n");
        g_ts_armed = 0; g_ts_armed_pub = 0; alarm(0); return;
    }
    /* 🚨🚨 2026-08-13 定案的真因就在这里。门的判据是**两个字段**:
     *     0x085D1EAA:  return (*(S+0x1a0)==2) ? *(S+0x9c) : 0     ; 之后取反
     *   `0x1a0`(数据状态)一旦不再是 2, getter 直接返回 0, 取反 = **放行** ——
     *   而 `0x9c` 还老老实实是我们写的 1。
     *   ⇒ 只盯 0x9c 会得出"门保持得好好的"这个**假绿**, 而原厂其实已经在收触摸了。
     *   实测: 设置页 3475 装门有效(原厂无反应), 蓝牙页 375 装门无效(原厂切到 377),
     *        而两次 `*(S+0x9c)` 都稳稳是 1 —— 差别只可能在这个没人看的字段上。
     *   `ts_resolve()` 本来就验了 0x1a0==2(返回 -5), 但它**只在装门那一刻跑一次**。
     *   知识一直在代码里, 就是没接进看门狗。 */
    {   u32 valid = 0;
        if(!rl_rd32(g_ts_S + TSS_VALID, &valid) && valid != 2){
            plat_log("[触摸门] 🚨 *(S+0x1a0)="); p_logd((int)valid);
            plat_log(" ≠2 ⇒ **门被静默旁路**(0x9c 写了也没用), 写回 2\n");
            rl_wr32(g_ts_S + TSS_VALID, 2);
        } }
    if(rl_rd32(g_ts_S + TSS_GATE, &v)) return;
    if(!g_ts_want){                    /* 已经不想要了: 只负责把它还回去, 绝不再按下。
                                        * ⚠️ 摘门**只需要**把 0x9c 写回 0 —— 那时 getter 返回 0,
                                        *   取反=放行, 与 0x1a0 是几无关。所以不动 0x1a0, 少碰一个字段。 */
        if(v & 1u){ rl_wr32(g_ts_S + TSS_GATE, g_ts_orig);
                    plat_log("[触摸门] 摘除后发现门还是奇数 -> 再写回一次\n"); }
        else { g_ts_armed = 0; g_ts_armed_pub = 0; alarm(0); }
        return;
    }
    if(v == 1){ alarm(3); return; }                        /* 正常, 顺便续看门狗 */
    if(v == 0){ rl_wr32(g_ts_S + TSS_GATE, 1); alarm(3); return; }  /* 被服务端刷回, 重写 */
    /* 🚨 门是 `xor #1`: **奇数 ⇒ 触摸仍然是死的**, 这时候放手不管 = 门开着走人, 最坏结果。
     *   只有偶数才是"原厂放行", 才可以安全放手。 */
    if(v & 1u){
        plat_log("[触摸门] 🚨 读到奇数 "); p_logh(v);
        plat_log(" -> 触摸仍被堵, 继续写回原值\n");
        rl_wr32(g_ts_S + TSS_GATE, g_ts_orig); alarm(3); return;
    }
    plat_log("[触摸门] 🚨 读到偶数第三值 "); p_logh(v);
    plat_log(" = 原厂自己在用且已放行 -> 放手并撤层\n");
    g_ts_armed = 0; g_ts_armed_pub = 0; alarm(0); set_cover(0);
}

/* 读 PCM3Reload 的任意地址(plat_peek 读的是 PCM3Root)。echo <hex> > /tmp/studio_peek2 */
void plat_peek2(void){
    int fd = open("/tmp/studio_peek2", O_RDONLY, 0);
    char b[24]; int n, i; u32 va = 0, w;
    if(fd < 0) return;
    n = read(fd, b, 20); close(fd);
    { int z = open("/tmp/studio_peek2", O_WRONLY|O_TRUNC, 0644); if(z>=0) close(z); }
    if(n <= 0 || g_rl < 0) return;
    for(i = 0; i < n; i++){
        char c = b[i];
        if(c>='0'&&c<='9')      va = va*16u + (u32)(c-'0');
        else if(c>='a'&&c<='f') va = va*16u + (u32)(c-'a'+10);
        else if(c>='A'&&c<='F') va = va*16u + (u32)(c-'A'+10);
        else break;
    }
    if(!va) return;
    plat_log("[peek2/Reload] "); p_logh(va); plat_log(":");
    for(i = 0; i < 8; i++){
        if(fd_rd(g_rl, va + (u32)i*4, &w, 4) == 4){ plat_log(" "); p_logh(le32((const unsigned char*)&w)); }
        else plat_log(" ????????");
    }
    plat_log("\n");
}
void plat_peek(void){
    int fd = open("/tmp/studio_peek", O_RDONLY, 0);
    char b[24]; int n, i; u32 va = 0;
    if(fd < 0) return;
    n = read(fd, b, 20); close(fd);
    { int z = open("/tmp/studio_peek", O_WRONLY|O_TRUNC, 0644); if(z>=0) close(z); }
    if(n <= 0) return;
    for(i = 0; i < n; i++){
        char c = b[i];
        if(c>='0'&&c<='9')      va = va*16u + (u32)(c-'0');
        else if(c>='a'&&c<='f') va = va*16u + (u32)(c-'a'+10);
        else if(c>='A'&&c<='F') va = va*16u + (u32)(c-'A'+10);
        else break;
    }
    if(!va) return;
    plat_log("[peek] "); p_logh(va); plat_log(":");
    for(i = 0; i < 8; i++){
        u32 w = 0;
        if(rd32(va + (u32)i*4, &w) == 0){ plat_log(" "); p_logh(w); }
        else { plat_log(" ????????"); }
    }
    plat_log("\n");
}

/* ============ 收音机 / 点火: 与 V 同源的多条件锚定(2026-08-06, 6 份 dump 各命中 1 个) ============
 * ⚠️ 扫描下限沿用 HEAP_LO=0x0866e200。从 0x08600000 起扫会扫进 rodata, 单条件就会误命中 4 个。 */
#define TUN_VT1 0x085d69acu   /* u32@X        */
#define TUN_VT2 0x085d6aa8u   /* u32@(X+0x84) */
#define TUN_VT3 0x085d6a1cu   /* u32@(X+0x14) */
#define ON_VT1  0x085a2114u   /* u32@X         */
#define ON_VT2  0x085a22bcu   /* u32@(X+0x14)  */
#define ON_VT3  0x085a2488u   /* u32@(X+0x2ac) */
#define TUN_FM   0x3B4        /* u32 kHz, 87500..108000 步进 50 */
#define TUN_AM   0x3F4        /* u32 kHz, 531..1602 步进 9 */
#define TUN_PRE  0x2A4        /* 预设表: rec = TUN+0x2A4+0x18*i; +0x08 频率 +0x14 波段(1FM 2AM 0xFF空) */
#define ON_IGN   0xdb0        /* 点火状态; 原厂判据是 >1 (字符串 "mIgnitionStatus > 1") */

static u32 g_TUN = 0, g_ONOFF = 0;

/* 先校验上次命中的地址(3 次读), 过了就跳过全扫 —— 堆地址在 6 份 dump 里只在少数几个值之间跳。 */
static int check3(u32 X, u32 a, u32 o1, u32 b, u32 o2, u32 c){
    u32 w;
    if(!X) return 0;
    if(rd32(X, &w) || w != a) return 0;
    if(rd32(X+o1, &w) || w != b) return 0;
    if(rd32(X+o2, &w) || w != c) return 0;
    return 1;
}
/* 一次扫描同时找 TUN 和 ONOFF(别各扫一遍 3.75MB) */
static void locate_root2(void){
    u32 va; int nt = 0, no = 0;
    if(check3(g_TUN, TUN_VT1, 0x84, TUN_VT2, 0x14, TUN_VT3) &&
       check3(g_ONOFF, ON_VT1, 0x14, ON_VT2, 0x2ac, ON_VT3)) return;   /* 都还在, 不扫 */
    g_TUN = g_ONOFF = 0;
    for(va = HEAP_LO; va < HEAP_HI; va += SCAN_CHUNK - 8){
        int got = as_rd(va, g_scan, SCAN_CHUNK), j;
        if(got < 8) continue;
        for(j = 0; j + 3 < got; j += 4){
            u32 w = le32(g_scan + j), X = va + (u32)j;
            if(w == TUN_VT1){
                if(check3(X, TUN_VT1, 0x84, TUN_VT2, 0x14, TUN_VT3)){ if(!g_TUN) g_TUN = X; nt++; }
            } else if(w == ON_VT1){
                if(check3(X, ON_VT1, 0x14, ON_VT2, 0x2ac, ON_VT3)){ if(!g_ONOFF) g_ONOFF = X; no++; }
            }
        }
    }
    plat_log("调谐器/开关机锚定: TUN 命中"); p_logd(nt);
    plat_log(" 个 @"); p_logh(g_TUN);
    plat_log("  ONOFF 命中"); p_logd(no); plat_log(" 个 @"); p_logh(g_ONOFF); plat_log("\n");
    if(nt != 1) g_TUN = 0;            /* 命中数不是 1 就不用, 别猜 */
    if(no != 1) g_ONOFF = 0;
}

/* ============ 蓝牙曲目数据: PCM3Reload 的 MediaManager 单例 ============
 * 全局指针 + vptr 校验, 失败再堆扫兜底(命中数必须 ==1)。 */
#define MME_G     0x0948067cu     /* .data 里的单例指针 */
#define MME_VPTR  0x09120f44u
#define MME_LO    0x09566000u
#define MME_EMPTY 0x0947e860u     /* 全局空串单例 —— 指到它就是"没有" */
#define MME_S     0xA0            /* S = u32@(OBJ+0xA0) */
#define S_HEALTH1 0x248           /* ==2 */
#define S_HEALTH2 0x48            /* ==1 */
#define S_NPSRC   0x4c            /* 门 +0x24c==2; 4 = A2DP */
#define S_TRKSRC  0x50            /* 门 +0x250==2 */
#define S_PLAY    0x144           /* 门 +0x264==2 */
/* NowPlayingInfo 结构基址 = S+0x50, 四个 HB 串指针连续排列:
 *     +0x1C title  +0x20 artist  +0x24 album  +0x28 genre
 * ⇒ S+0x6c / 0x70 / 0x74 / 0x78。
 * 前两个是**台架实证能用的**(曲名/艺人一直读得对), 所以这个布局不是纸上推的 ——
 * 它预测出的 0x6c/0x70 正好等于已知值, 后两个是同一结构的相邻成员。
 * 交叉证据: PCM3Reload 里有 `mMMEProxy.getNowPlayingInfo(state).mediastoreName = %s`
 * (@0x091ad1f0)和 `folderid<%llu > foldername<%s> mediastoreName<%s>`(@0x0911d78c),
 * 证明确实存在 NowPlayingInfo 这个结构且成员是这一套。
 * ⚠️ 2026-08-13 首次读时四个指针**全是空串单例 0x0947e860** —— 那是因为当时没在放音,
 *    不是偏移错。要验真值必须在放音状态下读。 */
#define S_TITLE   0x6c
#define S_ARTIST  0x70
#define S_ALBUM   0x74
#define S_GENRE   0x78
#define S_CUR     0x154           /* **秒** */
#define S_TOT     0x158           /* **秒** */
#define SRC_A2DP  4

static u32 g_mme = 0, g_mmeS = 0;

/* MME 的 PlayState 枚举跟我们的 PLAY_* **数值刚好错开** ——
 * 直接赋值会把"正在播放"显示成"停止"。必须显式转换。 */
static int mme_to_play(u32 v){
    switch(v){
        case 0: return PLAY_PLAYING;
        case 1: return PLAY_PAUSED;
        case 2: return PLAY_STOP;
        case 3: case 4: case 5: case 6: return PLAY_PLAYING;   /* 快进/快退/慢放 */
        default: return PLAY_STOP;
    }
}
/* HBString: p=u32@(S+off); 字节长 u32@(p+8); 文本在 p+17, UTF-8, NUL 结尾 */
static void read_hbstr(u32 sbase, int off, char *out, int cap){
    u32 p = 0, len = 0; int n;
    out[0] = 0;
    if(fd_rd(g_rl, sbase + (u32)off, &p, 4) != 4) return;
    p = le32((const unsigned char*)&p);
    if(!p || p == MME_EMPTY) return;                 /* 空串单例 = 没有 */
    if(fd_rd(g_rl, p + 8, &len, 4) != 4) return;
    len = le32((const unsigned char*)&len);
    if(len == 0 || len > 400u) return;
    n = (int)len; if(n > cap-1) n = cap-1;
    if(fd_rd(g_rl, p + 17, out, n) != n){ out[0]=0; return; }
    out[n] = 0;
}
static void locate_mme(void){
    u32 v = 0, vp = 0;
    g_mme = g_mmeS = 0;
    if(fd_rd(g_rl, MME_G, &v, 4) == 4){
        v = le32((const unsigned char*)&v);
        if(v >= MME_LO && fd_rd(g_rl, v, &vp, 4) == 4 && le32((const unsigned char*)&vp) == MME_VPTR)
            g_mme = v;
    }
    if(!g_mme){                                       /* 全局指针不可用 -> 堆扫兜底 */
        u32 va; int hits = 0;
        for(va = MME_LO; va < MM_HI; va += SCAN_CHUNK - 8){
            int got = fd_rd(g_rl, va, g_scan, SCAN_CHUNK), j;
            if(got < 8) continue;
            for(j = 0; j + 3 < got; j += 4)
                if(le32(g_scan+j) == MME_VPTR){ if(!g_mme) g_mme = va + (u32)j; hits++; }
        }
        if(hits != 1){ g_mme = 0; }
        plat_log("媒体管理器: 堆扫命中 "); p_logd(hits); plat_log(" 个\n");
    }
    if(g_mme){
        u32 sv = 0, h1 = 0, h2 = 0;
        if(fd_rd(g_rl, g_mme + MME_S, &sv, 4) == 4){
            sv = le32((const unsigned char*)&sv);
            if(sv && fd_rd(g_rl, sv + S_HEALTH1, &h1, 4) == 4 && fd_rd(g_rl, sv + S_HEALTH2, &h2, 4) == 4
               && le32((const unsigned char*)&h1) == 2 && le32((const unsigned char*)&h2) == 1)
                g_mmeS = sv;
        }
        plat_log("媒体管理器: OBJ="); p_logh(g_mme); plat_log(" S="); p_logh(g_mmeS); plat_log("\n");
    }
}

void plat_read_state(PcmState *st){
    static PcmState s;
    static int inited = 0;
    if(!inited){
        int i;
        for(i=0;i<(int)sizeof s;i++) ((char*)&s)[i] = 0;
        /* 🚨 一律从"未知"起步, **不放假占位**。真数据接上前页面就该少显示一个元素,
         *   而不是显示"蓝牙音频""未连接"这种编出来的字(本项目铁律: 不许假渲染)。 */
        s.volume = -1; s.source = SRC_NONE; s.play_state = PLAY_STOP;
        s.pos_ms = 0; s.dur_ms = 0; s.u_hour = -1; s.u_minute = -1;
        s.freq_khz = -1; s.u_freq_am_khz = -1; s.muted = -1; s.ignition = -1;
        s.title[0] = 0; s.artist[0] = 0; s.album[0] = 0; s.genre[0] = 0; s.device[0] = 0;
        if(g_as < 0){ g_as = open_as(); plat_log("PCM3Root as fd="); p_logd(g_as); plat_log("\n"); }
        if(g_as >= 0) locate_v4();
        /* 第二个 fd: PCM3Reload(当前页 id 在它里面)。pid 由 goprobe/gostudio 写进 /tmp/rlpid,
         * ⚠️ 用 pidin 现查, 别写死 —— 写死的 PID 变了就静默读到别的进程。 */
        if(g_rl_w < 0) g_rl_w = open_as_pidfile_rw("/tmp/rlpid");
        if(g_rl < 0){ g_rl = open_as_pidfile("/tmp/rlpid");
            plat_log("PCM3Reload as fd="); p_logd(g_rl); plat_log("\n"); }
        if(g_rl >= 0){ locate_menumgr(); locate_keymapper(); }
        /* 🔬 只读验证一个**还没收敛**的假设(F2):
         *   0x0864b500 是不是 == CPSoundPresCtrl 单例 == 我们锚到的 OBJ(V-0x218)?
         *   ⚠️ KB 里"垃圾 this 已排除"是证据倒置 —— ELF 静态值 0 对运行期取值零信息量,
         *      而且它落在 .as 采集空洞 [0x08643000,0x08650000) 里, 四份快照都没采到,
         *      **离线永远判不了**。只能上机读这一个字。开机打一次, 想再看走级 0 诊断。
         *   纯读一个字, 零风险。 */
        if(g_V){ u32 sing = 0;
            if(rd32(0x0864b500u, &sing) == 0){
                plat_log("[验] 0x0864b500="); p_logh(sing);
                plat_log(" 我们的OBJ="); p_logh(g_V - 0x218u);
                plat_log(sing == g_V - 0x218u ? "  ⇒ 同一对象 ✓\n" : "  ⇒ 不同 ✗\n");
            } }
        inited = 1;
    }
    /* 音量(已验证的活链) */
    if(g_P){
        u32 ok = 0; u8_ v = 0;
        if(rd32(g_P+0xc8,&ok)==0 && ok==2 && rd8(g_P+0x7c,&v)==0 && v<=40){
            s.volume = (int)v; g_v4_fail = 0;
        } else {
            /* 🚨 校验失败**绝不能每拍重扫** —— locate_v4 一次要读 3.75MB(58 次 64KB procfs 读
             *   + 近百万次逐字节拼 u32)。主循环从 100ms 放到 25ms 之后, 这个地雷伤害翻了 4 倍。
             *   节流: 最快 2 秒一次, 连续失败 10 次就彻底停扫(堆布局是真变了, 白扫无意义)。 */
            unsigned now = plat_now_ms();
            if(g_v4_fail < 10 && now - g_v4_last >= 2000){
                g_v4_last = now; g_v4_fail++;
                locate_v4();
                if(g_v4_fail >= 10 && !g_P) plat_log("V4: 连续10次定位失败, 停止重扫\n");
            }
        }
    }
    /* 当前音源: OBJ = V - 0x218(离线在真车 dump 上复验过, 不用再扫一次堆) */
    if(g_V){
        u32 obj = g_V - 0x218u, slot = 0, app = 0;
        if(rd32(obj + SND_OFF_SLOT, &slot) == 0 && rd32(obj + SND_OFF_APP, &app) == 0){
            s_slot = (int)slot; s_app = (int)app;
        }
    }
    /* ---- 收音机 / 点火 / 静音 / 时间 ----
     * 🚨 全部约定 **-1 = 未知**, 场景遇到 -1 一律不画该元素(不许假渲染)。 */
    if(g_as >= 0){
        static unsigned t_root2 = 0;
        unsigned now2 = plat_now_ms();
        if((!g_TUN || !g_ONOFF) && now2 - t_root2 >= 2000){ t_root2 = now2; locate_root2(); }
        else if(g_TUN || g_ONOFF) locate_root2();          /* 只做 3 次校验读, 过了立刻返回 */
    }
    s.freq_khz = -1; s.u_freq_am_khz = -1;
    if(g_TUN){
        u32 f;
        /* 合法性门就是最好的校验: 频率必须落在波段内且步进对得上 */
        if(rd32(g_TUN+TUN_FM,&f)==0 && f>=87500 && f<=108000 && ((f-87500)%50)==0) s.freq_khz = (int)f;
        if(rd32(g_TUN+TUN_AM,&f)==0 && f>=531   && f<=1602   && ((f-531)%9)==0)    s.u_freq_am_khz = (int)f;
        { int i;
          for(i=0;i<9;i++){
              u32 fr=0, bd=0, rec = g_TUN + TUN_PRE + 0x18u*(u32)i;
              s.preset_freq[i] = 0; s.preset_band[i] = 0;
              if(rd32(rec+0x08,&fr)==0 && rd32(rec+0x14,&bd)==0){
                  if(bd==1 && fr>=87500 && fr<=108000){ s.preset_freq[i]=(int)fr; s.preset_band[i]=1; }
                  else if(bd==2 && fr>=531 && fr<=1602){ s.preset_freq[i]=(int)fr; s.preset_band[i]=2; }
              }
          } }
    }
    s.ignition = -1;
    if(g_ONOFF){ u32 ig; if(rd32(g_ONOFF+ON_IGN,&ig)==0) s.ignition = (ig>1); }
    /* 静音: 复用已锚到的 V, 零新扫描。⚠️ 门是 ==2 不是 !=0; 值是 u32 不是字节。
     * ⚠️⚠️ V+0x7c 是**环绕声**不是音量 —— 音量是 u8@(P+0x7c), P=u32@(V+0x168), 两个不同对象。 */
    s.muted = -1;
    if(g_V){ u32 g_, m_;
        if(rd32(g_V+0xa0,&g_)==0 && g_==2 && rd32(g_V+0x64,&m_)==0) s.muted = (m_==1); }
    /* 时间: 车钟存的就是**本地墙钟当 epoch**(2026-08-06 用真车日志时间戳反推确认)。
     * ⚠️ 别用 localtime/mktime —— 再套一次时区就双重偏移了。整数除法对时区免疫。 */
    { u32 ts[2]={0,0};
      if(clock_gettime(0, ts)==0 && ts[0] > 1400000000u){
          s.u_hour   = (int)((ts[0]/3600u)%24u);
          s.u_minute = (int)((ts[0]/60u)%60u);
      } else { s.u_hour = -1; s.u_minute = -1; } }

    /* 当前页 id: 原厂替我们解释了用户输入, 我们只读结果 */
    { int pid_ = read_page_id();
      if(pid_ == 0xFFFE) { /* 换页过渡哨兵, 不是真实页 —— 忽略, 保持上一个稳定值 */ }
      else if(pid_ >= 0) s_page = pid_;
      else if(g_rl >= 0 && g_mm_fail < 10){        /* 节流重锚, 别每拍扫 16MB */
          unsigned now = plat_now_ms();
          if(now - g_mm_last >= 3000){ g_mm_last = now; g_mm_fail++; locate_menumgr(); }
      } }
    /* 每拍把三个原始值报一次变化 —— 台架标定时靠它把数字和真实页面/音源对上 */
    if(s_page != s_page_last || s_slot != s_slot_last || s_app != s_app_last){
        plat_log("[原厂状态] 页id="); p_logd(s_page);
        plat_log(" 源slot="); p_logd(s_slot);
        plat_log(" 源app="); p_logd(s_app); plat_log("\n");
        s_page_last = s_page; s_slot_last = s_slot; s_app_last = s_app;
    }
    s.stock_page = s_page; s.stock_src_slot = s_slot; s.stock_src_app = s_app;
    /* ============ 原厂状态 -> 我们的状态(2026-08-05 台架逐条实证) ============
     * 用户按原厂的键切源/切页, 原厂照常解释并改自己的状态, 我们只读地镜像。
     * 下面每一个数字都是**台架上按一次对一次**得来的, 不是推测:
     *   源(slot, app):  FM=(11,1)  AUX=(26,6)  蓝牙=(40,7)
     *   页 id:          855=FM页   375=蓝牙播放页   387=切到AUX/蓝牙后、进播放页前那一页
     *                   65534(0xFFFE)=换页过渡哨兵, **不是真实页, 必须忽略**
     * ⚠️ app 是比 slot 更稳的判据(slot 起始还出现过 13, app 恒为 1)。 */
    switch(s_app){
        case 1:  s.source = SRC_FM;  break;
        case 6:  s.source = SRC_AUX; break;
        case 7:  s.source = SRC_BT;  break;
        default: break;                        /* 没见过的值就保持原样, 别乱跳 */
    }
    /* ---- 蓝牙曲目 ----
     * 🚨 必须**先过音源门**。不过门的话, 听 FM 时会把陈旧的蓝牙数据镜像成"已暂停、无曲目"
     *   (真车 FM 那两份 dump 读出来就是 STOPPED + 空串)。 */
    if(g_rl >= 0 && !g_mmeS){
        static unsigned t_mme = 0; unsigned now3 = plat_now_ms();
        if(now3 - t_mme >= 3000){ t_mme = now3; locate_mme(); }
    }
    if(g_mmeS){
        u32 g1=0, np=0, g2=0, tsrc=0;
        int a2dp = 0;
        if(fd_rd(g_rl,g_mmeS+0x24c,&g1,4)==4 && le32((const unsigned char*)&g1)==2 &&
           fd_rd(g_rl,g_mmeS+S_NPSRC,&np,4)==4 && le32((const unsigned char*)&np)==SRC_A2DP)
            a2dp = 1;
        if(a2dp && fd_rd(g_rl,g_mmeS+0x250,&g2,4)==4 && le32((const unsigned char*)&g2)==2 &&
           fd_rd(g_rl,g_mmeS+S_TRKSRC,&tsrc,4)==4 && le32((const unsigned char*)&tsrc)==SRC_A2DP){
            /* 曲目文本: 只在变了才重读(每拍读字符串要多十几次 lseek+read) */
            static u32 last_tok = 0xffffffffu;
            u32 tok = 0;
            if(fd_rd(g_rl,g_mmeS+0x54,&tok,4)==4) tok = le32((const unsigned char*)&tok);
            if(tok != last_tok){
                last_tok = tok;
                g_meta_dirty = 1;
            }
            /* 🚨🚨 2026-08-13 实测: **换曲时 S+0x54 根本不变**(恒 3)——
             *   拿它当"曲目变了"的闸门, 结果是首次读到什么就永远显示什么, 手机换歌页面不刷新。
             *   (我当天早些时候用 peek2 就看到了"令牌没变、字符串指针变了", 却没把它和这个闸门连起来。)
             *   真正会变的是**四个 HB 串指针本身** —— 换曲时原厂重新分配字符串, 指针必变。
             *   它们在 S+0x6c..0x7b 连续 16 字节, 一次读完当指纹, 比读字符串便宜得多。 */
            {
                unsigned char fp[16]; int k;
                if(fd_rd(g_rl, g_mmeS + S_TITLE, fp, 16) == 16){
                    for(k = 0; k < 16; k++) if(fp[k] != g_meta_fp[k]) break;
                    if(k < 16){ for(k = 0; k < 16; k++) g_meta_fp[k] = fp[k]; g_meta_dirty = 1; }
                }
            }
            if(g_meta_dirty){
                g_meta_dirty = 0;
                read_hbstr(g_mmeS, S_TITLE,  s.title,  (int)sizeof s.title);
                read_hbstr(g_mmeS, S_ARTIST, s.artist, (int)sizeof s.artist);
                read_hbstr(g_mmeS, S_ALBUM,  s.album,  (int)sizeof s.album);
                read_hbstr(g_mmeS, S_GENRE,  s.genre,  (int)sizeof s.genre);
                /* 一次性把四个都打出来 —— 这就是 S_ALBUM/S_GENRE 的上机验证 */
                plat_log("[元数据] 曲名="); plat_log(s.title[0]?s.title:"(空)");
                plat_log(" 艺人=");        plat_log(s.artist[0]?s.artist:"(空)");
                plat_log(" 专辑=");        plat_log(s.album[0]?s.album:"(空)");
                plat_log(" 流派=");        plat_log(s.genre[0]?s.genre:"(空)");
                plat_log("\n");
            }
            { u32 gp=0, pv=0;
              if(fd_rd(g_rl,g_mmeS+0x264,&gp,4)==4 && le32((const unsigned char*)&gp)==2 &&
                 fd_rd(g_rl,g_mmeS+S_PLAY,&pv,4)==4)
                  s.play_state = mme_to_play(le32((const unsigned char*)&pv)); }
            { u32 gt=0, cur=0, tot=0;
              if(fd_rd(g_rl,g_mmeS+0x26c,&gt,4)==4 && le32((const unsigned char*)&gt)==2 &&
                 fd_rd(g_rl,g_mmeS+S_CUR,&cur,4)==4 && fd_rd(g_rl,g_mmeS+S_TOT,&tot,4)==4){
                  /* ⚠️ MME 给的是**秒**, PcmState 要毫秒 */
                  s.pos_ms = (int)le32((const unsigned char*)&cur) * 1000;
                  s.dur_ms = (int)le32((const unsigned char*)&tot) * 1000;
              } }
            /* 随机 / 重复 —— 走 MME 读(subtype 14/15, 已实证)。
             * 🚨 **必须节流**: 主循环 25ms 一拍, 不限速就是每秒 80 次 MsgSend 打服务端。
             *   2 秒一次足够 —— 这两个值只在用户或手机改它时才变。
             * 读失败保持 -1(未知)⇒ UI 画灰的, 而不是画成"关"。 */
            {
                static unsigned last_mode = 0;
                unsigned now = plat_now_ms();
                /* 首次连上就订阅一次(失败也不重试 —— 退回纯轮询即可) */
                { static int tried = 0; if(!tried){ tried = 1; mme_reg_events(1); } }
                mme_drain_events();       /* 每拍排空到空 */
                /* 订阅成功后轮询降为**兜底**(10 秒): 事件万一静默死掉, 状态还能自愈。
                 * 没订上就保持 2 秒 —— 那时轮询是唯一的路。 */
                if(now - last_mode >= (g_ev_on ? 10000u : 2000u) || last_mode == 0){
                    int v;
                    last_mode = now ? now : 1u;
                    if(mme_get_mode(0, &v) == 0) g_shuffle = v;
                    if(mme_get_mode(1, &v) == 0) g_repeat  = v;
                }
                s.u_shuffle = g_shuffle; s.u_repeat = g_repeat;
            }
        } else {
            /* 不是蓝牙在放 -> 清掉, 别把陈旧数据当真 */
            s.title[0] = 0; s.artist[0] = 0; s.album[0] = 0; s.genre[0] = 0;
            s.pos_ms = 0; s.dur_ms = 0; s.play_state = PLAY_STOP;
            s.u_trk_cur = 0; s.u_trk_total = 0; s.u_battery = BATT_NONE;
            s.u_shuffle = -1; s.u_repeat = -1;
        }
    }
    /* 设备名: 读 /fs/avrcp0 的 info.xml(见 read_bt_device_name 上面的说明)。
     * 🚨 **必须限速** —— 这是一次真的文件 open+read, 主循环 25ms 一拍不限速就是每秒 40 次
     *   打 io-fs-media, 而那个驱动今天已经被目录遍历崩过一次。名字只在连/断时变, 5 秒一次足够。
     * 读不到就保持空 ⇒ UI 退回显示"蓝牙", 不编。 */
    {
        static char  dev_cache[48] = {0};
        static unsigned dev_last = 0;
        unsigned now = plat_now_ms();
        if(dev_last == 0 || now - dev_last >= 5000u){
            dev_last = now ? now : 1u;
            read_bt_device_name(dev_cache, (int)sizeof dev_cache);
            plat_log("[蓝牙设备] "); plat_log(dev_cache[0] ? dev_cache : "(读不到)"); plat_log("\n");
        }
        { int k; for(k = 0; k < (int)sizeof s.device - 1 && dev_cache[k]; k++) s.device[k] = dev_cache[k];
          s.device[k] = 0; }
    }
    *st = s;
}

/* ================= 输入 / 命令: 待打通 ================= */
int plat_poll_event(PcmEvent *ev){
    /* 真实触摸优先 —— 只读镜像, 不抢原厂事件流 */
    if(read_touch(ev)) return 1;
    /* ⏳ FPGA/IPC 输入还没打通(最大技术风险, IPC 读挂死过车+台架, 必须找只读 tap)。
     *    在此之前系统只显示不交互 —— 场景代码已经写好, 通了直接就能用。
     *    调试用: 串口写 /tmp/studio_ev 一行 "type which arg x y" 就能注入一个事件。 */
    int fd = open("/tmp/studio_ev", O_RDONLY, 0);
    char b[64]; int n, i=0, f[5]={0,0,0,0,0}, k=0, sign=1, v=0, has=0;
    if(fd < 0) return 0;
    n = read(fd,b,63); close(fd);
    { int z = open("/tmp/studio_ev", O_WRONLY|O_TRUNC, 0644); if(z>=0) close(z); }
    if(n <= 0) return 0;
    b[n] = 0;
    for(i=0; i<=n && k<5; i++){
        if(b[i]=='-' && !has){ sign = -1; }
        else if(b[i]>='0' && b[i]<='9'){ v = v*10 + (b[i]-'0'); has = 1; }
        else if(has){ f[k++] = v*sign; v=0; sign=1; has=0; }
    }
    if(k == 0) return 0;
    ev->type=f[0]; ev->which=f[1]; ev->arg=f[2]; ev->x=f[3]; ev->y=f[4];
    return ev->type != EV_NONE;
}

/* 按需武装: 只武装 **plat_command 真会发的那几行**, 不是整个 VERIFIED 集。
 * 重启后 ARM 在 RAM 里被清零, 所以这里要能自愈。
 * ⚠️ 武装本身**不会让 cave 做任何事** —— cave 只在 CMD 非零时才动。所以这一步是安全的。
 * ⚠️ 序号 13(B4 合成硬键)**不在这里**: 2026-08-12 上机崩过 PCM3Root, 没查清之前不碰。 */
#define PUP_STUDIO_ARM (PUP_ARM_BIT(PUP_OP_VOL_UP) | PUP_ARM_BIT(PUP_OP_VOL_DOWN) \
                      | PUP_ARM_BIT(PUP_OP_TUNER_FREQUENCY))
static int pup_ensure_armed(int op){
    u32 arm = 0;
    if(op == PUP_OP_ENTERT_SOURCE_CHANGED) return 0;      /* 序号 0 恒武装 */
    if(rd32(PUP_CTL + PUP_O_ARM, &arm) != 0) return -1;
    if(arm & PUP_ARM_BIT(op)) return 0;
    if(!(PUP_STUDIO_ARM & PUP_ARM_BIT(op))){
        plat_log("[傀儡] ⚠ 序号 "); p_logd(op);
        plat_log(" 不在 studio 允许自动武装的集合里 -> 拒绝(要用请手动 `7 <bits>`)\n");
        return -1;
    }
    return pup_setarm(arm | PUP_ARM_BIT(op));
}

/* 场景发出来的命令 -> 傀儡通道。
 *
 * 🚨 **拿不到的能力一律返回 -1 并说清为什么, 绝不假装成功** —— 场景据此决定画不画那个控件。
 *   play/pause/next/prev 住在 **PCM3Reload**(CPMMEPresCtrl), 傀儡 cave 在 PCM3Root 里, 够不着。
 *   (硬键 SKIP_LEFT/RIGHT 走白名单序号 13 能绕过去, 但那一行 2026-08-12 上机崩过 PCM3Root,
 *    在没查清之前**不接**。)
 *
 * 音量: 原厂只给"加/减 N 步", 没有"设成 N"。所以按当前值算差值, 再挑 VOL_UP / VOL_DOWN。
 *   步数是 u8, 原厂自己会钳位("steps to decrease => desired/allowed")。 */
/* ============ MME 客户端 —— 蓝牙/媒体播放控制的**直连通道** ============
 *
 * 2026-08-13: 线格式从**本地固件包里 not-stripped 的 mmecli** 完全离线反出来, 台架第一枪就通
 * (GETCLIENTCOUNT 返回 8 = 原厂 HMI 和我们并存, 原厂毫发无伤)。
 *
 * 24 字节 _IO_MSG 头(逐字节抄 mme_getclientcount @0x0804dac0):
 *     off0 u16 = 0x0113   _IO_MSG
 *     off2 u16 = 24       combine_len  ★ **照抄, 别现算** —— 它 != sbytes
 *     off4 u16 = 19       mgrid = _IOMGR_MEDIA
 *     off6 u16 = subtype  命令号
 *     off8     = body(我们用到的全是整数, **一个指针都没有**)
 *
 * 🚨 三条铁律(每条都有离线逐指令证据, 别为了省事破例):
 *  ① **只 open 一次**。一次 open = 服务端 calloc 1720B OCB + 128B 通知记录
 *     + **一条新的 qdb(SQLite)连接** —— 贵。绝不能每次按键 open/close。
 *  ② **绝不注册事件**。ntfy_allocateClient 把四个注册槽清零, 事件扇出对 NULL 槽
 *     **直接跳过**(不 calloc、不入队)⇒ 只 open 不注册 = **可证明的惰性**, 不可能积压。
 *  ③ **subtype 必须是编译期常量**。真危险不是"崩"(越界读在数学上不可能 —— 接收缓冲 8000B,
 *     handler 最远只读到 msg+23), 而是**落到另一条合法命令上**:
 *     24=SHUTDOWN(和 next/prev/stop **同形状**!) 27/28=TRKSESSION 89=DELETE_MEDIASTORES …
 *
 * 超时: mmecli 的 setup_timer 默认是 0(不超时) —— 我们**不抄这个默认值**。
 *   studio 是单线程主循环, MsgSend 挂住就整个界面死。所以每次发之前设 2 秒超时。
 *   (flags=0x50 = SEND|REPLY 抄自 mmecli; SIGEV_UNBLOCK=5 来自 QNX 头的枚举顺序;
 *    调用形状来自官方示例 references/himmele-qnx-examples/.../TimerTimeout.c) */
#define MME_DEV      "/dev/mme/default"
#define MME_IO_MSG   0x0113
#define MME_MGRID    19
/* —— 白名单: 只有这几个常量允许出现在 subtype 上 —— */
#define MME_STOP        2
#define MME_SET_SPEED   5     /* body int32: **0=暂停 1000=正常播放**(端到端反自 mmecli 的 pause/resume) */
#define MME_GET_SPEED   6
#define MME_NEXT       10
#define MME_PREV       11
#define MME_SETRANDOM  12     /* 0..4, 服务端**不做上界检查**, 我们自己 clamp */
#define MME_SETREPEAT  13
/* 读回当前值 —— 逐指令反自 mmecli mme_getrandom@0x0804e020 / mme_getrepeat@0x0804e120:
 *   clen=24 sbytes=24 **rbytes=16**, 结果在 reply[0](函数尾 `mov.l @r14,r0`)。
 * 有它才能"先读原值 → 再改 → 出问题能还原" —— 这组是唯一会改原厂持久设置的命令。 */
#define MME_GETRANDOM  14
#define MME_GETREPEAT  15
#define MME_GETCLIENTS 16
/* ---- 事件订阅(2026-08-13 解除红线) ----
 * KB 里原来写着"🚨绝不注册事件"。重新评估后**改为可做**, 依据是 mme-becker 里四条逐指令事实:
 *   ① 注册写的是**我们自己的** ntfy client(`*(ocb+28)`, 每次 open 各一份);
 *      原厂那份在 `*(ocb+24)`(共享 attr)⇒ **扇出是广播, 抢不走原厂的**
 *   ② 队列硬上限 **80**, 溢出**只丢我们自己的**并打一行日志, **不回压投递方**
 *      ⇒ 我们排空不及时 = 我们自己丢事件, 伤不到 mme-becker 也伤不到原厂
 *   ③ 我们崩了能回收: `ocb_free` -> `ntfy_freeClient`
 *   ④ TIME 事件在队列里**原地合并**, 不堆积
 * 而且**原厂自己就在订同一类事件**(PCM3Reload 传 mask=9 = PLAY|GENERAL)。
 * 🚨 我们只订 **PLAY(0x01)** 一类 —— 它一条就覆盖 换曲/元数据/随机/重复 四件事。
 *   绝不订 SYNC(0x02)/COPY(0x04): 媒体库同步时那两类是高频的。
 * 🚨 sigevent **全零 = SIGEV_NONE**: 不建 channel、不用 pulse。事件照样入队,
 *   我们用 subtype 103 主动排空 —— 单线程主循环下这是唯一正确形状。 */
#define MME_REGEVENT   53     /* clen=32 sbytes=32; +8=class掩码 +12=1(注册)/0(注销) +16..31=全零 sigevent */
#define MME_GETREGSTAT 95     /* 只读自检: 回包 +0 = 当前已注册的 class 位图 */
#define MME_GETEVENT  103     /* clen=24 sbytes=24, 回包 = {u32 type; u32 size; u8 data[]} */
#define MME_EVCLASS_PLAY 0x01
#define EV_NONE_          0
#define EV_TIME_          1
#define EV_TRACKCHANGE_   2
#define EV_PLAYSTATE_     4
#define EV_REPEATCHANGE_  9
#define EV_RANDOMCHANGE_ 10
#define EV_NOWPLAY_META_ 39
#define EV_BUF_SMALL_    55
/* 🚨🚨 **绝不发 subtype 48**(mme_set_notification_interval)。
 *   它的 handler 取的是 `*(ocb+24)` = **全设备共享的 attr** -> 共享 control context,
 *   而 53 取的是 `*(ocb+28)` = 我们私有的。一个写共享一个写私有。
 *   原厂自己设过这个值(PCM3Reload 0x081e04dc), 我们改就是**直接覆盖原厂的设定**且它不会察觉:
 *   调小 = 给原厂队列加压; 调大/调 0 = 原厂进度条变卡或停走。
 *   我们不需要它 —— 位置自己按 250ms 采样就够。 */
#define MME_BUTTON     18     /* mme_button: clen=24 sbytes=24 body@+8=按键码, 无应答。
                               * 服务端 handler 0x0811ae40 "RCV -> MME_IOMSG_TYPE_BUTTON: type=%d"
                               * -> cc_button(0x08108e00, mme-becker 的**真动态符号**, 我手工解
                               *    PT_DYNAMIC/DT_HASH(nchain=2604)亲自核过, 不是常量池配对猜的)
                               * ⚠ 18 不是 24(SHUTDOWN)/27(RMTRKSESSION)/28(SETTRKSESSION)/
                               *   48/60/62/66/89(DELETE_MEDIASTORES) 中的任何一个;
                               *   邻居 17=GETCCID(只读)、19=跳转表 default->ENOSYS, 都无害。 */

/* mm_button_t 的**按键码**(是 body 里的值, 不是 subtype)。
 * 出处①: references/qnx-official-docs/mme-headers/mm_types.h 的 `enum mm_button`,
 *        NEXT 是第 0 项;我自己从头数到 STOP 复核过 0/1/26/27/28。
 * 出处②: PCM3Reload.elf 两张 braf 跳转表 @0x081d1918 / @0x081d6f9c 落点 `mov #N,r5`,
 *        经 MmeApi::button@0x08ab1b30 原样透传(全链无 add/sub/mask)。两条链逐项一致。
 * ⚠️ 这张枚举**满是 DVD 味**(MENU_ANGLE/CHAPTER/FRAME_ADVANCE), 官方描述也是
 *   "for devices that support navigation" ⇒ button 很可能是 DVD/iPod 的导航路,
 *   **对蓝牙管不管用没有任何证据**, 只能上机打一枪看。别当保票。 */
#define MMB_NEXT        0
#define MMB_PREV        1
#define MMB_PAUSE      26
#define MMB_PLAY       27
#define MMB_STOP       28

extern int MsgSend(int coid, const void *smsg, int sbytes, void *rmsg, int rbytes);
extern int TimerTimeout(int id, int flags, const void *ev, const void *ntime, void *otime);
/* QNX 的 errno 是每线程的, 必须走这个函数 —— 项目里 com_stack.cc / overlay_spike.c 一直这么用 */
extern int *__get_errno_ptr(void);

static int g_mmefd = -1;        /* /dev/mme/default 的 fd —— **全程只开这一个** */
static int g_mmefd_bad = 0;     /* 开失败过就别反复重试, 免得每帧都去 calloc 一个 OCB */

static int mme_open(void){
    if(g_mmefd >= 0) return 0;
    if(g_mmefd_bad) return -1;
    g_mmefd = open(MME_DEV, O_RDWR, 0);
    if(g_mmefd < 0){
        g_mmefd_bad = 1;
        plat_log("[MME] ⚠ 打不开 " MME_DEV " -> 播放控制不可用\n");
        return -1;
    }
    plat_log("[MME] 已连上 fd="); p_logd(g_mmefd); plat_log("  (只开这一个)\n");
    return 0;
}
/* 注册/注销事件。形状与别的命令不同(**clen=32 不是 24**), 所以单独写, 不走 mme_send4。 */
static int mme_reg_events(int on){
    unsigned char m[32];
    int ev[8], i, rc;
    unsigned long long ns = 2000000000ULL;
    if(mme_open()) return -1;
    for(i = 0; i < 32; i++) m[i] = 0;          /* +16..31 保持全零 = SIGEV_NONE */
    m[0] = 0x13; m[1] = 0x01;
    m[2] = 32;   m[3] = 0;                     /* clen = 32 */
    m[4] = 19;   m[5] = 0;
    m[6] = (unsigned char)MME_REGEVENT; m[7] = 0;
    m[8] = MME_EVCLASS_PLAY;                   /* +8  只订 PLAY 这一类 */
    m[12] = (unsigned char)(on ? 1 : 0);       /* +12 1=注册 0=注销 */
    for(i = 0; i < 8; i++) ev[i] = 0;
    ev[0] = 5;
    TimerTimeout(0, 0x50, ev, &ns, 0);
    rc = MsgSend(g_mmefd, m, 32, 0, 0);
    if(rc < 0){
        plat_log(on ? "[MME] ⚠ 订阅事件失败 errno=" : "[MME] ⚠ 注销事件失败 errno=");
        p_logd(*__get_errno_ptr()); plat_log(" -> 退回纯轮询(功能不受影响, 只是慢一拍)\n");
        return -1;
    }
    g_ev_on = on ? 1 : 0;
    plat_log(on ? "[MME] 已订阅事件 class=PLAY(只这一类)\n" : "[MME] 已注销事件\n");
    return 0;
}
/* 每拍排空到空。**事件只负责把标志置脏, 不替代轮询** ——
 * 这样万一订阅不通/事件丢了, 页面照样靠轮询刷新, 只是慢一拍(优雅降级)。 */
static unsigned char g_evbuf[1024];
static void mme_drain_events(void){
    int guard, rc;
    if(!g_ev_on || g_mmefd < 0) return;
    for(guard = 0; guard < 100; guard++){      /* 队列上限 80, 100 够 */
        unsigned char m[24];
        int ev[8], i;
        unsigned long long ns = 500000000ULL;  /* 排空用短超时, 别拖主循环 */
        u32 type, size;
        for(i = 0; i < 24; i++) m[i] = 0;
        m[0] = 0x13; m[1] = 0x01;
        m[2] = 24;   m[3] = 0;
        m[4] = 19;   m[5] = 0;
        m[6] = (unsigned char)MME_GETEVENT; m[7] = 0;
        for(i = 0; i < 8; i++) ev[i] = 0;
        ev[0] = 5;
        TimerTimeout(0, 0x50, ev, &ns, 0);
        rc = MsgSend(g_mmefd, m, 24, g_evbuf, (int)sizeof g_evbuf);
        if(rc < 0) return;                     /* 失败就下拍再说, 不刷日志 */
        type = le32(g_evbuf);
        size = le32(g_evbuf + 4);
        if(type == EV_NONE_) return;           /* 队列空 —— 正常出口 */
        if(type == EV_BUF_SMALL_){             /* 1024 装不下这条, 放弃它 */
            static int once = 0;
            if(!once){ once = 1; plat_log("[MME] ⚠ 事件超过 1024 字节, 已放弃该条\n"); }
            return;
        }
        switch(type){
            case EV_TRACKCHANGE_:
            case EV_NOWPLAY_META_:             /* 无载荷, 语义 = "你自己去重读" */
                g_meta_dirty = 1; break;
            case EV_REPEATCHANGE_:             /* size=4, 值与 subtype 15 同一套编码 */
                if(size >= 4) g_repeat  = (int)le32(g_evbuf + 8); break;
            case EV_RANDOMCHANGE_:             /* size=4, 值与 subtype 14 同一套编码 */
                if(size >= 4) g_shuffle = (int)le32(g_evbuf + 8); break;
            default: break;                    /* TIME / PLAYSTATE: 我们本来就每拍读内存 */
        }
    }
}
/* 发一条命令。subtype 只能来自上面那组常量 —— **绝不接受运行时输入**。
 * rep/replen 为 0 = 不要应答。返回 0 = 成功。 */
/* 🚨 **每条命令的 clen/sbytes 是各自不同的, 必须逐条照抄, 绝不能用一个统一值**:
 *     mme_next/prev/stop  = **8 字节纯头**  (clen=8  sbytes=8  rmsg=NULL rbytes=0)
 *     mme_getclientcount  = clen=24 sbytes=24 rbytes=16
 *     mme_play_set_speed  = clen=24 sbytes=24 rbytes=0
 *     mme_button          = clen=24 sbytes=24 rbytes=0
 *     mme_play_get_info   = clen=**72** sbytes=8 rbytes=72   ← clen 和 sbytes 差得最离谱的一条
 *   照抄是对的, 保留。
 * ❌ 但这里原来写的因果**是错的**, 2026-08-13 已被自己推翻:
 *   我曾断定 "NEXT(10) 被拒是因为我拿 24 套了所有命令"。改成 clen=8/sbytes=8 之后
 *   **照样 rc=-1** ⇒ 线格式根本不是原因。真正的位置在服务端语义层:
 *   subtype 10 有自己真实的 handler(0x0811abc0 -> cc_next 0x08108a40, 内部 request=11),
 *   不落 default, 也不走 mgrid 门 ⇒ 拒绝发生在 cc/io-media 那一层。 */
/* errno -> 人话。**不是背的**: 逐项读自台架自己的
 * scratchpad/dockertmp/extract_source/proc/boot/libc.so.2 的 sys_errlist(314 项),
 * 61/89/48 我亲自解出来核过("No data" / "Function not implemented" / "Not supported")。
 * 带 ★ 的三个就是"NEXT 为什么被拒"的定案判据。 */
static const char *mme_errname(int e){
    switch(e){
    case 0:   return "0 (服务端没设 errno, 或失败发生在本地)";
    case 4:   return "EINTR(4) 被解阻塞";
    case 9:   return "EBADF(9) ★★ **服务端**说它没有设备 fd(不是我们的 fd 坏了 —— 紧接着的 get_speed "
                     "在同一 fd 上照样成功)。出处: cc_button -> _setgenericvalue(request=6) -> "
                     "0x0810ea40 `if(*(r9+16)==-1) 失败码 9`。2026-08-13 台架实测 button(18) 就是这个码 "
                     "⇒ 该源没有能接 button 的 io-media 设备";
    case 11:  return "EAGAIN(11) ★ cc->io-media 内部超时(260 被改写成 11) => 卡在下游";
    case 16:  return "EBUSY(16) 设备忙";
    case 22:  return "EINVAL(22) 参数不对 => 查 body/clen";
    case 47:  return "ECANCELED(47)";
    case 48:  return "ENOTSUP(48) 这个源不支持这条命令";
    case 61:  return "ENODATA(61) ★★ 没有下一首/没有 trksession => 判决: next 语义不适用于蓝牙流";
    case 89:  return "ENOSYS(89) ★ 服务端不认 mgrid/subtype => 我们的包写错了";
    case 260: return "ETIMEDOUT(260)";
    default:  return "(不在已知表里 => 上面三条假设全错, 重开)";
    }
}
/* 🚨 唯一允许出现在 subtype 上的值。铁律靠注释守不住, 这是运行期兜底。
 *   这张表里**没有** 24(SHUTDOWN)、27/28(TRKSESSION)、48、60、62、66、89(DELETE_MEDIASTORES)
 *   —— 它们和 next/prev 同形状(8 字节纯头), 写错一个数就是关机。 */
static int mme_subtype_ok(int s){
    return s == MME_STOP      || s == MME_SET_SPEED || s == MME_GET_SPEED ||
           s == MME_NEXT      || s == MME_PREV      || s == MME_SETRANDOM ||
           s == MME_SETREPEAT || s == MME_GETCLIENTS|| s == MME_BUTTON    ||
           s == MME_GETRANDOM || s == MME_GETREPEAT ||
           s == MME_REGEVENT  || s == MME_GETREGSTAT|| s == MME_GETEVENT;
    /* 🚨 注意 53 的邻居全是会动状态的: 52=play_resume_msid / 54=sync_directed /
     *   55=directed_sync_cancel。差 1 就是另一条合法命令 —— 白名单是最后一道网。
     *   **48 不在这张表里, 而且永远不许加**(见上面的说明)。 */
}
static int mme_send4(int subtype, int body, int clen, int sbytes, void *rep, int replen){
    unsigned char m[24];
    int ev[8], i, rc, er; unsigned t0, dt;
    unsigned long long ns = 2000000000ULL;   /* 2 秒 —— 挂住也不会拖死主循环 */
    if(!mme_subtype_ok(subtype)){
        plat_log("[MME] ⛔ 拒发: subtype "); p_logd(subtype);
        plat_log(" 不在白名单里(可能是 SHUTDOWN 一类的同形状命令)\n");
        return -1;
    }
    if(mme_open()) return -1;
    for(i = 0; i < 24; i++) m[i] = 0;
    m[0] = (unsigned char)(MME_IO_MSG & 0xff);  m[1] = (unsigned char)(MME_IO_MSG >> 8);
    m[2] = (unsigned char)clen;                 m[3] = (unsigned char)(clen >> 8);
    m[4] = (unsigned char)MME_MGRID;            m[5] = 0;
    m[6] = (unsigned char)subtype;              m[7] = (unsigned char)(subtype >> 8);
    m[8]  = (unsigned char)body;        m[9]  = (unsigned char)(body >> 8);
    m[10] = (unsigned char)(body >> 16); m[11] = (unsigned char)(body >> 24);
    for(i = 0; i < 8; i++) ev[i] = 0;
    ev[0] = 5;                                  /* SIGEV_UNBLOCK */
    TimerTimeout(0 /*CLOCK_REALTIME*/, 0x50 /*SEND|REPLY*/, ev, &ns, 0);
    /* 🚨 2026-08-13: 之前这里只打 rc=-1, 一句 "被拒/超时" 把**两种完全相反的故障**混在一起了 ——
     *   "被拒" 说明包递到了服务端而它不认(改 subtype/参数);
     *   "超时" 说明包根本没被处理(改路径, 或者服务端在等别的东西)。
     * 两个判据都要:
     *   ① errno —— QNX 每线程 errno, 必须 __get_errno_ptr()。已知 89=ENOSYS = mgrid/subtype 服务端不认。
     *   ② 耗时 —— 这条**不依赖我记不牢的 QNX errno 表**: TimerTimeout 设的是 2 秒,
     *      所以 dt≈2000ms 就是超时, 瞬回(dt 很小)就是服务端主动拒。errno 表记错也不影响这个判据。 */
    *__get_errno_ptr() = 0;
    t0 = plat_now_ms();
    rc = MsgSend(g_mmefd, m, sbytes, rep, replen);
    dt = plat_now_ms() - t0;
    er = *__get_errno_ptr();
    if(rc < 0){
        plat_log("[MME] ⚠ 命令 "); p_logd(subtype);
        plat_log(" 失败 rc="); p_logd(rc);
        plat_log(" errno="); p_logd(er);
        plat_log(" 耗时="); p_logd((int)dt); plat_log("ms -> ");
        if(dt >= 1500) plat_log("**超时**(2s TimerTimeout 到期, 服务端没应答 —— 不是拒绝) ");
        else           plat_log("**被拒**(瞬回 = 服务端主动 MsgError) ");
        plat_log(mme_errname(er)); plat_log("\n");
        /* "包本身写错了"的唯一可靠判据 = 把真正发出去的字节原样打出来。
         * 每个 subtype 只打一次, 不刷屏(控制台被写爆过三次, 见 KB)。 */
        {   static unsigned printed = 0;
            if(!(printed & (1u << (subtype & 31)))){
                printed |= (1u << (subtype & 31));
                plat_log("[MME]   发出的字节:");
                for(i = 0; i < sbytes && i < 24; i++){ plat_log(" "); p_logh((u32)m[i]); }
                plat_log("  clen="); p_logd(clen);
                plat_log(" sbytes="); p_logd(sbytes);
                plat_log(" rbytes="); p_logd(replen); plat_log("\n");
            } }
        return -1;
    }
    return 0;
}
static int mme_send(int subtype, int body, void *rep, int replen){
    return mme_send4(subtype, body, 24, 24, rep, replen);
}
/* 上/下一曲。**2026-08-13 台架实测定案**(蓝牙真在放音, get_speed=1000 前后各验一次):
 *   ① mme_next/prev(10/11, 8 字节纯头) —— ❌ **蓝牙下死路**。
 *      errno=61 ENODATA, 1ms 瞬回 ⇒ 包被服务端收下了(线格式没问题), 是**语义层**拒绝。
 *      官方 mme_mme.h 原话: "skip to the next title in **the track session**" ——
 *      蓝牙是流式源, MME 侧没有 trksession。**停播和真播放两种状态下都是 ENODATA**, 无混淆。
 *      对 USB/CD/iPod 这类有曲目会话的源它应该仍是正路, 所以保留先试。
 *   ② mme_button(18, body=MMB_NEXT/PREV) —— ✅ **蓝牙下真能跳曲, 已逐字节验证**:
 *      读 PCM3Reload 的曲名串(g_mmeS+0x6c, HB 串: len@p+8, 字符@p+17):
 *        NEXT: "Burn It All Down"(len16) -> "Forgotten Ghost"(len15)
 *        PREV: 连按两次退回 "Burn It All Down"(第一次是回到本曲开头, 常见行为)
 *      ⚠️ 我离线时判它"可能是 DVD/iPod 专用路、置信度 low" —— **实测推翻了**, 它对 A2DP 有效。
 *   🚨 **前置条件: 必须正在放音**。停播时 button 返回 **errno=9(EBADF)**, 那是**服务端**在说
 *      "没有设备 fd"(cc_button -> _setgenericvalue -> 0x0810ea40 `if(*(r9+16)==-1)`),
 *      不是我们的 fd 坏了 —— 同一个 fd 紧接着 get_speed 照样成功。
 * ①失败一次就记住, 之后直接走 ②(每次按键多一个 MsgSend 往返没意义, 日志也吵)。 */
static int g_mme_skip_dead = 0;
static int mme_skip(int fwd){
    if(!g_mme_skip_dead){
        if(mme_send4(fwd ? MME_NEXT : MME_PREV, 0, 8, 8, 0, 0) == 0) return 0;
        g_mme_skip_dead = 1;
        plat_log("[MME] next/prev(10/11) 不可用, 之后直接走 button(18)\n");
    }
    return mme_send(MME_BUTTON, fwd ? MMB_NEXT : MMB_PREV, 0, 0);
}
/* 随机/重复。**参数是选择器不是 subtype** —— 这样 subtype 常量只出现在上面的白名单区,
 * "subtype 必须是编译期常量、绝不接受外部输入"这条规矩就不会被调用方稀释掉。
 * rep=0 -> 随机(12/14);rep=1 -> 重复(13/15)。 */
static int mme_get_mode(int rep, int *out){
    unsigned char r[16]; int i;
    for(i = 0; i < 16; i++) r[i] = 0;
    if(mme_send(rep ? MME_GETREPEAT : MME_GETRANDOM, 0, r, 16)) return -1;
    *out = (int)((u32)r[0] | ((u32)r[1]<<8) | ((u32)r[2]<<16) | ((u32)r[3]<<24));
    return 0;
}
/* 🚨 **服务端不做上界检查**(反汇编确认), 越界值会被原样写进原厂持久设置,
 *   所以钳位是我们这边的责任, 而且必须在**发之前**钳, 不是发完再补救。 */
static int mme_set_mode(int rep, int mode){
    if(mode < 0 || mode > 4){
        plat_log("[MME] ⛔ 模式越界 "); p_logd(mode); plat_log(" (只允许 0..4), 不发\n");
        return -1;
    }
    return mme_send(rep ? MME_SETREPEAT : MME_SETRANDOM, mode, 0, 0);
}
/* 只读: 当前播放速率(0=暂停)。给"播放/暂停"按钮判方向用, 也是最安全的探针。 */
static int mme_get_speed(int *out){
    unsigned char rep[16]; int i;
    for(i = 0; i < 16; i++) rep[i] = 0;
    if(mme_send(MME_GET_SPEED, 0, rep, 16)) return -1;
    *out = (int)((u32)rep[0] | ((u32)rep[1]<<8) | ((u32)rep[2]<<16) | ((u32)rep[3]<<24));
    return 0;
}

int plat_command(int cmd, int arg){
    PcmState st;
    int d;
    switch(cmd){
        case CMD_SET_VOLUME:
            plat_read_state(&st);
            d = arg - st.volume;
            if(!d) return 0;                       /* 已经是这个音量, 不用投 */
            if(d > 255) d = 255; if(d < -255) d = -255;
            { int op = d > 0 ? PUP_OP_VOL_UP : PUP_OP_VOL_DOWN;
              if(pup_ensure_armed(op)) return -1;
              return pup_arm_op(PUP_LEVEL_REAL, op, (u32)(d > 0 ? d : -d), 0, 0); }
        case CMD_SET_SOURCE:                        /* arg = 原厂**槽**号(FM=11 AUX=26 BT=40) */
            if(!arg){ plat_log("cmd: 源号不能是 0\n"); return -1; }
            return pup_arm_op(PUP_LEVEL_REAL, PUP_OP_ENTERT_SOURCE_CHANGED, (u32)arg, 0, 0);
        case CMD_TUNE:                              /* arg = kHz */
            if(pup_ensure_armed(PUP_OP_TUNER_FREQUENCY)) return -1;
            return pup_arm_op(PUP_LEVEL_REAL, PUP_OP_TUNER_FREQUENCY, (u32)arg, 1, 0);
        /* ★ 2026-08-13: 播放控制走 **MME 直连**(不是傀儡 cave, 也不是模拟原厂坐标)。
         *   subtype 全是编译期常量 —— 见 mme_send 上面那三条铁律。 */
        case CMD_PLAY:   return mme_send(MME_SET_SPEED, 1000, 0, 0);   /* resume */
        case CMD_PAUSE:  return mme_send(MME_SET_SPEED, 0,    0, 0);   /* pause  */
        case CMD_SET_SHUFFLE: { int r = mme_set_mode(0, arg); if(!r) g_shuffle = arg; return r; }
        case CMD_SET_REPEAT:  { int r = mme_set_mode(1, arg); if(!r) g_repeat  = arg; return r; }
        case CMD_NEXT:   return mme_skip(1);
        case CMD_PREV:   return mme_skip(0);
        default:
            plat_log("cmd(不认识) c="); p_logd(cmd); plat_log(" a="); p_logd(arg); plat_log("\n");
            return -1;
    }
}

/* 🚨 PCM 上**不做转场动画**。台架实测: 整屏上屏 = 130ms(u32 拷贝) + 7ms(比对)。
 *   220ms 的淡入只够画 1-2 帧, 效果就是"闪一下黑再跳到位", 比直接切还难看。
 *   局部更新已经把常规重画压到 8ms, 但转场是整屏, 逃不掉。
 *   ⚠️ 别指望 store queue: MMU 开着时目标物理地址来自内核装的 UTLB 表项, 用户态碰即 address error;
 *   QEMU 也没建模它(sim 跑通≠车上跑通, 违反 sim==car 硬规则)。这条已判死, 别再试。
 *   真要提速看 Carmine 2D 硬件 blit(libgdcApiCarmine.so 里 gf_draw_* 是真函数体), 但它要抢绘图锁, 风险另算。 */
int plat_can_animate(void){ return 0; }

#endif /* PLAT_PCM_C */
