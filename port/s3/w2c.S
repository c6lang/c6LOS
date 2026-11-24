/* windowed → call0 的 trampoline。
 *
 * 我们的内核（kernel/*.c6 生成的 C）是 **call0** ABI，而 port/s3/wifi_glue.c
 * 是 `-mabi=windowed`（必须的，见 docs/wifi-plan.md §120）。windowed 代码直接
 * 调 call0 函数会同时踩两个坑：
 *
 *   1) call<n> 把窗口轮转量写进 a0 的 bit31:30，call0 返回用的 `ret`
 *      （= jx a0）会跳到 0x82xxxxxx 这种地址；
 *   2) call0 被调方不 `entry`、不轮转窗口，直接作用在调用者的物理寄存器上。
 *
 * 所以每个内核函数配一个薄壳：`entry` 建立自己的窗口 → 调内核 → `retw`
 * 正常回到 windowed 调用者。
 *
 * 两个细节：
 *   * 目标可能被链接脚本放进 flash（0x42xxxxxx），离 IRAM 超过 `call0` 的
 *     ±512KB 范围，所以用 `l32r` + `callx0`；
 *   * 内层调用会把 a0 改成壳内部的返回地址，而 `retw` 要用带窗口标签的那个，
 *     所以在壳自己的栈帧里（entry 之外再多要 16 字节）存一份再取回来。
 *
 * 参数/返回值完全透传（call0 和 windowed 的 a2..a7 位置一样）。
 */

    .section .text, "ax", @progbits

    .align  4
7:  .word   0x50000F08   /* 诊断区：记录 **库** 调用 _queue_recv 的返回地址 */
    .align  4
6:  .word   0x50000F00   /* 诊断区 2：s3_w2c_queue_get 的调用者 */
    .align  4
12: .word   g_s3_win_spill_en  /* spill 总开关（见 ctx.c / wifi_glue.c） */
    .align  4
4:  .word   0x50001FA0   /* §1032：壳用它**存自己的 sp**（不依赖任何寄存器）*/

/* ---- 把祖先窗口整圈 spill 到栈上（IDF SPILL_ALL_WINDOWS 的完整版）------
 * 与 `xt_asm_utils.h` 里那个裸宏相比，官方 `_xt_context_save` 还多做三件事，
 * 少做任何一件这套机制就静默失效（这是前几轮"签名没变"的根因）：
 *
 *   1) 寄存器窗口溢出是 **level-1 异常**，向量末尾的 `rfwo` 会消耗 EPC1，
 *      所以要先把 EPC1 存下来、转完再写回；
 *   2) 溢出向量只在 PS.WOE=1 且 PS.EXCM=0 时才生效 —— WOE=0 时整段 rotw
 *      是**静默空转**（官方注释原话 "becomes a silent noop"）。所以先把
 *      INTLEVEL 抬到 7（屏蔽所有可屏蔽中断，溢出异常不受 INTLEVEL 影响）、
 *      清 EXCM、置 WOE；
 *   3) 转完只有"当前窗口"该是 live 的，WINDOWSTART 必须显式写成
 *      1<<windowbase，否则就是 §197 那个签名（windowbase=3 / ws=0x20）。
 *
 * 转 16 个窗口（64 aregs / 4）正好一圈，WINDOWBASE 回到原值，所以**不需要**
 * （也没办法）写 WINDOWBASE。`and a12,a12,a12` 是恒等写：即使被溢出到栈上，
 * 写回去的还是原值，所以它"不占用任何寄存器"。
 *
 * 破坏 a8..a11（windowed ABI 的 caller-saved）；a4..a7 与 a12..a15 一个字节
 * 都不能动（a4..a7 = 调用者的 a12..a15 是 callee-saved，a12..a15 = 调用者的
 * a0..a3 是 retw 要用的返回地址和帧指针）。 */
