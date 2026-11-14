set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break set_chan_freq_hw_init
continue
printf "\n=== 进入 pc=%p ps=%p a0=%p\n", $pc, $ps, $a0
set $i = 0
while $i < 300
  si
  printf "STEP %p ps=%p a0=%p\n", $pc, $ps, $a0
  set $i = $i + 1
end
