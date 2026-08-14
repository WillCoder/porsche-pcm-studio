/*
 * plat_internal.h — PCM Studio · **引擎内部**平台接口
 *
 * 【为什么单独一个文件】
 * pcm_sys.h 是给**场景**看的接口: 画一帧、读状态、发命令。
 * 这里是引擎自己维护不变式和做诊断用的东西 —— 场景**不该也不能**碰:
 *   · 触摸门是"我们盖着 ⟺ 原厂收不到触摸"这条不变式的执行机构。
 *     只要有一个场景自己去 arm/disarm, 这条不变式就没人说得清了。
 *   · 内存探针是诊断工具, 出现在业务代码里就是设计事故。
 *
 * 只允许 main_pcm.c / platform/plat_pcm.c 包含它。构建期 lint 会检查
 * studio/scenes/ 下没有任何地方引用这些符号。
 */
#ifndef PLAT_INTERNAL_H
#define PLAT_INTERNAL_H

/* 触摸门。⚠️ 装/摘由 set_cover + plat_tick_watch 独占维护, 别在别处调。
 * 🚨 **维护不变式一律用 plat_ts_ensure()** —— 它返回"执行后到底装没装",
 *   而不是各家不同的返回码。下面两个是它的实现细节:
 *   plat_ts_arm() 成功返回 **1**(不是 0!), 失败返回负错误码。 */
int  plat_ts_ensure(int want);   /* 返回执行后的实际状态: 1=装着 0=没装 */
int  plat_ts_arm(void);
int  plat_ts_disarm(void);
void plat_ts_watch(void);
void plat_touchgate_probe(void);   /* 纯读探测, 零写入 */

/* 诊断: 读 PCM3Reload 任意地址 */
void plat_peek2(void);

/* 单实例锁。**main() 的第一句**, 早于任何 plat_log(首写带 O_TRUNC)。
 * 返回 0 = 锁到手; 非 0 = 已经有一个活着的 studio, 直接退出别继续。
 * 释放在 plat_shutdown() 里, 跟 gf 层同生共死。理由见 plat_pcm.c 的定义处。 */
int  plat_claim_singleton(void);

#endif /* PLAT_INTERNAL_H */
