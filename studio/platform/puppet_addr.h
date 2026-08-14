/* puppet_addr.h —— **自动生成, 别手改**
 * 生成者: dev/build_flash_bench_puppet.py   真源: dev/build_puppet_cave.py
 * 它同时决定了刷进 flash 的那份 cave 和 studio 二进制里的常量 ⇒ 两边永远对齐。
 * 改了 cave 就重跑打包器, 然后重编 studio(bash studio/tools/build.sh pcm)。
 */
#ifndef PUPPET_ADDR_H
#define PUPPET_ADDR_H

#define PUP_BUILD_CKSUM 0x08b8d6c5u   /* 刷写包 POSIX cksum=146331333 —— 与台架 cksum 对得上才是同一版 */

/* ---- 代码 ---- */
#define PUP_CAVE   0x0860cb38u   /* hook 入口; 池字 0x08074690 指向它 */
#define PUP_EXEC   0x0860cbf4u   /* 假 vtable[4]: 跑在 AudioCtrlThread 的 dispatch 里 */
#define PUP_STUB   0x0860cd24u   /* 其余 vtable 槽: 纯 rts */
#define PUP_CHAIN  0x0860caa0u   /* 尾跳: 隔壁 PDC 桩 B 入口 */
#define PUP_POOL   0x08074690u

/* ---- 信箱 ---- */
#define PUP_MB     0x0865acc8u
#define PUP_MB_END 0x0865ad48u
#define PUP_CTL    0x0865acc8u   /* 控制块 */
#define PUP_EV     0x0865ad08u   /* 傀儡事件 */
#define PUP_VT     0x0865ad28u   /* 假 vtable, 8 槽 */
#define PUP_VT_SLOTS 8
#define PUP_EV_SPAN  0x20
#define PUP_CTL_SPAN 0x40

/* CTL 字段偏移(u8 = BUSY/LEVEL, 其余 u32) */
#define PUP_O_BUSY    0
#define PUP_O_LEVEL   1
#define PUP_O_CMD     4
#define PUP_O_RC      8
#define PUP_O_XRC    12
#define PUP_O_ACK    16
#define PUP_O_TIDP   20
#define PUP_O_TIDX   24
#define PUP_O_NPOST  28
#define PUP_O_OBJ    32
#define PUP_O_MODE   36
#define PUP_O_SRC    40
#define PUP_O_OP     44
#define PUP_O_ARG0   48
#define PUP_O_OPX    52
#define PUP_O_ARM    56
#define PUP_O_ARG2   60
/*  O_MODE 就是 arg1(旧名保留); O_SRC 是 hook 锁内定好的**实际 arg0**。
 *  O_OP   = 白名单序号(全零默认 = 0 = entertSourceChanged)。
 *  O_ARG0 = 非 F_ARGCMD 的行从这里取 arg0(允许写 0)。
 *  O_OPX  = hook 锁内锁存的序号, 只读给 exec 看(exec 仍自己判越界)。
 *  O_ARM  = 运行时武装位图, cave 用的是 (ARM | 1) ⇒ 序号 0 恒武装。
 *  O_ARG2 = **第三个参数格**(CTL 里最后一个可达字)。requestFrequency 的 sid 走它。 */

/* EV 字段偏移。+0x08 是原厂 pushBack(0x080943B4) **唯一**会写的字段;
 * studio 侧原来把这个 8 硬编码在 3 处 —— cave 侧改布局的话不会有任何编译期报错。 */
#define PUP_O_EV_NEXT 8

/* ---- 校验/枚举 ---- */
#define PUP_OBJ_LO 0x08643cc8u   /* OBJ 范围门: 先范围再解引用(S5) */
#define PUP_OBJ_HI 0x086ff000u
#define PUP_VPTR   0x085c4c5cu   /* 序号 0 那一行的 vptr(CPSoundPresCtrl)。⚠ 每一行有自己的 vptr, 在只读表里 */
#define PUP_TID    41             /* 序号 0 那一行的 tid(AudioCtrlThread)。⚠ 每一行有自己的 tid, 在只读表里 */
#define PUP_EV_SENTINEL 0x00007FFFu  /* EV+0x10: 恒 != 目标 -> post 永远走跨线程分支 */

#define PUP_LEVEL_OFF    0            /* 重启默认: hook 只读一个字节就直通 */
#define PUP_LEVEL_DRYRUN 1            /* 只回写线程索引, 一个原厂字段都不碰 */
#define PUP_LEVEL_REAL   2            /* 真调白名单里那一行的原厂函数 */

