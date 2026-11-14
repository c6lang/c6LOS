set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
hbreak _vec_level1
set $n = 0
while 1
  continue
  if $exccause != 4
    printf "\n=== 真异常: exccause=%d\n", $exccause
    printf "epc1=%p  excvaddr=%p\n", $epc1, $excvaddr
    printf "ps=%p  a0=%p  a1=%p  a2=%p  a3=%p\n", $ps, $a0, $a1, $a2, $a3
    loop_break
  end
  set $n = $n + 1
  if $n > 4000
    printf "\n(4000 次中断都没遇到异常)\n"
    loop_break
  end
end
