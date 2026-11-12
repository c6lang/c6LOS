/* port/rv32/stub.c —— 只给不链接调度器的 rv32 目标（如 hello）补弱符号。
 *
 * port/rv32/console.c 的 port_idle() 会调 os_sched_tick()，
 * 但 hello 不需要调度器。这里给一个 weak 定义：
 *   - hello：弱定义生效，链接通过；
 *   - sched/tick：kernel/tick.c6 的强定义覆盖它，行为不变。 */

__attribute__((weak)) void os_sched_tick(void)
{
}
