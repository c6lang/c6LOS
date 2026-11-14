set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
printf "\n=== 停住 pc=%p\n", $pc
info registers pc a0 a1 a2 ps
bt
x/8i $pc