#define PUP_RC_POSTED    1   /* hook: 已入队 */
#define PUP_RC_NOLOCK    2   /* hook: 没抢到锁(同拍另一线程在用) */
#define PUP_RC_NOCMD     3   /* hook: 锁内重读 CMD 已被消费 */
#define PUP_RC_POSTFAIL  5   /* hook: post 返回 0 = 原厂拒绝入队 */
#define PUP_XRC_CALLED   1   /* exec: 三道门全过, 真调了 */
#define PUP_XRC_LVL1     10   /* exec: 级1, 只记线程索引 */
#define PUP_XRC_BADALIGN (-2) /* exec: OBJ 没 4 字节对齐(没解引用) */
#define PUP_XRC_BADRANGE (-3) /* exec: OBJ 不在区间(没解引用) */
#define PUP_XRC_BADVPTR  (-4) /* exec: *(OBJ) 不是这一行要的 vtable */
#define PUP_RC_BADOP     6   /* hook: OP >= NROWS(序号硬门) */
#define PUP_RC_NOTARMED  7   /* hook: ARM 里这一位没置 */
#define PUP_XRC_BADVPTR2 (-5) /* exec: *(this) 不是这一行要的子对象 vtable */
#define PUP_XRC_BADOP    (-6) /* exec: OPX >= NROWS(exec 自己那道硬门) */
#define PUP_XRC_NOTARMED (-7) /* exec: ARM 里这一位没置 */
#define PUP_XRC_BADFUNC  (-8) /* exec: 行里 FUNC==0(兜底, 走不到) */
#define PUP_XRC_NOPROXY  (-9) /* exec: B4 的 acquire 返回 0(KeyInput 服务没起) -> 什么都没发 */

/* 假 vtable 的 8 个槽(studio 逐字写进去) */
#define PUP_VT_INIT { 0x0860cd24u, 0x0860cd24u, 0x0860cd24u, 0x0860cd24u, 0x0860cbf4u, 0x0860cd24u, 0x0860cd24u, 0x0860cd24u }

/* ---- 编译期白名单(在 cave 尾部 0x0860cd28, 14 行 x 32 B, **只读**) ----
 * 信箱只给序号 PUP_O_OP; 序号先过 `op < PUP_NROWS` 硬门, 再过 PUP_O_ARM 武装位。
 * 🚨 序号即 ABI。TABLE_REV 变了就说明序号重排过 —— studio 必须跟着重编,
 *    别拿旧二进制配新 flash(PUP_BUILD_CKSUM 是这件事的死门)。
 * 🚨 只有 status=VERIFIED 的行进 PUP_ARM_VERIFIED; GATED/NOOP/PENDING 默认全死,
 *    要跑先往 ARM 里置对应位(重启即清 ⇒ 猜错 = 重启一次, 不是再刷一次)。
 */
#define PUP_TABLE_REV 3
#define PUP_NROWS    14
#define PUP_ROW_SIZE 32
#define PUP_TABLE    0x0860cd28u
#define PUP_ARM_BIT(op) (1u << (op))
#define PUP_F_ARGCMD 1   /* 这一行的 arg0 取消费掉的 CMD(否则取 PUP_O_ARG0) */
#define PUP_F_NOTHIS 2   /* 这一行没有 this: r4=0, OBJ 三道门整段跳过 */
#define PUP_F_KEYSYNTH 4 /* B4: acquire->publish->release 三步链, 参数是**值**不是指针 */
#define PUP_F_BYVAL  8   /* 这一行的原厂函数**按值收参**: r5/r6/r7 = 三个格的值 */

/* 三个参数格 -> 原厂函数看到的是 r5=&ARG0格 r6=&MODE格 r7=&ARG2格(小端, 一格通吃 u8/u16/u32)。
 * F_BYVAL / F_KEYSYNTH 的行例外: r5/r6/r7 直接就是三个格的**值**。
 * (F_KEYSYNTH: r5 = key|source<<16, r6 = status|slider<<16) */
/* FUNC=0 的行 = RESERVED 空行, 只占序号; 武装了也只到 PUP_XRC_BADFUNC。 */

