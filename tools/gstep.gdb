set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break set_chan_freq_hw_init
continue
printf "\n=== 进入 set_chan_freq_hw_init pc=%p a0=%p ps=%p\n", $pc, $a0, $ps
set $i = 0
while $i < 260
  stepi
  printf "%p a0=%p ps=%p\n", $pc, $a0, $ps
  set $i = $i + 1
end
