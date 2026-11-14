set pagination off
set confirm off
target extended-remote :3333
break set_chan_freq_hw_init
break s3_trap
continue
printf "\n=== 停在 set_chan_freq_hw_init pc=%p a0=%p\n", $pc, $a0
delete 1
continue
printf "\n=== 陷阱 pc=%p\n", $pc
info registers pc a0 a1 a2 a3 a4 ps
