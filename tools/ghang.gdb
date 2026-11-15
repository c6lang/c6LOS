set pagination off
set confirm off
set remotetimeout 40
target extended-remote :3333
monitor halt
printf "\n=== 当前现场 ===\n"
printf "pc = %p\n", $pc
printf "ps = %p\n", $ps
printf "windowbase  = %p\n", $windowbase
printf "windowstart = %p\n", $windowstart
printf "a0=%p a1=%p a2=%p a3=%p\n", $a0, $a1, $a2, $a3
printf "exccause=%d excvaddr=%p epc1=%p\n", $exccause, $excvaddr, $epc1
printf "\n=== 调用栈 ===\n"
bt
printf "\n=== 附近反汇编 ===\n"
x/6i $pc-16