#ifdef S3_ONE_ABI
/* §332：全 windowed 构建下**不需要壳** —— 内核也是 windowed，同一套 ABI，
 * 名字直接别名到目标函数即可（省掉 entry/callx0/retw 和 spill）。 */
    /* 全 windowed 下 W2C 名字由链接器的 --defsym 直接指向 windowed 目标函数；
     * 这里不生成任何壳（GAS 的 .set 对跨对象符号无效）。 */
    .macro  W2C name, target
    .endm
    .macro  W2C_TRACE name, target
    .endm
    /* ★★★ §1015（**真正的根因修法**）：全 windowed 构建下 `WIN_SPILL`
     * **不能**是空宏！
     *
     * 原来这里写着"全 windowed 下内核自己管窗口，定义成空宏即可"。
     * 这个理由漏掉了一个关键事实：
     * **内核是 windowed 只保证"调用时"窗口账目正确，不保证"任务切换后"正确。**
     *
     * Xtensa 的寄存器窗口只有 64 个 areg（16 个 4 寄存器窗口，绕成一个环）。
     * ppTask 阻塞在 `osi_queue_recv` 里时，它的**祖先窗口还活在寄存器文件里**
     * —— 此时一旦发生任务切换，另一个任务的 `call8` 链会把窗口基址推着绕过
     * 一整圈，**把 ppTask 还活着的那几个祖先窗口物理覆盖掉**。
     * ppTask 被换回来时，`a1`（物理寄存器 17）里已经是别的任务留下的值，
     * 最终就是 0；随后 `ppTask+0x33` 的 `l32i.n a5, a1, 0` 以 0 为基址访存，
     * `exccause=0x1c` / `excvaddr=0`，掉进坏 sp 闸门 —— 就是困扰三十多轮的 `bWu1b`。
     *
     * 证据链：
     *   - `irq.c:410` 的注释早就写明"在中断里切走 = 让另一个任务去轮转窗口文件
     *     ⇒ 回来时祖先窗口已被覆盖，a1 变成垃圾"，并说阻塞点靠本宏 spill 保平安；
     *   - §1014 读 RTC 切换轨迹，**切换确实在发生**（另一个任务 sp=0x3fcc6870）；
     *   - §1000 证明本宏在最终构建里是空宏 ⇒ **那句"靠 spill 保平安"落空了**；
     *   - §1010 自动续跑过滤显示整个启动只有一次涉及 ppTask 栈区的欠载，而且
     *     是健康的 ⇒ a1 变 0 跟欠载处理器无关；
     *   - §980 全关中断故障照旧 ⇒ 因为阻塞点切换不是中断驱动的。
     *
     * 现在内核已经是 windowed（帧里有 `[sp-12]` 帧链），`s3_win_spill`
     * 官方实现可以安全工作，所以把这段恢复回来。它破坏 a8..a11（caller-saved，
     * 本壳本来就不依赖它们跨调用），a2..a7 先存帧里空闲的 [a1+64..84] 再取回。 */
    /* §1055 曾在 §1050 之上重开祖先窗口 spill（`osi_queue_recv` 正是 §1054
     * 指出的"有自由栈的 windowed 边界"）。真板实测：spill 确实跑了
     * （`ws=0xaaaa` = 整圈轮转后的位型），故障点也再次前移
     * （epc1 0x42017010 → 0x42017040），但仍死在 `port_task_switch` 附近。
     * ⇒ 方向对，但只给 `_queue_recv` 一处加壳不够（还有别的阻塞点）。已撤回。*/
    .macro  WIN_SPILL
    .endm

    /* ★★★★ §1056（§1054 的落地）：给**会阻塞**的 OSI 函数恢复一层薄壳，
     * 壳里先 `entry a1,128`（帧下方留出 128 字节**自由栈**），
     * 再 `call8 s3_win_spill` 把祖先窗口 spill 到自己栈上 ——
     * 这正是 `ctx.S` 里 `port_task_switch_c0` 注释要求的"安全边界"。
     *
     * sp 先存到 RTC（`0x50001FA0 + 8`，复用 §1032 的字面量 4:），
     * 因为 spill 的整圈轮转可能把它毁掉（§1030 的教训）。
     *
     * 真机上这些函数是**会阻塞**的（sem/mutex/event/delay），
     * 阻塞就会切任务，切任务就会毁掉祖先窗口 —— 所以必须逐个加壳。 */
    /* §§1114 ★★★★ 每个壳必须用**自己的 RTC 槽位**！
     * 原来五个壳共用 `0x50001FA0+8`，一旦发生**嵌套**
     * （如 `s3_w2c_queue_recv` → `osi_queue_recv_body` → `s3_w2c_task_yield`），
     * 内层壳会把外层的 sp 覆掉，外层返回时 `a1` 就是内层的 sp
     * 甚至 0。真板现场（0x500018C0）：`a1=0`、`epc1=0x420134e2`
     * （`= s3_w2c_queue_recv` 的 `retw`）、`windowbase=4`。
     * 现在每个壳用自己的偏移：8/12/16/20/24。 */
    .macro  W2C_SPILL name, target, slot=8
    .align  4
9:  .word   \target
    .align  4
    .global \name
    .type   \name, @function
\name:
    entry   a1, 128
    l32r    a8, 4b
    s32i    a1, a8, \slot
    /* §§1326 ★★★★★ **`a0` 必须存在自己的帧里！**
     *
     * `entry a1,128` 把调用者的 `a0` 保存在**调用者的帧**里。
     * 而 `s3_win_spill` 会把 WINDOWBASE **推满一整圈**，
     * 整圈轮转很可能覆掉那个调用者帧
     * ⇒ `retw` 从被覆盖的帧里取回一个脏 `a0`。
     * 真板证据（§1324）：`send_setup` 里读到 `a0=0x82028adc`，
     * 而那个地址根本不是任何 `call8` 的返回点。
     * 现在额外存一份在自己的帧里（`[a1+0]`），返回前恢复。 */
    s32i    a0, a1, 0
    /* §§1124 ★★★★ **根因修法**：参数在 a10..a15，不是 a2..a7！
     *
     * `call8`/`callx8` 会轮转窗口：调用者的 a2..a7 在被调者里是 **a10..a15**。
     * 原来存/恢复 a2..a7，等于把自己的未初始化寄存器当成参数传给了目标。
     * 真板证据（RTC 0x50000F40）：`_queue_recv` 进入 32 次，
     * 但 qh=0、item=0x42013668、ticks=0x42013668（都是代码地址）。 */
    s32i    a10, a1, 64
    s32i    a11, a1, 68
    s32i    a12, a1, 72
    s32i    a13, a1, 76
    s32i    a14, a1, 80
    s32i    a15, a1, 84
    /* §§1125：**两组都存**。`call8` 与 `callx8` 的轮转行为不完全一致，
     * 不确定库把参数放在哪一组；两组都保存并恢复，
     * 无论哪组携带参数都能原样传给目标。 */
    s32i    a2, a1, 88
    s32i    a3, a1, 92
    s32i    a4, a1, 96
    s32i    a5, a1, 100
    s32i    a6, a1, 104
    s32i    a7, a1, 108
    mov     a9, a1
    call8   s3_win_spill
    l32r    a8, 4b
    l32i    a1, a8, \slot
    l32i    a10, a1, 64
    l32i    a11, a1, 68
    l32i    a12, a1, 72
    l32i    a13, a1, 76
    l32i    a14, a1, 80
    l32i    a15, a1, 84
    l32i    a2, a1, 88
    l32i    a3, a1, 92
    l32i    a4, a1, 96
    l32i    a5, a1, 100
    l32i    a6, a1, 104
    l32i    a7, a1, 108
    mov     a9, a1
    l32r    a8, 9b
    callx8  a8
    l32i    a0, a1, 0
    retw
    .endm

    /* 会阻塞的四个：取信号量 / 锁互斥量 / 等事件 / 任务延时 */
    W2C_SPILL s3_w2c_sem_pend,   os_sem_pend,   8
    W2C_SPILL s3_w2c_mutex_pend, os_mutex_pend, 12
    W2C_SPILL s3_w2c_event_wait, os_event_wait, 16
    W2C_SPILL s3_w2c_task_delay, os_task_delay, 20
    /* ★★★★ §1084：队列接收也是**阻塞点**（队列空就等），必须享受 §1056 同款
     * 处理：先 `entry a1,128` 留出自由栈，再 spill 祖先窗口。
     *
     * 真板证据（§1082/§1083）：任务切换真正发生之后，第一次崩就崩在
     * `osi_queue_recv_body+0x268` 的 `retw.n`（EXCCAUSE=28,
     * EXCVADDR=0xfffffff4、a1=0）—— 正是"阻塞后返回时祖先窗口已被覆盖"。
     * 之前只给 sem/mutex/event/delay 加了壳，漏了 queue_recv。 */
    W2C_SPILL s3_w2c_queue_recv, osi_queue_recv_body, 24
