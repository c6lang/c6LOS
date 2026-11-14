set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
echo \n=== 现在停在哪 ===\n
p $pc
p $a0
p $a1
bt
