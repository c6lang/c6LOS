set pagination off
set confirm off
target extended-remote :3333
break register_chipv7_phy
break set_chan_freq_hw_init
continue
printf "\n=== HIT pc=%p\n", $pc
info registers pc a0 a1 a2 ps