#else
    .macro  WIN_SPILL
    /* §386 教训：**绝不能在这个宏里放 `.word` 字面量**。
     * WIN_SPILL 是内联展开在指令流**中间**的（不像 W2C 的 `9:` 那样在函数
     * 标签之前），插一个 `.align 4` + `.word` 就等于让 CPU 顺序执行到数据字上
     * ——真板症状是 `TRAP pc=s3_w2c_queue_init+..` 且 `a0` 带着窗口标签。
     * 需要诊断时用 `movi aN, 常量`（能装下 16 位）或另找字面量池。 */
    /* §315：改用 IDF 官方 `window_spill_function`（下面 s3_win_spill）来
     * spill 祖先窗口。之前这里是手写的一圈 `rotw`：rotw 落到活窗口上会触发
     * 窗口溢出异常，而溢出处理体要用 `[a1-12]` 那条**帧链**去算落点；
     * 我们这套 call0 内核的帧里那条链是垃圾，于是 `s32e a0,a9,-16` 直接用
     * 脏 a9 写飞（§312 的 RTC 记录：vaddr=0xffffffd1 = a9-16，a9=0xffffffe1）。
     *
     * 官方版本不靠帧链：它用一串 `call4/8/12` 深度调用把窗口基址推满一整圈，
     * 由硬件溢出机制把每个活窗口 spill 到它自己的栈帧里，返回时再退回来。
     * 这个函数会破坏 caller-saved 的 a2..a7，而本壳要把参数透传给内核，
     * 所以先存到帧里空闲的 [a1+64..] 再取回。 */
    /* §447 ★ 开关必须在这里判断，否则它形同虚设。
     *
     * `g_s3_win_spill_en` 的初值是 0（ctx.c:57），`s3_wifi_osi_init()` 里也
     * 有一段"对照实验"把它压回 0 —— **但 `s3_win_spill` 是 IDF 官方实现，
     * 它自己根本不看这个变量**（反汇编：`entry a1,48` 之后直接按 `a0` 的
     * CALLINC 位 `bbci/bbsi` 递归 `call4/8/12`）。所以这个开关从加进来那天
     * 起就没有生效过，spill 一直在跑。
     *
     * 而 spill 的设计就是"把 WINDOWBASE 推满一整圈"，推满必然触发窗口溢出
     * 异常，溢出处理体要用 `[a1-12]` 那条帧链去算落点 —— 我们这套 call0
     * 内核的帧里没有这条链，于是它拿脏 a9 写飞。§446 真板现场完全吻合：
     *     exccause=0x1d(StoreProhibited)  excvaddr=0x0005ffe0
     *     epc1=0x42014ee7 = s3_win_spill+0x40 的 `call12`
     *     windowbase=0x0e（被 spill 推到 14），pc 卡在 `_vec_level1` 入口
     *
     * 不 spill 会怎样：壳自己已经把 a0..a15 完整存进帧里（下面 0..60 那 16 条
     * s32i），窗口快照是完整的；wb 因此稳定在 0..4 之间，不会绕圈、也就不会
     * 触发溢出处理。这正是 §230 想做而没做到的对照实验。
     *
     * 用 a8 检查（spill 本来就会破坏 a8..a11，不额外损失）；
     * 5: 是本文件唯一空闲的数字标号，3 处展开靠 `5f` 各自向后配对。 */
    l32r    a8, 12b
    l32i    a8, a8, 0
    beqz    a8, 5f
    s32i    a2, a1, 64
    s32i    a3, a1, 68
    s32i    a4, a1, 72
    s32i    a5, a1, 76
    s32i    a6, a1, 80
    s32i    a7, a1, 84
    call8   s3_win_spill
    l32i    a2, a1, 64
    l32i    a3, a1, 68
    l32i    a4, a1, 72
    l32i    a5, a1, 76
    l32i    a6, a1, 80
    l32i    a7, a1, 84
5:
    .endm

    .macro  W2C name, target
    /* l32r 只能往前引用，所以字面量必须放在指令之前；用数字标签 9 保证
     * 每次展开都指向紧邻自己的那一个。 */
    .align  4
9:  .word   \target
    .align  4
    .global \name
    .type   \name, @function
\name:
    /* 帧布局（128 字节）：
     *   [a1 + 0 .. 15]   ABI 保留，不碰
     *   [a1 + 16 .. 31]  自己的保存槽（带窗口标签的 a0）
     *   [a1 + 32 .. 127] 空闲
     * 帧**下方**再留 64 字节死区（addi -64）：spill 出来的窗口槽位落在
     * [a1-64, a1) 里，之后内核的调用帧从 a1-64 往下长，两者不重叠。
     * 这是本轮的关键顺序：**先 spill、再把 sp 降下去调内核**。
     * 反过来（先降 sp 再 spill）槽位就会落在内核帧里，等于踩内核的局部量。 */