#define PUP_OP_ENTERT_SOURCE_CHANGED     0  /* VERIFIED tid=41 this=OBJ         entertSourceChanged */
#define PUP_OP_VOL_UP                    1  /* VERIFIED tid=41 this=OBJ+0x218   volUp   requestIncreaseForegroundVolSteps */
#define PUP_OP_VOL_DOWN                  2  /* VERIFIED tid=41 this=OBJ+0x218   volDown requestDecreaseForegroundVolSteps */
#define PUP_OP_RESERVED_3                3  /* RESERVED tid=41 this=—(空行)       [RESERVED] 空行 —— 原 SET_DESIRED_APP, 实证是 muteSystem */
#define PUP_OP_TUNER_FREQUENCY           4  /* VERIFIED tid=48 this=OBJ         tuner requestFrequency */
#define PUP_OP_TUNER_SEEK                5  /* VERIFIED tid=48 this=OBJ         tuner requestSeek */
#define PUP_OP_TUNER_SKIP                6  /* VERIFIED tid=48 this=OBJ         tuner requestSkip */
#define PUP_OP_TUNER_SCAN                7  /* VERIFIED tid=48 this=OBJ         tuner requestScan */
#define PUP_OP_TUNER_ACTIVATE            8  /* VERIFIED tid=48 this=OBJ         tuner requestActivateTuner */
#define PUP_OP_TUNER_STATION_KEY         9  /* GATED    tid=48 this=OBJ         tuner requestStationKey */
#define PUP_OP_TUNER_PRESET_GROUP       10  /* GATED    tid=48 this=OBJ         tuner requestPresetGroup */
#define PUP_OP_TUNER_AUTOSTORE          11  /* GATED    tid=48 this=OBJ         tuner requestAutostore */
#define PUP_OP_TUNER_WAVEBAND           12  /* NOOP     tid=48 this=OBJ         tuner requestWaveband */
#define PUP_OP_B4_KEY_SYNTH             13  /* PENDING  tid=38 this=三步链         B4 keySynth (CHBProxyBase 三步链) */

/* 每一行期望的 *(OBJ) —— studio 用它决定去锚哪个单例(Sound / Tuner / 无)。
 * ⚠ **地址一律活体扫**: 台架 Tuner 与真车 Sound 都是 0x086EF01C, 同地址不同类。 */
#define PUP_VPTR_SOUND 0x085c4c5cu
#define PUP_VPTR_TUNER 0x085d69acu
#define PUP_ROW_VPTR_INIT { 0x085c4c5cu, 0x085c4c5cu, 0x085c4c5cu, 0x00000000u, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x085d69acu, 0x00000000u }
#define PUP_ROW_TID_INIT  { 41, 41, 41, 41, 48, 48, 48, 48, 48, 48, 48, 48, 48, 38 }
#define PUP_ROW_FLAGS_INIT { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6 }

/* 离线证据最硬、参数形状也对得上的行(可以直接武装)。
 * 🔴 仍然**没有任何一行上过机** —— 上机顺序见 references/puppet-bringup-checklist.md */
#define PUP_ARM_VERIFIED (PUP_ARM_BIT(PUP_OP_ENTERT_SOURCE_CHANGED) | PUP_ARM_BIT(PUP_OP_VOL_UP) | PUP_ARM_BIT(PUP_OP_VOL_DOWN) | PUP_ARM_BIT(PUP_OP_TUNER_FREQUENCY) | PUP_ARM_BIT(PUP_OP_TUNER_SEEK) | PUP_ARM_BIT(PUP_OP_TUNER_SKIP) | PUP_ARM_BIT(PUP_OP_TUNER_SCAN) | PUP_ARM_BIT(PUP_OP_TUNER_ACTIVATE))
/* 有副作用 / 状态耦合, 要单独论证才武装 */
#define PUP_ARM_GATED (PUP_ARM_BIT(PUP_OP_TUNER_STATION_KEY) | PUP_ARM_BIT(PUP_OP_TUNER_PRESET_GROUP) | PUP_ARM_BIT(PUP_OP_TUNER_AUTOSTORE))

/* 🔴 永不进白名单(构建期断言拦着) */
#define PUP_BANNED_WRAPPER  0x082a8f50u  /* wrapper 0x082A8F50(读 0x0864B500, 离线判不了) */
#define PUP_BANNED_MUTE     0x082a4a54u  /* entertSourceChanged(活体真名 desc 0x0865FC00 id 0x686; S4 复核: 真的 muteSystem 是 0x082A4250, 它不是 ⇒ 老理由作废。维持禁入的**新**理由: 签名没人逐指令核过, 且它是 0x082A4250 的邻居, 认错一次的地方不给第二次机会) */
#define PUP_BANNED_MUTESYS  0x082a4250u  /* muteSystem(★真的那个; 按值收 bool 且 exec 通用路径会把栈地址当 bool 写进原厂对象; 更硬的理由: 两个原厂调用点 requestMuteAudio 0x082AB99C / requestIncreaseForegroundVoldB 都拿**自己缓存的 *(this+100)** 当门, 我们绕过去调它那份缓存不会更新 ⇒ 最坏情况原厂再也不发 demute = 车持续无声, 而这条离线证不了。外加路线图明令先不放) */
#define PUP_BANNED_STOREKEY 0x082f3c58u  /* requestStoreStationKey(要 CHBString, cave 造不出) */
#define PUP_BANNED_STATION  0x082f4960u  /* requestStation(要 4 个参数格, 本版只有 3 格) */

#endif /* PUPPET_ADDR_H */
