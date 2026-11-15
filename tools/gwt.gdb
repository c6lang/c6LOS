set pagination off
set confirm off
set remotetimeout 40
target extended-remote :3333
monitor reset halt
hbreak __wrap_pp_create_task
continue
printf "HIT1 pc=%p a1=%p wb=%d ws=0x%x ps=0x%x\n", $pc, $a1, $windowbase, $windowstart, $ps
continue
printf "HIT2 pc=%p a1=%p wb=%d ws=0x%x ps=0x%x\n", $pc, $a1, $windowbase, $windowstart, $ps
continue
printf "HIT3 pc=%p a1=%p wb=%d ws=0x%x ps=0x%x\n", $pc, $a1, $windowbase, $windowstart, $ps