#ifdef S3_W2C_SAVEALL
    /* 保存全部 16 个寄存器（§245）——这一版是理论正确的：
     *
     * 我们这层壳是 **windowed** 函数（被闭源库用 call8 调），却要 `callx0` 进
     * **call0** 的内核函数。call0 调用**不轮转窗口**，被调方可以随手把 a0..a15
     * 里的 caller-saved 全部改掉；而 windowed 的调用者（库）依赖窗口机制来保存
     * 它自己的寄存器，根本没料到同一个窗口里的 a4..a15 会被人从背后改掉。
     * 之前只存了 a0（或 a0+a12..a15），于是 `osi_semphr_take` 这种回调返回时
     * `retw.n` 用的 a0 已经变成栈地址了（真板 §242/§243 的症状）。
     * 这里把 a0..a15 全存全取，壳就变成"对调用者完全透明"的函数。
     * 帧：entry 已经存了 a0..a3 到 [a1+0..12]，我们把 a4..a15 存到
     * [a1+16..]（entry 帧 64 字节：16 个槽）。 */
    entry   a1, 64
    s32i    a4,  a1, 16
    s32i    a5,  a1, 20
    s32i    a6,  a1, 24
    s32i    a7,  a1, 28
    s32i    a8,  a1, 32
    s32i    a9,  a1, 36
    s32i    a10, a1, 40
    s32i    a11, a1, 44
    s32i    a12, a1, 48
    s32i    a13, a1, 52
    s32i    a14, a1, 56
    s32i    a15, a1, 60
    l32r    a8, 9b
    callx0  a8
    l32i    a4,  a1, 16
    l32i    a5,  a1, 20
    l32i    a6,  a1, 24
    l32i    a7,  a1, 28
    l32i    a8,  a1, 32
    l32i    a9,  a1, 36
    l32i    a10, a1, 40
    l32i    a11, a1, 44
    l32i    a12, a1, 48
    l32i    a13, a1, 52
    l32i    a14, a1, 56
    l32i    a15, a1, 60
    l32i    a0,  a1, 0
    retw                    /* retw 会自己用 [a1+4] 里的值恢复调用者的 a1 */
#elif defined(S3_W2C_MIN)
    /* 极简版（§231）：只保存 a0。 */
    entry   a1, 32
    s32i    a0, a1, 0
    l32r    a8, 9b
    callx0  a8
    l32i    a0, a1, 0
    retw
#else
    entry   a1, 160
    s32i    a0, a1, 16           /* 存下带窗口标签的返回地址 */
    s32i    a8,  a1, 40          /* §385：调用者的 a0..a3 —— 第一件事就存 */
    s32i    a9,  a1, 44
    s32i    a10, a1, 48
    s32i    a11, a1, 52
    /* §390：**a4..a7 也必须存**（= 调用者的 a12..a15，windowed 的 callee-saved）。
     * 到这一步为止壳还一条指令没执行过（上面的 4 条只写内存），所以 a4..a7
     * 仍是调用者的原值。存到 96..108（避开 WIN_SPILL 用的 64..84）。 */
    s32i    a4,  a1, 96
    s32i    a5,  a1, 100
    s32i    a6,  a1, 104
    s32i    a7,  a1, 108
    /* §385（真 bug，比 §367 更致命）：**a8..a11 也必须保存**！
     *
     * 本壳被窗源库用 `call8` 调用，窗口轮转 8 个寄存器：
     *     壳的 a0..a7  = 调用者的 a8..a15
     *     壳的 a8..a15 = 调用者的 a0..a7
     * ⇒ **壳的 a8/a9/a10/a11 就是调用者的 a0（返回地址！）/a1（栈指针！）/
     *    a2/a3**。
     * 而壳里到处用 a8/a9 做临时（`rsr a8, ps`、`l32r a8, 9b`、
     * `l32i a9, a8, 0`），随后 `callx0` 进 call0 内核更是把 a8..a11 全冲掉。
     * 于是库拿到的"自己的返回地址"变成了我们的字面量地址 —— 真板实测
     * `a0=0x8037c6c1`（`0x4037c6c1` 正是 `osi_wifi_create_queue` 的地址，
     * 带 call8 的 CALLINC=2 标签），`retw` 于是把 PC 送到那里，立即 TRAP。
     *
     * 修法：a8..a11 在**做任何事之前**就存进帧（偏移 40/44/48/52），
     * `retw` 之前再原样还原。同时把帧从 128 加到 160（多出的 16 字节
     * 仍远远低于 spill 死区，不影响 §378 的布局）。
     * §367（真 bug！）：**a12..a15 也必须保存**。
     *
     * 本壳是 windowed 函数，被闭源库用 `call8` 调进来。窗口轮转 8 个寄存器后，
     * 壳的 **a12..a15 就是调用者的 a4..a7**，而 a4..a7 在 windowed ABI 里是
     * **callee-saved** —— 库把长期存活的指针放在那儿，根本没料到有人会改。
     * 我们随后 `callx0` 进 call0 内核，GCC 生成的 call0 代码把 a12..a15 当
     * caller-saved 随手改掉 ⇒ 库里的指针被毁，之后就是"垃圾基址 + 假返回地址"
     * 那类现场（§364：a1=0x50000b00、epc1=0x4201860e 就是这么来的）。
     * §245 的 S3_W2C_SAVEALL 版本正是为这个写的，但它没有 PS 和 spill 死区；
     * 这里把两边的要点合成一份默认实现。 */
    s32i    a12, a1, 24
    s32i    a13, a1, 28
    s32i    a14, a1, 32
    s32i    a15, a1, 36
    /* §385：真正的调用者 a0..a3（= 壳的 a8..a11）—— **必须在任何临时
     * 使用之前**就落栈，下面 `rsr a8, ps` 一开始就会把它盖掉。 */
    /* §310：**把 PS 存下来、callx0 之后再写回**。
     * 内核是 call0 ABI，闭源库是 windowed；两者混用时 PS.CALLINC（bit17:16）
     * 是 `retw` 判断"要回退几个窗口"的唯一依据。真板实测：从壳里 callx0
     * 进内核再回来，CALLINC 可能已经不是调用者那一档（抓到 CALLINC=2
     * 而实际是 call8 进来），壳末尾的 `retw.n` 就成了**非法指令**
     * （cause=0、pc 正好落在我们 osi_queue_recv_body 的 retw.n 上）。 */
    rsr     a8, ps
    s32i    a8, a1, 20
    l32r    a8, 12b
    l32i    a9, a8, 0
    beqz    a9, .Lno_spill\@
    WIN_SPILL
