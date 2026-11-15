set pagination off
set confirm off
set remotetimeout 40
target extended-remote :3333
monitor reset halt
hbreak port_task_switch
continue
printf "\n=== 进入 port_task_switch: pc=%p wb=%d ws=0x%x ps=0x%x a1=%p\n", $pc, $windowbase, $windowstart, $ps, $a1
set $i = 0
set $pwb = $windowbase
set $pws = $windowstart
while $i < 60
  stepi
  if $windowbase != $pwb || $windowstart != $pws
    printf "  step %2d pc=%p wb=%d->%d ws=0x%x->0x%x\n", $i, $pc, $pwb, $windowbase, $pws, $windowstart
    set $pwb = $windowbase
    set $pws = $windowstart
  end
  set $i = $i + 1
end
printf "\n=== 60 步之后: pc=%p wb=%d ws=0x%x\n", $pc, $windowbase, $windowstart
