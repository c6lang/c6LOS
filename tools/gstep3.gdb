set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break set_chan_freq_hw_init
break register_chipv7_phy
set {int}0x600080C0 = 0xDEADBEEF
continue
printf "\n=== HIT1 pc=%p ps=%p a0=%p\n", $pc, $ps, $a0
delete 2
continue
printf "\n=== HIT2 set_chan_freq_hw_init pc=%p ps=%p a0=%p\n", $pc, $ps, $a0
set $i = 0
while $i < 200
  si
  printf "STEP %p ps=%p a0=%p\n", $pc, $ps, $a0
  set $i = $i + 1
end