.Lno_spill\@:
    /* §378（真 bug）：**spill 死区必须 ≥ 480 字节**，原来只有 128。
     *
     * `s3_win_spill` 是靠一串深调用把寄存器文件填满、由硬件溢出机制把活窗口
     * 压到栈上的。这一串的栈开销：
     *     call8            -32
     *     entry a1,48      -48
     *     4 × (call12 -48, entry a1,48 -48)   -384
     *     entry a1,16      -16
     *   ------------------------------------------
     *   合计 **约 480 字节**（都在壳的 sp 之下）
     *
     * 而壳原来只 `addi a1,a1,-128` 就 `callx0` 进内核 —— 内核的帧正好压在这
     * 480 字节里，**把刚 spill 出来的祖先窗口全部盖掉**。等这一层层返回、
     * 硬件从栈上把窗口下溢回来时，读到的就是内核的局部量 ⇒ 真板症状正是
     * "a0 变成了别的代码地址（还带着窗口标签 0x82xxxxxx）"、
     * 然后 `rfe` 直接 InstrFetchProhibited（§377 现场）。
     *
     * 取 768 留足余量（任务栈都是 6144+，放得下）。 */
    /* §393：**栈预算检查**。s3_win_spill 的级联要 ~480 字节，这里再降 768，
     * 加起来 ~1248 字节都要在**同一个任务栈**上。3c 的深递归本身已经吃掉了
     * 不少，最深处会撞到栈底 —— 真板现场正是 a1 变成 0。
     * 降 768 改成降 512（480 是实测开销，512 够用且省 256）。 */
    /* §398：死区**不是越大越好** —— 它吃的是**调用者任务**的栈。
     *
     * `s3_win_spill` 实测要约 784 字节（4 层 call12，见 §396），但壳执行
     * spill 时调用者栈上还有它自己的活帧，两边加起来才是总预算。
     * 试过 896：隔离用例没变化，但 WiFi 构建在更早的 PHY 阶段就压穿栈
     *（`get_tx_gain_value`，vaddr=0xc）。512 是"够 spill 用又不压穿 PHY"
     * 的折中值。 */
    addi    a1, a1, -128
    addi    a1, a1, -128
    addi    a1, a1, -128
    addi    a1, a1, -128          /* 共 -512 */
    l32r    a8, 9b
    callx0  a8
    movi    a9, 512               /* 回退 -512（addi 立即数上限 127） */
    add     a1, a1, a9
    l32i    a12, a1, 24          /* §367：还调用者的 a4..a7 */
    l32i    a13, a1, 28
    l32i    a14, a1, 32
    l32i    a15, a1, 36
    l32i    a8, a1, 20           /* 先借 a8 写 PS，随后马上还原它 */
    wsr     a8, ps
    rsync
    l32i    a0,  a1, 16
    /* §385：把调用者的 a0..a3 还回 a8..a11，且必须是**最后一步**
     *（这四条之后到 `retw` 之间不能再碰 a8..a11，也不能再碰 a0）。 */
    l32i    a8,  a1, 40
    l32i    a9,  a1, 44
    l32i    a10, a1, 48
    l32i    a11, a1, 52
    l32i    a4,  a1, 96           /* §390：还调用者的 a12..a15 */
    l32i    a5,  a1, 100
    l32i    a6,  a1, 104
    l32i    a7,  a1, 108
    retw
#endif
    .endm


/* 带调用点追踪的版本：在 entry 之后记录 a0（此刻它就是 windowed 调用者的
 * 真实返回地址），存进 RTC 0x50000F00 开始的环（每项 2 字：返回地址、计数）。 */
    .macro  W2C_TRACE name, target
    .align  4
9:  .word   \target
    .align  4
    .global \name
    .type   \name, @function
\name:
    entry   a1, 128
    s32i    a0,  a1, 16
    s32i    a12, a1, 20
    s32i    a13, a1, 24
    s32i    a14, a1, 28           /* §367：调用者的 a6/a7（callee-saved） */
    s32i    a15, a1, 32
    l32r    a12, 6b
    l32i    a13, a12, 4
    s32i    a0, a12, 0
    addi    a13, a13, 1
    s32i    a13, a12, 4
    l32r    a8, 12b
    l32i    a9, a8, 0
    beqz    a9, .Lno_spill\@
    WIN_SPILL
.Lno_spill\@:
    addi    a1, a1, -64
    l32r    a8, 9b
    callx0  a8
    addi    a1, a1, 64
    l32i    a0,  a1, 16
    l32i    a12, a1, 20
    l32i    a13, a1, 24
    l32i    a14, a1, 28
    l32i    a15, a1, 32
    retw
    .endm
#endif  /* S3_ONE_ABI */

/* 注意：堆**不**走内核的 os_sys_*（那两个在 port/s3/fast.c，LEAN 构建里
 * 不编进来）。lwIP 的 malloc/calloc/realloc 直接落在 wifi_glue.c 的
 * osi_malloc 堆上，和闭源库的 free() 是同一个堆（见 §210）。 */

/* ---- 读当前 sp（诊断用，§239）---- */
    .align  4
9:  .word   0
    .align  4
    .global s3_w2c_sp_probe
    .type   s3_w2c_sp_probe, @function
s3_w2c_sp_probe:
    entry   a1, 32
    mov     a2, a1
    retw

/* ---- 看门狗（§228）：os_wdt_feed/os_wdt_start 是 call0 的 C 函数（flash.c），
 * windowed 岛里不能直接调 —— 必须过这个壳，否则返回地址带窗口标签、
 * 一返回就跳飞（真板实测：TRAP cause=0 pc=0x8037a7e9，正是带标签的 a0）。 */
    /* §269：windowed 侧想睡一会儿只能走这个桥（直接调 call0 的 os_task_delay 会 TRAP） */
    W2C s3_w2c_delay,          os_task_delay
    W2C s3_w2c_wdt_feed,       os_wdt_feed
    W2C s3_w2c_wdt_start,      os_wdt_start

/* ---- 关中断（OSI 的 _wifi_int_disable/_restore，§226）----
 * 内核的 port_irq_save/restore 是 rsil/wsr ps，正好是 IDF portENTER_CRITICAL
 * 的等价物。库在临界区里不会阻塞（IDF 下也是这么用的）。 */
    W2C s3_w2c_irq_save,       port_irq_save
    W2C s3_w2c_irq_restore,    port_irq_restore

/* ---- 信号量 / 互斥 ---- */
    W2C s3_w2c_sem_init,       os_sem_init
    W2C s3_w2c_sem_destroy,    os_sem_destroy
    W2C s3_w2c_sem_pend,       os_sem_pend
    W2C s3_w2c_sem_post,       os_sem_post

