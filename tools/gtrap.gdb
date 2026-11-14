set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break _vec_level1
continue
printf "\n=== 在异常向量停住 pc=%p\n", $pc
p $epc1
p $exccause
p $excvaddr
p $ps
p $a0
p $a1