/* ---- 递归互斥（内核有 owner 追踪，IDF 的 API 锁需要它）---- */
    W2C s3_w2c_mutex_init,     os_mutex_init
    W2C s3_w2c_mutex_destroy,  os_mutex_destroy
    W2C s3_w2c_mutex_pend,     os_mutex_pend
    W2C s3_w2c_mutex_post,     os_mutex_post

/* ---- 队列 ---- */
    W2C s3_w2c_queue_init,     os_queue_init_ex
    W2C s3_w2c_queue_put,      os_queue_put_item
    W2C_TRACE s3_w2c_queue_get, os_queue_get_item
    W2C s3_w2c_queue_count,    os_queue_count
    W2C s3_w2c_queue_try_get,  os_queue_try_get_item
    W2C s3_w2c_queue_try_put,  os_queue_try_put_item
    W2C s3_w2c_queue_destroy,  os_queue_destroy

/* ---- 事件组 ---- */
    W2C s3_w2c_event_init,     os_event_init
    W2C s3_w2c_event_set,      os_event_set
    W2C s3_w2c_event_clear,    os_event_clear
    W2C s3_w2c_event_wait,     os_event_wait
    W2C s3_w2c_event_destroy,  os_event_destroy
    W2C s3_w2c_event_get,      os_event_get

/* ---- 任务 / 时间 ---- */
    W2C s3_w2c_task_create,    os_task_create
    W2C s3_w2c_task_delete,    os_task_delete
    W2C s3_w2c_task_suspend,   os_task_suspend
    W2C s3_w2c_task_resume,    os_task_resume
    W2C s3_w2c_task_delay,     os_task_delay
    W2C s3_w2c_task_yield,     os_task_yield
    W2C s3_w2c_tick_get,       os_tick_get
    W2C s3_w2c_task_probe,     os_task_probe
    W2C s3_w2c_tick_hz,        os_tick_hz
    W2C s3_w2c_current_task,   os_task_current_slot
    W2C s3_w2c_slot_of_id,     os_task_slot_of_id


/* 在 osi_queue_recv 入口处记录 **库** 的返回地址（windowed 的 call8 值），
 * 用来定位到底是库里的哪个函数在收队列。
 * 参数按 windowed 约定在 a2/a3/a4，转发给 C 侧要用 a10/a11/a12；
 * a12/a13 属于调用者（callee-saved），必须先存栈再借用。 */
    .section .text, "ax", @progbits
    .align  4
11: .word   0x50001560
    .align  4
10: .word   g_lra_seq   /* 壳**自己**的序号：以前和函数体共用 g_recv_seq，
                         双方的写入会互相覆盖，导致 task1 的返回地址被冲掉 */
    .align  4
8:  .word   osi_queue_recv_body
    .align  4
    .global osi_queue_recv
    .type   osi_queue_recv, @function
osi_queue_recv:
    /* 这条壳是 ppTask 真正阻塞的地方，spill 必须在这里做（理由见文件头
     * WIN_SPILL 的注释）：此刻 a1 就是本壳的帧基址，帧下的 48 字节
     * （IDF 溢出向量用 [sp-16..-4]，帧链在 [sp-12]）都在自由栈里。 */
    entry   a1, 128
/* §934/§955: [sp-12] = 上一帧 sp（帧链），_win_ovf8/_win_unf8 都要用。
     * 地址必须是**本壳当前的 sp - 12** —— 现在 sp 始终 = B（-64 死区已删），
     * 所以 [B-12] = B+128（ppTask 的 sp）与下面 callx8 真正使用的 sp 一致。
     * 这条链是 windowed ABI 的硬性要求，**不能挂在实验开关后面**，
     * 否则最终构建（不定义 S3_SPILL_SHELL）里这条链根本不存在，
     * 溢出/欠载时硬件就把垃圾当帧链用。 */
    movi    a10, 128
    add     a10, a10, a1
    addi    a8, a1, -12
    s32i    a10, a8, 0
    mov     a9, a1
    /* §1041 曾在壳入口把 WINDOWSTART 全 16 位都标成 live（逼每次调用都走溢出
     * spill），用来验证"window N 的 a9 与 window N+4 的 a1 同一物理寄存器、
     * 绕环到 WB=0 时踩掉 ppTask 的 a1"这个别名机制。
     * 真板实测：故障记录**逐字节不变**（a1=3 / epc1=0x403822ef）。已撤回。*/
    /* §1040 曾在这里把 window 0 的 WS 位（bit 0）显式补回再进深层调用，
     * 想让硬件走正常溢出 spill 而不是从空记录欠载。
     * 真板实测：`ws` 确实变成 0x05（补位生效），但故障记录**一模一样**
     * （a1=3 / epc1=0x403822ef）⇒ 欠载到 window 0 也不是最终机制。已撤回。*/
    /* ★★ §1032：把本壳 sp 存进 **RTC 内存**，返回后从内存取回。
     * §1030 试过用 callee-saved 的 a13 保存，实测 a13 也被污染成
     * 0x50001308 ⇒ 这条链上没有任何寄存器可信。内存不会骗人。*/
    l32r    a8, 4b
    s32i    a1, a8, 0
    s32i    a12, a1, 20
    s32i    a13, a1, 24
    s32i    a14, a1, 28           /* §367：调用者的 a6/a7 也是 callee-saved */
    s32i    a15, a1, 32
    /* 按调用序号把库侧返回地址存进 4 个槽（0x50000F08+4*i） */
    l32r    a8, 10b
    l32i    a9, a8, 0
    addi    a10, a9, 1
    s32i    a10, a8, 0
    extui   a9, a9, 0, 2
    slli    a9, a9, 2
    l32r    a8, 7b
    add     a8, a8, a9
    s32i    a0, a8, 0
    s32i    a0, a1, 16
    /* spill 要在把 a10/a11/a12 改成参数之前做：它会用到 a8..a11 */
    l32r    a8, 12b
    l32i    a9, a8, 0
    beqz    a9, .Lrecv_nospill
    /* §1000：这里**本来就是空操作**。
     *
     * 曾怀疑 `s3_win_spill` 的深 call12 级联把 ppTask 的活窗口盖掉
     * （依据是 §999 在 `_win_ovf8` 入口采样 12 次，a9 全在
     * `0x3fcc05xx..07xx`，没有一次落在 ppTask 的 `0x3fcc47xx`）。
     * 把这行注释掉重新构建，**md5 与改前逐字节一致**（`a994b452…`）
     * ⇒ 最终构建里 `WIN_SPILL` 是空宏（见本文件开头 62 行处的说明），
     * 这条路径根本没代码。**该假设排除。** */
    WIN_SPILL
.Lrecv_nospill:
    mov.n   a10, a2
    mov.n   a11, a3
    /* §1029 曾在这里把 recv 超时强制成 0 做"非阻塞 ⇒ 不切换"的判别。
     * 实测：调用期间 `RESTORE_FRAME` 计数**完全没变**（514→514，零次切换），
     * 但崩溃照旧（epc1=0x42013313 / a1=0）⇒ **任务切换也被排除**。
     * 行为已恢复。*/
    mov.n   a12, a4
    l32r    a9, 11b
    s32i    a0, a9, 0            /* 诊断：callx8 之前的 a0 */
    /* §955 ★★ 删除原来的 `addi a1,a1,-64`（及配对 `addi a1,a1,64`）——
     * 它让帧链错位 64 字节。IDF _WindowOverflow8/_WindowUnderflow8 的约定是
     * 槽位相对**当前 sp**：[sp-16..-4] = 本窗口 a0..a3，[sp-12] = 调用者 sp。
     * 链写在 [B-12]，若 sp 变成 B-64 而链不跟着挪，硬件就去 [B-76] 读链，
     * 把别处的链值（c0 帧写的 B+128）当成 a2 之类恢复出来 —— 真板签名正是
     * a2 = a1+0x80（128 = 帧大小）、a8 = 带标签返回地址。
     * 删掉后 sp 始终 = B，链 [B-12] = B+128 与真正调用时的 sp 一致。
     * 那个"死区"本来也不需要：被调者 `entry a1,N` 的帧顶 16 字节就是留给
     * 调用者的溢出槽（IDF 的 ppTask 就把 a0 存在 [sp+16]）。 */
    /* §948：诊断把 a9 冲成 0x50001560（RTC 共享内存），而 windowed ABI 下
     * 被调者的 a1 = 调用者的 a9 —— 必须恢复成当前 sp，否则 body 把 RTC 当栈。 */
or      a9, a1, a1
    /* §1030 曾在这里把壳 sp 存进 a13 并在返回后写回 a1，试图绕过 a1 丢失。
     * 实测**更糟**：`retw` 时 PS.CALLINC 变成 0，它没有返回而是落到
     * 紧随其后的 `ill`（epc1=0x42013332 / exccause=0 非法指令），
     * 且 a13 被污染成 0x50001308。已撤回。*/
    l32r    a8, 8b
    callx8  a8
    /* ★★ §998（**根因**）：这里绝对不能再用 **a9** 当诊断的临时基址！
     *
     * 原来写的是 `l32r a9, 11b / s32i a0, a9, 4`，把 a9 冲成了
     * `0x50001560`（RTC 共享内存）。而 windowed ABI 下 **`retw` 一旦触发
     * 窗口欠载，`_win_unf8` 就是拿 a9 当"本帧 sp"去 `[a9-12]` 读帧链**：
     *   _win_unf8: l32e a0, a9, -16 / l32e a1, a9, -12 / ...
     * a9 = 0x50001560 ⇒ 它从 RTC 里读出一堆垃圾当 a0/a1 恢复 ——
     * **正好就是 §986 抢到的现场：a1 = 0、a0 = 0**（而 [真正sp-12] 里
     * 存着正确的 0x3fcc4790，所以我们一直以为"处理器没被调用"）。
     *
     * 实测证据（gdb 硬件断点）：在这个 `retw` 上 `a9 = 0x50001560`、
     * `a1 = 0x3fcc4710`（正确）、`a0 = 0x803822d9`（正确，CALLINC=2），
     * 而它的调用者 ppTask 在 `+0x2d` 处拿到的 a1 = 0。
     *
     * §948 已经发现过同一函数里**前一处** `l32r a9, 11b` 的同类问题
     * 并在后面补了 `or a9,a1,a1`；这**第二处**在 `callx8` 之后、
     * `retw` 之前，被漏掉了。
     *
     * 修法：诊断改用 a8（本壳自己可随意用的 scratch），并在 `retw` 前
     * 重新把 a9 置成本帧 sp，保证任何欠载路径都拿到正确帧链。 */
    l32r    a8, 11b
    s32i    a0, a8, 4
    l32r    a8, 4b
    l32i    a1, a8, 0            /* ★ §1032：从 RTC 取回本壳 sp */            /* 诊断：callx8 之后的 a0 */
    mov.n   a2, a10
    /* ★★★ §1011（据 §1009/§1010 实测）：`retw` 之前把**两个**溢出槽都填好。
     *
     * §1009 从 RTC 坏 sp 记录拿到权威现场：`ps = 0x00060c10` 里 **CALLINC = 2**。
     * `retw` 正常完成会把它清零；保持 2 说明**这条 retw 没完成** —— 它触发了
     * WindowUnderflow，`_win_unf8` 跑了，`rfwu` 把控制直接送回返回地址，
     * 而 ppTask 拿到的 a0/a1 就是处理器从槽里读出来的值。
     * （§1008 之前"处理器没运行"的推论因此作废。）
     *
     * `_win_unf8` 读 `[a9-16]` → a0、`[a9-12]` → a1。实测 ppTask 在 `+0x2d`
     * 拿到的 **a0 = a1 = 0** ⇒ 那两个槽当时都是 0。
     * §999 只补写了 `[-12]`、**漏了 `[-16]`**，所以 a0 仍然是 0 ——
     * 这也解释了为什么 §999 独立看"无效"：它只修好了一半。
     *
     * 这里一次写全两个槽（a8 当 scratch；a10 已被上面的 `mov.n a2,a10` 释放）：
     *   [B-16] = 当前 a0（带 CALLINC 标签的返回地址）
     *   [B-12] = 调用者 sp = B + 128
     * 必须写在 `l32i a0, a1, 16` **之前**，因为那条会覆盖 a0。 */
    movi    a10, 128
    add     a10, a10, a1           /* a10 = B + 128 = 调用者 sp */
    addi    a8, a1, -16
    s32i    a0, a8, 0              /* [B-16] = 返回地址（含 CALLINC 标签）*/
    s32i    a10, a8, 4             /* [B-12] = 调用者 sp */
    memw
    or      a9, a1, a1             /* a9 = 本帧 sp（retw 欠载用）*/
    l32i    a0,  a1, 16
    l32i    a12, a1, 20
    l32i    a13, a1, 24
    l32i    a14, a1, 28
    l32i    a15, a1, 32
    retw


/* ---- s3_win_spill：IDF `window_spill_function`（XCHAL_NUM_AREGS=64）----
 * 语义：把它**调用者的祖先**窗口全部 spill 到各自栈帧；返回后没有祖先窗口
 * 还活在 AR 里。被调用方式（call4/8/12）由 a0 的 bit31:30 判断。
 * 这段和 esp-idf components/xtensa/include/xtensa/coreasm.h 完全一致。 */
    .align  4
    .global s3_win_spill
    .type   s3_win_spill, @function
s3_win_spill:
    entry   a1, 48
    bbci.l  a0, 31, 1f          /* 用 call4 调的 */
    bbsi.l  a0, 30, 2f          /* 用 call12 调的 */
    call4   .Lws52              /* 用 call8 调的：再补一个 call4 */
    retw
1:  call8   .Lws52              /* 用 call4 调的：再补一个 call8 */
    retw
2:  call12  .Lws40              /* 用 call12 调的：可以少转一个 call12 */
    retw

    .align  4
.Lws52:
    entry   a1, 48
    call12  .Lws40
    retw
    .align  4
.Lws40:
    entry   a1, 48
    call12  .Lws28
    retw
    .align  4
.Lws28:
    entry   a1, 48
    call12  .Lws16
    retw
    .align  4
.Lws16:
    entry   a1, 16
    movi    a15, 0
    retw

/* §474 ★★★ `ets_printf` 是 **windowed** 函数，而本仓的 C 代码全部按
 * **call0** 编译 —— 直接 `call0` 调它，`a0` 就不带 CALLINC，ROM 里的
 * `entry a1, 80` 因而**不轮转窗口**；等它内部的 `call8` 把 WINDOWBASE
 * 推上去之后，`retw` 拿着 CALLINC=0 的 `a0` 回退，与 WINDOWBASE 不配套
 * ⇒ **非法指令**。
 *
 * 真板症状（§468 抓到的完整现场）：
 *   exccause=0、epc1 落在 ROM 的 `retw.n`、a0=0、ps.EXCM=1。
 * 这也解释了为什么"大多数 ets_printf 调用看着没问题" —— 只有当调用瞬间
 * WINDOWBASE 恰好是 0 时，CALLINC=0 才碰巧与实践一致。
 *
 * 修法：一层 **call0 可调**的薄壳，内部用 `callx8` 进 ROM 的跳转桩。
 * `ets_printf` 的桩（0x400005d0）是 `l32r a9,[0x400005cc] / jx a9`，
 * 纯跳转、不改 CALLINC，于是 CALLINC=2 一路带到真实实现的
 * `entry a1, 80`。参数 a2..a7 在 call8 前后是重叠的，不需要搬。 */
    /* §479：`s3_ets_printf` 改由 `port/s3/idf_shim.c` 的 C 版提供
     * （windowed 编译），**不再碰 ROM 的 ets_printf**。 */

    /* §478：给 **call0 编译单元**（`console.c` 等）专用的 printf 桥。
     * windowed 目标要求 CALLINC=2，所以必须 `call8` 进去；
     * 参数 a2..a7 在 call8 前后重叠、不用搬（`call8` 轮转后
     * 被调者的 a2..a7 = 我的 a10..a15… 这里要小心！）。
     *
     * call8 的轮转规则（§385 实测）：
     *     callee 的 a0..a7  = caller 的 a8..a15
     *     callee 的 a8..a15 = caller 的 a0..a7
     * ⇒ **callee 的 a2..a7 来自我的 a10..a15**，callee 的 a0 来自我的 a8。
     * 所以要把参数先搬到 a10..a15，并且 a0 会丢（不影响，call8 本来就不写 a0）。
     *
     * 返回值：callee 的 a2 在我的 a10。
     */
    .align 4
16: .word   s3_ets_printf

    .align 4
    .global s3_ets_printf_call0
    .type   s3_ets_printf_call0, @function
s3_ets_printf_call0:
    addi    a1, a1, -48
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    s32i    a10, a1, 20            /* 存原 a10（返回值落点） */
    /* call0 的栈参数在 a1+0 起；call8 之后被调者看到的是 a1+16 起（entry 推 16）。
     * 这里**只支持 0 个栈参数**的调用（fmt + 最多 5 个字面参数都走寄存器），
     * console.c 的调用点全是这种。 */
    mov     a10, a2                /* fmt  → callee 的 a2 */
    mov     a11, a3                /* arg1 → callee 的 a3 */
    mov     a12, a4                /* arg2 → callee 的 a4 */
    mov     a13, a5                /* arg3 → callee 的 a5 */
    mov     a14, a6                /* arg4 → callee 的 a6 */
    mov     a15, a7                /* arg5 → callee 的 a7 */
    l32r    a8, 16b
    /* §955：同属 windowed ABI 的硬性要求 —— 被调者的 a1 = 调用者的 a9。
     * 本壳用 `addi a1,a1,-48` 建帧（不轮转窗口），所以 a9 仍是调用者的值；
     * 不改成当前 sp 的话，s3_ets_printf 的 `entry a1,80` 会把帧开在调用者
     * 栈上，与本壳保存的 a0/a12..a15 直接重叠。 */
    or      a9, a1, a1
    callx8  a8
    mov     a2, a10                /* 返回值 */
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 48
    ret

