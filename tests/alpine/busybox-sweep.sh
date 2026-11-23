#!/bin/sh
# tests/alpine/busybox-sweep.sh —— 真实 Alpine 3.20 busybox 的全量 applet 验收。
#
# 进 ISO 的 initramfs 里（Makefile 的 x86-alpine-busybox 把它拷到 /etc/），
# 由 tests/t_iso_alpine.c6 的 C6OS_ALPINE_BUSYBOX 分支用 `sh` 执行。
#
# 顺序是 **F 段先、A 段后**。A 段是 608 次 fork + 动态装载的进程风暴，跑在
# 前面会把内核资源压到极限（实测 F 段若排在 A 后，跑到一半 open 就 ENOMEM）；
# F 段结果因此单独先取。
#
# F 段（**功能性验证**）：在 /data（FAT16 可写盘）上造数据，逐条命令验证真实
# 输出，打印 OK|<name> / NO|<name>|rc=<n>。全程不用管道：musl ash 的外部命令
# 管道在本内核上仍有已知限制（见 docs/linux-abi-complete.md），用管道测不出
# applet 本身的好坏。F 段也顺带覆盖 `cmd > file` 这条重定向路径 —— 它曾经把
# 输出全写到串口（sys_write 只看 fd 号 1/2），所以每条命令的 stdout 都落到
# /data/o 再用 grep/cmp 比对，正好能抓住这一类 ABI 退化。
#
# A 段（**全量派发扫描**）：minirootfs 里 busybox 的每个 applet 跑两种形式各
# 一次，输出 `XA|applet|rc`（busybox 按 argv[1] 派发）和
# `XB|applet|rc`（按 argv[0] / 符号链接派发）：
#
#     /bin/busybox <applet> --help
#     <applet 在 rootfs 里的绝对路径> --help
#
# stdin 一律接 /dev/null：万一个别 applet 不认 --help、转去读 stdin，也会立刻
# 读到 EOF 退出，扫描不会挂住（同一份 busybox 在宿主上已验证 304 个全部返回）。
# 期望值见 tests/alpine/busybox-baseline.tsv（在宿主 Linux 上用 qemu-user 跑
# 同一份 minirootfs 得到）。A 段 304 行由下面这条命令生成，改动前请重跑：
#
#     qemu-x86_64 -L <minirootfs> <minirootfs>/bin/busybox --list
#
# 收尾：A 段每 25 个 applet 打一行 `# ACHUNK n`，方便中断时定位。

# ---- F 段：功能性验证 ----
# 每条命令把 stdout 落到 /tmp/o，再用 grep -qx（整行相等）/ cmp（整文件相等）
# 比对期望值。不建管道：musl ash 的外部命令管道在本内核上还有已知限制
# （docs/linux-abi-complete.md 的 M28 遗留），用管道测不出 applet 本身的好坏。
# 工作目录用 /tmp（ramfs）而不是 /data（FAT16）：本轮验证发现本内核
# `write(2)` 写 FAT 文件返回 EINVAL，把数据都放 /data 会把每个 applet 的
# 输出比对一起带偏，测不出 applet 本身；FAT 只读/写状态单独在 F5 记录。
SH=/bin/sh

ok() { echo "OK|$1"; }
no() {
    echo "NO|$1|rc=$2"
    # 失败时把实际输出的前两行带出来（"O|" 前缀，矩阵工具不看），便于定位
    if [ -s /tmp/o ]; then
        /bin/sed -n '1,2p' /tmp/o 2>/dev/null | /bin/sed 's/^/O|/' 2>/dev/null
    fi
}

okeq() {
    n="$1"; w="$2"; shift 2
    "$@" </dev/null >/tmp/o 2>/dev/null
    r=$?
    if [ "$r" = "0" ] && grep -qx "$w" /tmp/o 2>/dev/null; then ok "$n"; else no "$n" "$r"; fi
}
okhas() {
    n="$1"; w="$2"; shift 2
    "$@" </dev/null >/tmp/o 2>/dev/null
    r=$?
    if [ "$r" = "0" ] && grep -q "$w" /tmp/o 2>/dev/null; then ok "$n"; else no "$n" "$r"; fi
}
okeqsh() {   # name, want, script —— 需要重定向/复合语句时用（sh -c）
    n="$1"; w="$2"; s="$3"
    "$SH" -c "$s" </dev/null >/tmp/o 2>/dev/null
    r=$?
    if [ "$r" = "0" ] && grep -qx "$w" /tmp/o 2>/dev/null; then ok "$n"; else no "$n" "$r"; fi
}
okhassh() {
    n="$1"; w="$2"; s="$3"
    "$SH" -c "$s" </dev/null >/tmp/o 2>/dev/null
    r=$?
    if [ "$r" = "0" ] && grep -q "$w" /tmp/o 2>/dev/null; then ok "$n"; else no "$n" "$r"; fi
}
okcmp() {    # name, want-file, cmd...
    n="$1"; w="$2"; shift 2
    "$@" </dev/null >/tmp/o 2>/dev/null
    r=$?
    if [ "$r" = "0" ] && cmp -s /tmp/o "$w" 2>/dev/null; then ok "$n"; else no "$n" "$r"; fi
}
okrc() {     # name, want-rc, cmd...
    n="$1"; w="$2"; shift 2
    "$@" </dev/null >/tmp/o 2>/dev/null
    r=$?
    if [ "$r" = "$w" ]; then ok "$n"; else no "$n" "$r"; fi
}
ffile() { if [ -f "$2" ]; then ok "$1"; else no "$1" "nofile"; fi; }
fdir()  { if [ -d "$2" ]; then ok "$1"; else no "$1" "nodir"; fi; }
fnone() { if [ -e "$2" ]; then no "$1" "still-there"; else ok "$1"; fi; }
fsize() { if [ -s "$2" ]; then ok "$1"; else no "$1" "empty"; fi; }

# ---- F0：造数据（/tmp 是 FAT16 可写盘）----
echo "# F-SETUP"
PATH=/bin:/usr/bin:/sbin:/usr/sbin; export PATH
HOME=/root; export HOME
printf 'hello\nworld\n'          > /tmp/t.txt         # 2 行
printf 'hello'                   > /tmp/hello.txt     # 无换行
printf 'a:b:c\n'                 > /tmp/colon.txt
printf 'b\na\nc\n'               > /tmp/uns.txt
printf 'dup\ndup\nuniq\n'        > /tmp/dup.txt
printf 'a b c\n1 2 3\n'          > /tmp/cols.txt
printf 'lower\n'                 > /tmp/lower.txt
printf '1\n2\n3\n4\n5\n'         > /tmp/nums.txt
printf 'p1\np2\n'                > /tmp/files.txt
printf 'a\nc\n'                  > /tmp/a1.txt
printf 'b\nd\n'                  > /tmp/b2.txt
printf 'aaaa\n'                  > /tmp/a4.txt
printf 'x\ty\n'                  > /tmp/tab.txt
# 期望值文件
printf 'a\nb\nc\n'               > /tmp/want.sort
printf 'dup\nuniq\n'             > /tmp/want.uniq
printf '1\n2\n3\n4\n5\n'         > /tmp/want.seq
printf 'b\n2\n'                  > /tmp/want.awk
printf 'a-b-c\n'                 > /tmp/want.sed
printf 'world\nhello\n'          > /tmp/want.tac
printf 'a\tb\nc\td\n'            > /tmp/want.paste
printf 'aa\naa\n'                > /tmp/want.fold
printf 'olleh\ndlrow\n'          > /tmp/want.rev
printf 'x   y\n'                 > /tmp/want.expand
printf 'p1\np2\n'                > /tmp/want.xargs

# ---- F1：文件与元数据 ----
echo "# F1-file"
okeq  echo       hello   /bin/echo hello
okeq  printf     x-7     /usr/bin/printf '%s-%d\n' x 7
okeq  cat        hello   /bin/cat /tmp/hello.txt
okhas ls         busybox /bin/ls /bin
okhas ls-l       busybox /bin/ls -l /bin/busybox
okeqsh pwd       /tmp    'cd /tmp && /bin/pwd'
okeq  basename   b.txt   /usr/bin/basename /a/b.txt
okeq  dirname    /a      /usr/bin/dirname /a/b.txt
okeq  realpath   /bin/busybox /usr/bin/realpath /bin/sh
okeq  readlink   /bin/busybox /usr/bin/readlink /bin/sh
okhas stat       /tmp/hello.txt /bin/stat /tmp/hello.txt
okrc  touch      0       /bin/touch /tmp/touch.x
ffile touch-file         /tmp/touch.x
okrc  mkdir      0       /bin/mkdir /tmp/d1
fdir  mkdir-dir          /tmp/d1
okrc  rmdir      0       /bin/rmdir /tmp/d1
fnone rmdir-gone         /tmp/d1
okrc  chmod      0       /bin/chmod 600 /tmp/t.txt
okhas chmod-mode 0600    /bin/stat /tmp/t.txt
okrc  chmod2     0       /bin/chmod 644 /tmp/t.txt
okhas chmod-mode2 0644   /bin/stat /tmp/t.txt
okrc  cp         0       /bin/cp /tmp/hello.txt /tmp/cp.txt
okcmp cp-content /tmp/hello.txt /bin/cat /tmp/cp.txt
okrc  mv         0       /bin/mv /tmp/cp.txt /tmp/mv.txt
ffile mv-file            /tmp/mv.txt
okrc  rm         0       /bin/rm /tmp/mv.txt
fnone rm-gone            /tmp/mv.txt
okrc  ln-s       0       /bin/ln -s /tmp/hello.txt /tmp/lnk
okeq  ln-read    hello   /bin/cat /tmp/lnk
okrc  install    0       /usr/bin/install -m 644 /tmp/hello.txt /tmp/inst.txt
okcmp install-content /tmp/hello.txt /bin/cat /tmp/inst.txt
okrc  mktemp     0       /bin/mktemp -p /tmp
okrc  sync       0       /bin/sync
okrc  true       0       /bin/true
okrc  false      1       /bin/false

# ---- F2：文本处理 ----
echo "# F2-text"
okeq  head-n1    hello   /usr/bin/head -n 1 /tmp/t.txt
okeq  head-c3    hel     /usr/bin/head -c 3 /tmp/hello.txt
okeq  tail-n1    world   /usr/bin/tail -n 1 /tmp/t.txt
okeq  tail-c3    llo     /usr/bin/tail -c 3 /tmp/hello.txt
okcmp tac       /tmp/want.tac  /usr/bin/tac /tmp/t.txt
okeqsh wc-l      2       'wc -l < /tmp/t.txt'
okeqsh wc-w      2       'wc -w < /tmp/t.txt'
okeqsh wc-c      12      'wc -c < /tmp/t.txt'
okhas nl        1       /usr/bin/nl /tmp/t.txt
okcmp rev      /tmp/want.rev  /bin/rev /tmp/t.txt
okeq  cut-d      b       /usr/bin/cut -d: -f2 /tmp/colon.txt
okeq  cut-c      a       /usr/bin/cut -c1 /tmp/colon.txt
okeqsh tr-upper LOWER   'tr a-z A-Z < /tmp/lower.txt'
okeqsh tr-del   abc     'tr -d ":" < /tmp/colon.txt'
okcmp sort     /tmp/want.sort  /usr/bin/sort /tmp/uns.txt
okcmp sort-u   /tmp/want.uniq  /usr/bin/sort -u /tmp/dup.txt
okcmp sort-n   /tmp/want.seq   /usr/bin/sort -n /tmp/nums.txt
okcmp uniq     /tmp/want.uniq  /usr/bin/uniq /tmp/dup.txt
okhas uniq-c   '2 dup'  /usr/bin/uniq -c /tmp/dup.txt
okeq  grep      hello   /bin/grep hello /tmp/t.txt
okeq  grep-v    world   /bin/grep -v hello /tmp/t.txt
okeq  grep-c    1       /bin/grep -c hello /tmp/t.txt
okeq  grep-i    hello   /bin/grep -i HELLO /tmp/t.txt
okeq  grep-n    1:hello /bin/grep -n hello /tmp/t.txt
okeq  egrep     a:b:c   /bin/egrep 'a.*c' /tmp/colon.txt
okeq  fgrep     a:b:c   /bin/fgrep 'a:b' /tmp/colon.txt
okcmp sed     /tmp/want.sed  /bin/sed 's/:/-/g' /tmp/colon.txt
okeq  sed-n     hello   /bin/sed -n '1p' /tmp/t.txt
okcmp awk     /tmp/want.awk  /usr/bin/awk '{print $2}' /tmp/cols.txt
okeq  awk-F     b       /usr/bin/awk -F: '{print $2}' /tmp/colon.txt
okrc  diff      0       /usr/bin/diff /tmp/t.txt /tmp/t.txt
okrc  cmp       0       /usr/bin/cmp -s /tmp/t.txt /tmp/t.txt
okcmp paste   /tmp/want.paste /usr/bin/paste /tmp/a1.txt /tmp/b2.txt
okcmp fold    /tmp/want.fold  /usr/bin/fold -w 2 /tmp/a4.txt
okcmp expand  /tmp/want.expand /usr/bin/expand -t 4 /tmp/tab.txt
okcmp seq     /tmp/want.seq   /usr/bin/seq 1 5
okeq  expr      5       /usr/bin/expr 2 + 3
okeq  factor    '12: 2 2 3' /usr/bin/factor 12
okeq  base64    aGVsbG8= /bin/base64 /tmp/hello.txt
okhas md5sum    5d41402abc4b2a76b9719d911017c592 /usr/bin/md5sum /tmp/hello.txt
okhas sha256sum 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 /usr/bin/sha256sum /tmp/hello.txt
okeq  cksum     '3287646509 5 /tmp/hello.txt' /usr/bin/cksum /tmp/hello.txt
okhas od        'h   e'   /usr/bin/od -An -c /tmp/hello.txt
okhas hexdump   '68 65 6c 6c 6f' /usr/bin/hexdump -C /tmp/hello.txt
okhas xxd       '6865 6c6c 6f' /usr/bin/xxd /tmp/hello.txt
okhas strings   busybox /usr/bin/strings /bin/busybox
okrc  split     0       /usr/bin/split -l 1 /tmp/t.txt /tmp/sp-
ffile split-part        /tmp/sp-aa
okrc  shuf      0       /usr/bin/shuf /tmp/nums.txt
okrc  tree      0       /usr/bin/tree /tmp/a1.txt
okcmp xargs   /tmp/want.xargs /bin/sh -c 'xargs -n1 echo < /tmp/files.txt'
okeqsh tee     hello   'tee /tmp/tee.out < /tmp/hello.txt'
fsize tee-file          /tmp/tee.out
okhas find      /tmp/hello.txt /usr/bin/find /tmp -name hello.txt
okhas du        /tmp/hello.txt /usr/bin/du -k /tmp/hello.txt
okhas df        /tmp   /bin/df -h
okeq  printf-o  abc     /usr/bin/printf 'abc\n'

# ---- F3：进程与系统 ----
echo "# F3-system"
okhas uname-a   Linux           /bin/uname -a
okeq  uname-m   x86_64          /bin/uname -m
okeq  uname-r   6.1.0-c6los     /bin/uname -r
okeq  id-u      0               /usr/bin/id -u
okhas id        uid=0           /usr/bin/id
okeq  whoami    root            /usr/bin/whoami
okhas groups    root            /usr/bin/groups
okeq  hostname  c6los           /bin/hostname
okhassh date-year '^[0-9][0-9][0-9][0-9]$' 'date +%Y'
okhas env       PATH=           /usr/bin/env
okhas printenv  /bin            /bin/printenv PATH
okhassh nproc   '^[0-9]'        'nproc'
okhas free      Mem             /usr/bin/free
okhas uptime    up              /usr/bin/uptime
okhas ps        PID             /bin/ps
okhas top       CPU             /usr/bin/timeout 15 /usr/bin/top -b -n 1
okhas pstree    '/'             /usr/bin/pstree
okhassh pidof   '[0-9]'         'pidof init'
okhassh pgrep   '[0-9]'         'pgrep init'
okeq  nice      ok              /bin/nice -n 5 /bin/echo ok
okeq  nohup     ok              /usr/bin/nohup /bin/echo ok
okeq  timeout   ok              /usr/bin/timeout 2 /bin/echo ok
okrc  timeout-124 124           /usr/bin/timeout 1 /bin/sleep 5
okrc  sleep     0               /bin/sleep 0.2
okrc  usleep    0               /bin/usleep 200000
okhas kill-l    INT             /bin/kill -l
okhas mount     /tmp           /bin/mount
okeq  dd        hello           /bin/dd if=/tmp/hello.txt bs=5 count=1
okrc  tar-c     0               /usr/bin/timeout 15 /bin/tar -cf /tmp/t.tar /tmp/hello.txt
okhas tar-t     hello.txt       /usr/bin/timeout 15 /bin/tar -tf /tmp/t.tar
okrc  mkdir-untar 0             /bin/mkdir /tmp/untar
okrc  tar-x     0               /usr/bin/timeout 15 /bin/tar -xf /tmp/t.tar -C /tmp/untar
ffile tar-extracted             /tmp/untar/tmp/hello.txt
okrc  gzip-c    0               /usr/bin/timeout 20 /bin/sh -c 'gzip -c /tmp/hello.txt > /tmp/hello.gz'
fsize gzip-file                 /tmp/hello.gz
okeq  gzip-dc   hello           /usr/bin/timeout 20 /bin/sh -c 'gzip -dc /tmp/hello.gz'
okrc  bzip2-c   0               /usr/bin/timeout 20 /bin/sh -c 'bzip2 -c /tmp/hello.txt > /tmp/hello.bz2'
fsize bzip2-file                /tmp/hello.bz2
okeq  bzip2-dc  hello           /usr/bin/timeout 20 /bin/sh -c 'bzip2 -dc /tmp/hello.bz2'
okhas lsof      sh              /usr/bin/lsof
okhas mpstat    CPU             /usr/bin/timeout 15 /bin/mpstat 1 1
okhas ifconfig  eth0            /sbin/ifconfig
okhas ping      received        /bin/ping -c 1 -W 2 127.0.0.1
okhas nslookup  Name            /usr/bin/timeout 20 /usr/bin/nslookup qq.com
okhas netstat   10.0.2          /bin/netstat -rn
okhas route     10.0.2          /sbin/route -n

# ---- F4：错误路径（rc 与 errno 语义）----
echo "# F4-error"
okrc  err-cat   1       /bin/cat /tmp/nonexist
okrc  err-ls    1       /bin/ls /nonexist
okrc  err-grep  1       /bin/grep nomatch /tmp/t.txt
okrc  err-mkdir 1       /bin/mkdir /tmp/t.txt
okrc  err-rm    1       /bin/rm /tmp/nonexist
okrc  err-chmod 1       /bin/chmod 999 /tmp/t.txt
okrcsh err-cd   1       'cd /nonexist'
okrcsh err-exec 127     '/tmp/nonexist-cmd'
okrc  err-sed   1       /bin/sed 's/[/x/' /tmp/t.txt
okrc  err-awk   1       /usr/bin/awk 'BEGIN{' /tmp/t.txt
okrc  test-true 0       /usr/bin/test 1 -eq 1
okrc  test-false 1      /usr/bin/test 1 -eq 2
okrc  bracket-true 0    /usr/bin/[ -f /tmp/t.txt ]
okrc  bracket-false 1   /usr/bin/[ 1 -gt 2 ]
okrcsh exit-code 3      'exit 3'
okrcsh sh-c      0       'echo hi'

# ---- F5：/data FAT16 盘（只读操作 + 已知写缺口的状态记录）----
# 这些行不参与 OK/NO 统计：F 段的 OK/NO 只回答“applet 本身对不对”，
# /data 上的一切属于内核文件系统栈。本内核当前 `write(2)` 到 FAT 文件返回
# EINVAL（见报告），所以写类只打 FB| 状态行，供报告引用。
echo "# F5:FAT-disk"
/bin/ls /data >/dev/null 2>&1 && echo "FB|fat-ls|0" || echo "FB|fat-ls|$?"
/bin/stat /data >/dev/null 2>&1 && echo "FB|fat-stat|0" || echo "FB|fat-stat|$?"
/bin/df /data >/dev/null 2>&1 && echo "FB|fat-df|0" || echo "FB|fat-df|$?"
/bin/ls /data/*.txt >/dev/null 2>&1 && echo "FB|fat-glob|0" || echo "FB|fat-glob|$?"
/bin/touch /data/fb-touch.x >/dev/null 2>&1 && echo "FB|fat-touch|0" || echo "FB|fat-touch|$?"
/bin/mkdir /data/fb-d >/dev/null 2>&1 && echo "FB|fat-mkdir|0" || echo "FB|fat-mkdir|$?"
/bin/rm /data/fb-touch.x >/dev/null 2>&1 && echo "FB|fat-rm|0" || echo "FB|fat-rm|$?"
/bin/rmdir /data/fb-d >/dev/null 2>&1 && echo "FB|fat-rmdir|0" || echo "FB|fat-rmdir|$?"
# 写：先清空再写 4 字节，读回来必须相等
printf 'wxyz\n' > /data/fb-w.txt 2>/dev/null && echo "FB|fat-write|0" || echo "FB|fat-write|$?"
/bin/grep -qx wxyz /data/fb-w.txt 2>/dev/null && echo "FB|fat-write-readback|0" || echo "FB|fat-write-readback|1"
/bin/rm -f /data/fb-w.txt >/dev/null 2>&1
echo "# F5-done"

echo "# F-DONE"

# ---- A 段：全量派发扫描（304 个 applet × 2）----
echo "# ACHUNK 1"
/bin/busybox [ --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox [ --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|[|$r|$r2"; r=$r2; fi; echo "XA|[|$r"
/usr/bin/[ --help </dev/null >/dev/null 2>&1; echo "XB|[|$?"
/bin/busybox [[ --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox [[ --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|[[|$r|$r2"; r=$r2; fi; echo "XA|[[|$r"
/usr/bin/[[ --help </dev/null >/dev/null 2>&1; echo "XB|[[|$?"
/bin/busybox acpid --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox acpid --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|acpid|$r|$r2"; r=$r2; fi; echo "XA|acpid|$r"
/sbin/acpid --help </dev/null >/dev/null 2>&1; echo "XB|acpid|$?"
/bin/busybox add-shell --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox add-shell --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|add-shell|$r|$r2"; r=$r2; fi; echo "XA|add-shell|$r"
/usr/sbin/add-shell --help </dev/null >/dev/null 2>&1; echo "XB|add-shell|$?"
/bin/busybox addgroup --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox addgroup --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|addgroup|$r|$r2"; r=$r2; fi; echo "XA|addgroup|$r"
/usr/sbin/addgroup --help </dev/null >/dev/null 2>&1; echo "XB|addgroup|$?"
/bin/busybox adduser --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox adduser --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|adduser|$r|$r2"; r=$r2; fi; echo "XA|adduser|$r"
/usr/sbin/adduser --help </dev/null >/dev/null 2>&1; echo "XB|adduser|$?"
/bin/busybox adjtimex --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox adjtimex --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|adjtimex|$r|$r2"; r=$r2; fi; echo "XA|adjtimex|$r"
/sbin/adjtimex --help </dev/null >/dev/null 2>&1; echo "XB|adjtimex|$?"
/bin/busybox arch --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox arch --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|arch|$r|$r2"; r=$r2; fi; echo "XA|arch|$r"
/bin/arch --help </dev/null >/dev/null 2>&1; echo "XB|arch|$?"
/bin/busybox arp --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox arp --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|arp|$r|$r2"; r=$r2; fi; echo "XA|arp|$r"
/sbin/arp --help </dev/null >/dev/null 2>&1; echo "XB|arp|$?"
/bin/busybox arping --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox arping --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|arping|$r|$r2"; r=$r2; fi; echo "XA|arping|$r"
/usr/sbin/arping --help </dev/null >/dev/null 2>&1; echo "XB|arping|$?"
/bin/busybox ash --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ash --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ash|$r|$r2"; r=$r2; fi; echo "XA|ash|$r"
/bin/ash --help </dev/null >/dev/null 2>&1; echo "XB|ash|$?"
/bin/busybox awk --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox awk --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|awk|$r|$r2"; r=$r2; fi; echo "XA|awk|$r"
/usr/bin/awk --help </dev/null >/dev/null 2>&1; echo "XB|awk|$?"
/bin/busybox base64 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox base64 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|base64|$r|$r2"; r=$r2; fi; echo "XA|base64|$r"
/bin/base64 --help </dev/null >/dev/null 2>&1; echo "XB|base64|$?"
/bin/busybox basename --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox basename --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|basename|$r|$r2"; r=$r2; fi; echo "XA|basename|$r"
/usr/bin/basename --help </dev/null >/dev/null 2>&1; echo "XB|basename|$?"
/bin/busybox bbconfig --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox bbconfig --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|bbconfig|$r|$r2"; r=$r2; fi; echo "XA|bbconfig|$r"
/bin/bbconfig --help </dev/null >/dev/null 2>&1; echo "XB|bbconfig|$?"
/bin/busybox bc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox bc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|bc|$r|$r2"; r=$r2; fi; echo "XA|bc|$r"
/usr/bin/bc --help </dev/null >/dev/null 2>&1; echo "XB|bc|$?"
/bin/busybox beep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox beep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|beep|$r|$r2"; r=$r2; fi; echo "XA|beep|$r"
/usr/bin/beep --help </dev/null >/dev/null 2>&1; echo "XB|beep|$?"
/bin/busybox blkdiscard --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox blkdiscard --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|blkdiscard|$r|$r2"; r=$r2; fi; echo "XA|blkdiscard|$r"
/usr/bin/blkdiscard --help </dev/null >/dev/null 2>&1; echo "XB|blkdiscard|$?"
/bin/busybox blkid --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox blkid --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|blkid|$r|$r2"; r=$r2; fi; echo "XA|blkid|$r"
/sbin/blkid --help </dev/null >/dev/null 2>&1; echo "XB|blkid|$?"
/bin/busybox blockdev --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox blockdev --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|blockdev|$r|$r2"; r=$r2; fi; echo "XA|blockdev|$r"
/sbin/blockdev --help </dev/null >/dev/null 2>&1; echo "XB|blockdev|$?"
/bin/busybox brctl --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox brctl --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|brctl|$r|$r2"; r=$r2; fi; echo "XA|brctl|$r"
/usr/sbin/brctl --help </dev/null >/dev/null 2>&1; echo "XB|brctl|$?"
/bin/busybox bunzip2 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox bunzip2 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|bunzip2|$r|$r2"; r=$r2; fi; echo "XA|bunzip2|$r"
/usr/bin/bunzip2 --help </dev/null >/dev/null 2>&1; echo "XB|bunzip2|$?"
/bin/busybox bzcat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox bzcat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|bzcat|$r|$r2"; r=$r2; fi; echo "XA|bzcat|$r"
/usr/bin/bzcat --help </dev/null >/dev/null 2>&1; echo "XB|bzcat|$?"
/bin/busybox bzip2 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox bzip2 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|bzip2|$r|$r2"; r=$r2; fi; echo "XA|bzip2|$r"
/usr/bin/bzip2 --help </dev/null >/dev/null 2>&1; echo "XB|bzip2|$?"
/bin/busybox cal --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cal --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cal|$r|$r2"; r=$r2; fi; echo "XA|cal|$r"
/usr/bin/cal --help </dev/null >/dev/null 2>&1; echo "XB|cal|$?"
echo "# ACHUNK 2"
/bin/busybox cat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cat|$r|$r2"; r=$r2; fi; echo "XA|cat|$r"
/bin/cat --help </dev/null >/dev/null 2>&1; echo "XB|cat|$?"
/bin/busybox chattr --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chattr --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chattr|$r|$r2"; r=$r2; fi; echo "XA|chattr|$r"
/bin/chattr --help </dev/null >/dev/null 2>&1; echo "XB|chattr|$?"
/bin/busybox chgrp --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chgrp --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chgrp|$r|$r2"; r=$r2; fi; echo "XA|chgrp|$r"
/bin/chgrp --help </dev/null >/dev/null 2>&1; echo "XB|chgrp|$?"
/bin/busybox chmod --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chmod --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chmod|$r|$r2"; r=$r2; fi; echo "XA|chmod|$r"
/bin/chmod --help </dev/null >/dev/null 2>&1; echo "XB|chmod|$?"
/bin/busybox chown --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chown --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chown|$r|$r2"; r=$r2; fi; echo "XA|chown|$r"
/bin/chown --help </dev/null >/dev/null 2>&1; echo "XB|chown|$?"
/bin/busybox chpasswd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chpasswd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chpasswd|$r|$r2"; r=$r2; fi; echo "XA|chpasswd|$r"
/usr/sbin/chpasswd --help </dev/null >/dev/null 2>&1; echo "XB|chpasswd|$?"
/bin/busybox chroot --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chroot --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chroot|$r|$r2"; r=$r2; fi; echo "XA|chroot|$r"
/usr/sbin/chroot --help </dev/null >/dev/null 2>&1; echo "XB|chroot|$?"
/bin/busybox chvt --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox chvt --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|chvt|$r|$r2"; r=$r2; fi; echo "XA|chvt|$r"
/usr/bin/chvt --help </dev/null >/dev/null 2>&1; echo "XB|chvt|$?"
/bin/busybox cksum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cksum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cksum|$r|$r2"; r=$r2; fi; echo "XA|cksum|$r"
/usr/bin/cksum --help </dev/null >/dev/null 2>&1; echo "XB|cksum|$?"
/bin/busybox clear --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox clear --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|clear|$r|$r2"; r=$r2; fi; echo "XA|clear|$r"
/usr/bin/clear --help </dev/null >/dev/null 2>&1; echo "XB|clear|$?"
/bin/busybox cmp --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cmp --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cmp|$r|$r2"; r=$r2; fi; echo "XA|cmp|$r"
/usr/bin/cmp --help </dev/null >/dev/null 2>&1; echo "XB|cmp|$?"
/bin/busybox comm --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox comm --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|comm|$r|$r2"; r=$r2; fi; echo "XA|comm|$r"
/usr/bin/comm --help </dev/null >/dev/null 2>&1; echo "XB|comm|$?"
/bin/busybox cp --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cp --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cp|$r|$r2"; r=$r2; fi; echo "XA|cp|$r"
/bin/cp --help </dev/null >/dev/null 2>&1; echo "XB|cp|$?"
/bin/busybox cpio --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cpio --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cpio|$r|$r2"; r=$r2; fi; echo "XA|cpio|$r"
/usr/bin/cpio --help </dev/null >/dev/null 2>&1; echo "XB|cpio|$?"
/bin/busybox crond --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox crond --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|crond|$r|$r2"; r=$r2; fi; echo "XA|crond|$r"
/usr/sbin/crond --help </dev/null >/dev/null 2>&1; echo "XB|crond|$?"
/bin/busybox crontab --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox crontab --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|crontab|$r|$r2"; r=$r2; fi; echo "XA|crontab|$r"
/usr/bin/crontab --help </dev/null >/dev/null 2>&1; echo "XB|crontab|$?"
/bin/busybox cryptpw --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cryptpw --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cryptpw|$r|$r2"; r=$r2; fi; echo "XA|cryptpw|$r"
/usr/bin/cryptpw --help </dev/null >/dev/null 2>&1; echo "XB|cryptpw|$?"
/bin/busybox cut --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox cut --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|cut|$r|$r2"; r=$r2; fi; echo "XA|cut|$r"
/usr/bin/cut --help </dev/null >/dev/null 2>&1; echo "XB|cut|$?"
/bin/busybox date --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox date --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|date|$r|$r2"; r=$r2; fi; echo "XA|date|$r"
/bin/date --help </dev/null >/dev/null 2>&1; echo "XB|date|$?"
/bin/busybox dc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dc|$r|$r2"; r=$r2; fi; echo "XA|dc|$r"
/usr/bin/dc --help </dev/null >/dev/null 2>&1; echo "XB|dc|$?"
/bin/busybox dd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dd|$r|$r2"; r=$r2; fi; echo "XA|dd|$r"
/bin/dd --help </dev/null >/dev/null 2>&1; echo "XB|dd|$?"
/bin/busybox deallocvt --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox deallocvt --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|deallocvt|$r|$r2"; r=$r2; fi; echo "XA|deallocvt|$r"
/usr/bin/deallocvt --help </dev/null >/dev/null 2>&1; echo "XB|deallocvt|$?"
/bin/busybox delgroup --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox delgroup --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|delgroup|$r|$r2"; r=$r2; fi; echo "XA|delgroup|$r"
/usr/sbin/delgroup --help </dev/null >/dev/null 2>&1; echo "XB|delgroup|$?"
/bin/busybox deluser --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox deluser --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|deluser|$r|$r2"; r=$r2; fi; echo "XA|deluser|$r"
/usr/sbin/deluser --help </dev/null >/dev/null 2>&1; echo "XB|deluser|$?"
/bin/busybox depmod --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox depmod --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|depmod|$r|$r2"; r=$r2; fi; echo "XA|depmod|$r"
/sbin/depmod --help </dev/null >/dev/null 2>&1; echo "XB|depmod|$?"
echo "# ACHUNK 3"
/bin/busybox df --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox df --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|df|$r|$r2"; r=$r2; fi; echo "XA|df|$r"
/bin/df --help </dev/null >/dev/null 2>&1; echo "XB|df|$?"
/bin/busybox diff --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox diff --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|diff|$r|$r2"; r=$r2; fi; echo "XA|diff|$r"
/usr/bin/diff --help </dev/null >/dev/null 2>&1; echo "XB|diff|$?"
/bin/busybox dirname --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dirname --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dirname|$r|$r2"; r=$r2; fi; echo "XA|dirname|$r"
/usr/bin/dirname --help </dev/null >/dev/null 2>&1; echo "XB|dirname|$?"
/bin/busybox dmesg --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dmesg --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dmesg|$r|$r2"; r=$r2; fi; echo "XA|dmesg|$r"
/bin/dmesg --help </dev/null >/dev/null 2>&1; echo "XB|dmesg|$?"
/bin/busybox dnsdomainname --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dnsdomainname --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dnsdomainname|$r|$r2"; r=$r2; fi; echo "XA|dnsdomainname|$r"
/bin/dnsdomainname --help </dev/null >/dev/null 2>&1; echo "XB|dnsdomainname|$?"
/bin/busybox dos2unix --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dos2unix --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dos2unix|$r|$r2"; r=$r2; fi; echo "XA|dos2unix|$r"
/usr/bin/dos2unix --help </dev/null >/dev/null 2>&1; echo "XB|dos2unix|$?"
/bin/busybox du --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox du --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|du|$r|$r2"; r=$r2; fi; echo "XA|du|$r"
/usr/bin/du --help </dev/null >/dev/null 2>&1; echo "XB|du|$?"
/bin/busybox dumpkmap --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox dumpkmap --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|dumpkmap|$r|$r2"; r=$r2; fi; echo "XA|dumpkmap|$r"
/bin/dumpkmap --help </dev/null >/dev/null 2>&1; echo "XB|dumpkmap|$?"
/bin/busybox echo --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox echo --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|echo|$r|$r2"; r=$r2; fi; echo "XA|echo|$r"
/bin/echo --help </dev/null >/dev/null 2>&1; echo "XB|echo|$?"
/bin/busybox egrep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox egrep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|egrep|$r|$r2"; r=$r2; fi; echo "XA|egrep|$r"
/bin/egrep --help </dev/null >/dev/null 2>&1; echo "XB|egrep|$?"
/bin/busybox eject --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox eject --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|eject|$r|$r2"; r=$r2; fi; echo "XA|eject|$r"
/usr/bin/eject --help </dev/null >/dev/null 2>&1; echo "XB|eject|$?"
/bin/busybox env --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox env --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|env|$r|$r2"; r=$r2; fi; echo "XA|env|$r"
/usr/bin/env --help </dev/null >/dev/null 2>&1; echo "XB|env|$?"
/bin/busybox ether-wake --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ether-wake --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ether-wake|$r|$r2"; r=$r2; fi; echo "XA|ether-wake|$r"
/usr/sbin/ether-wake --help </dev/null >/dev/null 2>&1; echo "XB|ether-wake|$?"
/bin/busybox expand --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox expand --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|expand|$r|$r2"; r=$r2; fi; echo "XA|expand|$r"
/usr/bin/expand --help </dev/null >/dev/null 2>&1; echo "XB|expand|$?"
/bin/busybox expr --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox expr --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|expr|$r|$r2"; r=$r2; fi; echo "XA|expr|$r"
/usr/bin/expr --help </dev/null >/dev/null 2>&1; echo "XB|expr|$?"
/bin/busybox factor --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox factor --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|factor|$r|$r2"; r=$r2; fi; echo "XA|factor|$r"
/usr/bin/factor --help </dev/null >/dev/null 2>&1; echo "XB|factor|$?"
/bin/busybox fallocate --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fallocate --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fallocate|$r|$r2"; r=$r2; fi; echo "XA|fallocate|$r"
/usr/bin/fallocate --help </dev/null >/dev/null 2>&1; echo "XB|fallocate|$?"
/bin/busybox false --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox false --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|false|$r|$r2"; r=$r2; fi; echo "XA|false|$r"
/bin/false --help </dev/null >/dev/null 2>&1; echo "XB|false|$?"
/bin/busybox fatattr --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fatattr --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fatattr|$r|$r2"; r=$r2; fi; echo "XA|fatattr|$r"
/bin/fatattr --help </dev/null >/dev/null 2>&1; echo "XB|fatattr|$?"
/bin/busybox fbset --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fbset --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fbset|$r|$r2"; r=$r2; fi; echo "XA|fbset|$r"
/usr/sbin/fbset --help </dev/null >/dev/null 2>&1; echo "XB|fbset|$?"
/bin/busybox fbsplash --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fbsplash --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fbsplash|$r|$r2"; r=$r2; fi; echo "XA|fbsplash|$r"
/sbin/fbsplash --help </dev/null >/dev/null 2>&1; echo "XB|fbsplash|$?"
/bin/busybox fdflush --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fdflush --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fdflush|$r|$r2"; r=$r2; fi; echo "XA|fdflush|$r"
/bin/fdflush --help </dev/null >/dev/null 2>&1; echo "XB|fdflush|$?"
/bin/busybox fdisk --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fdisk --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fdisk|$r|$r2"; r=$r2; fi; echo "XA|fdisk|$r"
/sbin/fdisk --help </dev/null >/dev/null 2>&1; echo "XB|fdisk|$?"
/bin/busybox fgrep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fgrep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fgrep|$r|$r2"; r=$r2; fi; echo "XA|fgrep|$r"
/bin/fgrep --help </dev/null >/dev/null 2>&1; echo "XB|fgrep|$?"
/bin/busybox find --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox find --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|find|$r|$r2"; r=$r2; fi; echo "XA|find|$r"
/usr/bin/find --help </dev/null >/dev/null 2>&1; echo "XB|find|$?"
echo "# ACHUNK 4"
/bin/busybox findfs --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox findfs --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|findfs|$r|$r2"; r=$r2; fi; echo "XA|findfs|$r"
/sbin/findfs --help </dev/null >/dev/null 2>&1; echo "XB|findfs|$?"
/bin/busybox flock --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox flock --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|flock|$r|$r2"; r=$r2; fi; echo "XA|flock|$r"
/usr/bin/flock --help </dev/null >/dev/null 2>&1; echo "XB|flock|$?"
/bin/busybox fold --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fold --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fold|$r|$r2"; r=$r2; fi; echo "XA|fold|$r"
/usr/bin/fold --help </dev/null >/dev/null 2>&1; echo "XB|fold|$?"
/bin/busybox free --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox free --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|free|$r|$r2"; r=$r2; fi; echo "XA|free|$r"
/usr/bin/free --help </dev/null >/dev/null 2>&1; echo "XB|free|$?"
/bin/busybox fsck --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fsck --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fsck|$r|$r2"; r=$r2; fi; echo "XA|fsck|$r"
/sbin/fsck --help </dev/null >/dev/null 2>&1; echo "XB|fsck|$?"
/bin/busybox fstrim --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fstrim --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fstrim|$r|$r2"; r=$r2; fi; echo "XA|fstrim|$r"
/sbin/fstrim --help </dev/null >/dev/null 2>&1; echo "XB|fstrim|$?"
/bin/busybox fsync --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fsync --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fsync|$r|$r2"; r=$r2; fi; echo "XA|fsync|$r"
/bin/fsync --help </dev/null >/dev/null 2>&1; echo "XB|fsync|$?"
/bin/busybox fuser --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox fuser --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|fuser|$r|$r2"; r=$r2; fi; echo "XA|fuser|$r"
/usr/bin/fuser --help </dev/null >/dev/null 2>&1; echo "XB|fuser|$?"
/bin/busybox getopt --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox getopt --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|getopt|$r|$r2"; r=$r2; fi; echo "XA|getopt|$r"
/bin/getopt --help </dev/null >/dev/null 2>&1; echo "XB|getopt|$?"
/bin/busybox getty --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox getty --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|getty|$r|$r2"; r=$r2; fi; echo "XA|getty|$r"
/sbin/getty --help </dev/null >/dev/null 2>&1; echo "XB|getty|$?"
/bin/busybox grep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox grep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|grep|$r|$r2"; r=$r2; fi; echo "XA|grep|$r"
/bin/grep --help </dev/null >/dev/null 2>&1; echo "XB|grep|$?"
/bin/busybox groups --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox groups --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|groups|$r|$r2"; r=$r2; fi; echo "XA|groups|$r"
/usr/bin/groups --help </dev/null >/dev/null 2>&1; echo "XB|groups|$?"
/bin/busybox gunzip --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox gunzip --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|gunzip|$r|$r2"; r=$r2; fi; echo "XA|gunzip|$r"
/bin/gunzip --help </dev/null >/dev/null 2>&1; echo "XB|gunzip|$?"
/bin/busybox gzip --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox gzip --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|gzip|$r|$r2"; r=$r2; fi; echo "XA|gzip|$r"
/bin/gzip --help </dev/null >/dev/null 2>&1; echo "XB|gzip|$?"
/bin/busybox halt --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox halt --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|halt|$r|$r2"; r=$r2; fi; echo "XA|halt|$r"
/sbin/halt --help </dev/null >/dev/null 2>&1; echo "XB|halt|$?"
/bin/busybox hd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox hd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|hd|$r|$r2"; r=$r2; fi; echo "XA|hd|$r"
/usr/bin/hd --help </dev/null >/dev/null 2>&1; echo "XB|hd|$?"
/bin/busybox head --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox head --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|head|$r|$r2"; r=$r2; fi; echo "XA|head|$r"
/usr/bin/head --help </dev/null >/dev/null 2>&1; echo "XB|head|$?"
/bin/busybox hexdump --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox hexdump --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|hexdump|$r|$r2"; r=$r2; fi; echo "XA|hexdump|$r"
/usr/bin/hexdump --help </dev/null >/dev/null 2>&1; echo "XB|hexdump|$?"
/bin/busybox hostid --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox hostid --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|hostid|$r|$r2"; r=$r2; fi; echo "XA|hostid|$r"
/usr/bin/hostid --help </dev/null >/dev/null 2>&1; echo "XB|hostid|$?"
/bin/busybox hostname --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox hostname --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|hostname|$r|$r2"; r=$r2; fi; echo "XA|hostname|$r"
/bin/hostname --help </dev/null >/dev/null 2>&1; echo "XB|hostname|$?"
/bin/busybox hwclock --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox hwclock --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|hwclock|$r|$r2"; r=$r2; fi; echo "XA|hwclock|$r"
/sbin/hwclock --help </dev/null >/dev/null 2>&1; echo "XB|hwclock|$?"
/bin/busybox id --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox id --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|id|$r|$r2"; r=$r2; fi; echo "XA|id|$r"
/usr/bin/id --help </dev/null >/dev/null 2>&1; echo "XB|id|$?"
/bin/busybox ifconfig --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ifconfig --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ifconfig|$r|$r2"; r=$r2; fi; echo "XA|ifconfig|$r"
/sbin/ifconfig --help </dev/null >/dev/null 2>&1; echo "XB|ifconfig|$?"
/bin/busybox ifdown --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ifdown --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ifdown|$r|$r2"; r=$r2; fi; echo "XA|ifdown|$r"
/sbin/ifdown --help </dev/null >/dev/null 2>&1; echo "XB|ifdown|$?"
/bin/busybox ifenslave --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ifenslave --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ifenslave|$r|$r2"; r=$r2; fi; echo "XA|ifenslave|$r"
/sbin/ifenslave --help </dev/null >/dev/null 2>&1; echo "XB|ifenslave|$?"
echo "# ACHUNK 5"
/bin/busybox ifup --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ifup --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ifup|$r|$r2"; r=$r2; fi; echo "XA|ifup|$r"
/sbin/ifup --help </dev/null >/dev/null 2>&1; echo "XB|ifup|$?"
/bin/busybox init --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox init --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|init|$r|$r2"; r=$r2; fi; echo "XA|init|$r"
/sbin/init --help </dev/null >/dev/null 2>&1; echo "XB|init|$?"
/bin/busybox inotifyd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox inotifyd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|inotifyd|$r|$r2"; r=$r2; fi; echo "XA|inotifyd|$r"
/sbin/inotifyd --help </dev/null >/dev/null 2>&1; echo "XB|inotifyd|$?"
/bin/busybox insmod --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox insmod --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|insmod|$r|$r2"; r=$r2; fi; echo "XA|insmod|$r"
/sbin/insmod --help </dev/null >/dev/null 2>&1; echo "XB|insmod|$?"
/bin/busybox install --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox install --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|install|$r|$r2"; r=$r2; fi; echo "XA|install|$r"
/usr/bin/install --help </dev/null >/dev/null 2>&1; echo "XB|install|$?"
/bin/busybox ionice --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ionice --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ionice|$r|$r2"; r=$r2; fi; echo "XA|ionice|$r"
/bin/ionice --help </dev/null >/dev/null 2>&1; echo "XB|ionice|$?"
/bin/busybox iostat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox iostat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|iostat|$r|$r2"; r=$r2; fi; echo "XA|iostat|$r"
/bin/iostat --help </dev/null >/dev/null 2>&1; echo "XB|iostat|$?"
/bin/busybox ip --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ip --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ip|$r|$r2"; r=$r2; fi; echo "XA|ip|$r"
/sbin/ip --help </dev/null >/dev/null 2>&1; echo "XB|ip|$?"
/bin/busybox ipaddr --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ipaddr --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ipaddr|$r|$r2"; r=$r2; fi; echo "XA|ipaddr|$r"
/sbin/ipaddr --help </dev/null >/dev/null 2>&1; echo "XB|ipaddr|$?"
/bin/busybox ipcalc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ipcalc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ipcalc|$r|$r2"; r=$r2; fi; echo "XA|ipcalc|$r"
/bin/ipcalc --help </dev/null >/dev/null 2>&1; echo "XB|ipcalc|$?"
/bin/busybox ipcrm --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ipcrm --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ipcrm|$r|$r2"; r=$r2; fi; echo "XA|ipcrm|$r"
/usr/bin/ipcrm --help </dev/null >/dev/null 2>&1; echo "XB|ipcrm|$?"
/bin/busybox ipcs --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ipcs --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ipcs|$r|$r2"; r=$r2; fi; echo "XA|ipcs|$r"
/usr/bin/ipcs --help </dev/null >/dev/null 2>&1; echo "XB|ipcs|$?"
/bin/busybox iplink --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox iplink --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|iplink|$r|$r2"; r=$r2; fi; echo "XA|iplink|$r"
/sbin/iplink --help </dev/null >/dev/null 2>&1; echo "XB|iplink|$?"
/bin/busybox ipneigh --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ipneigh --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ipneigh|$r|$r2"; r=$r2; fi; echo "XA|ipneigh|$r"
/sbin/ipneigh --help </dev/null >/dev/null 2>&1; echo "XB|ipneigh|$?"
/bin/busybox iproute --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox iproute --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|iproute|$r|$r2"; r=$r2; fi; echo "XA|iproute|$r"
/sbin/iproute --help </dev/null >/dev/null 2>&1; echo "XB|iproute|$?"
/bin/busybox iprule --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox iprule --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|iprule|$r|$r2"; r=$r2; fi; echo "XA|iprule|$r"
/sbin/iprule --help </dev/null >/dev/null 2>&1; echo "XB|iprule|$?"
/bin/busybox iptunnel --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox iptunnel --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|iptunnel|$r|$r2"; r=$r2; fi; echo "XA|iptunnel|$r"
/sbin/iptunnel --help </dev/null >/dev/null 2>&1; echo "XB|iptunnel|$?"
/bin/busybox kbd_mode --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox kbd_mode --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|kbd_mode|$r|$r2"; r=$r2; fi; echo "XA|kbd_mode|$r"
/bin/kbd_mode --help </dev/null >/dev/null 2>&1; echo "XB|kbd_mode|$?"
/bin/busybox kill --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox kill --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|kill|$r|$r2"; r=$r2; fi; echo "XA|kill|$r"
/bin/kill --help </dev/null >/dev/null 2>&1; echo "XB|kill|$?"
/bin/busybox killall --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox killall --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|killall|$r|$r2"; r=$r2; fi; echo "XA|killall|$r"
/usr/bin/killall --help </dev/null >/dev/null 2>&1; echo "XB|killall|$?"
/bin/busybox killall5 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox killall5 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|killall5|$r|$r2"; r=$r2; fi; echo "XA|killall5|$r"
/usr/sbin/killall5 --help </dev/null >/dev/null 2>&1; echo "XB|killall5|$?"
/bin/busybox klogd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox klogd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|klogd|$r|$r2"; r=$r2; fi; echo "XA|klogd|$r"
/sbin/klogd --help </dev/null >/dev/null 2>&1; echo "XB|klogd|$?"
/bin/busybox last --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox last --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|last|$r|$r2"; r=$r2; fi; echo "XA|last|$r"
/usr/bin/last --help </dev/null >/dev/null 2>&1; echo "XB|last|$?"
/bin/busybox less --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox less --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|less|$r|$r2"; r=$r2; fi; echo "XA|less|$r"
/usr/bin/less --help </dev/null >/dev/null 2>&1; echo "XB|less|$?"
/bin/busybox link --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox link --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|link|$r|$r2"; r=$r2; fi; echo "XA|link|$r"
/bin/link --help </dev/null >/dev/null 2>&1; echo "XB|link|$?"
echo "# ACHUNK 6"
/bin/busybox linux32 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox linux32 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|linux32|$r|$r2"; r=$r2; fi; echo "XA|linux32|$r"
/bin/linux32 --help </dev/null >/dev/null 2>&1; echo "XB|linux32|$?"
/bin/busybox linux64 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox linux64 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|linux64|$r|$r2"; r=$r2; fi; echo "XA|linux64|$r"
/bin/linux64 --help </dev/null >/dev/null 2>&1; echo "XB|linux64|$?"
/bin/busybox ln --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ln --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ln|$r|$r2"; r=$r2; fi; echo "XA|ln|$r"
/bin/ln --help </dev/null >/dev/null 2>&1; echo "XB|ln|$?"
/bin/busybox loadfont --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox loadfont --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|loadfont|$r|$r2"; r=$r2; fi; echo "XA|loadfont|$r"
/usr/sbin/loadfont --help </dev/null >/dev/null 2>&1; echo "XB|loadfont|$?"
/bin/busybox loadkmap --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox loadkmap --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|loadkmap|$r|$r2"; r=$r2; fi; echo "XA|loadkmap|$r"
/sbin/loadkmap --help </dev/null >/dev/null 2>&1; echo "XB|loadkmap|$?"
/bin/busybox logger --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox logger --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|logger|$r|$r2"; r=$r2; fi; echo "XA|logger|$r"
/usr/bin/logger --help </dev/null >/dev/null 2>&1; echo "XB|logger|$?"
/bin/busybox login --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox login --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|login|$r|$r2"; r=$r2; fi; echo "XA|login|$r"
/bin/login --help </dev/null >/dev/null 2>&1; echo "XB|login|$?"
/bin/busybox logread --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox logread --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|logread|$r|$r2"; r=$r2; fi; echo "XA|logread|$r"
/sbin/logread --help </dev/null >/dev/null 2>&1; echo "XB|logread|$?"
/bin/busybox losetup --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox losetup --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|losetup|$r|$r2"; r=$r2; fi; echo "XA|losetup|$r"
/sbin/losetup --help </dev/null >/dev/null 2>&1; echo "XB|losetup|$?"
/bin/busybox ls --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ls --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ls|$r|$r2"; r=$r2; fi; echo "XA|ls|$r"
/bin/ls --help </dev/null >/dev/null 2>&1; echo "XB|ls|$?"
/bin/busybox lsattr --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lsattr --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lsattr|$r|$r2"; r=$r2; fi; echo "XA|lsattr|$r"
/bin/lsattr --help </dev/null >/dev/null 2>&1; echo "XB|lsattr|$?"
/bin/busybox lsmod --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lsmod --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lsmod|$r|$r2"; r=$r2; fi; echo "XA|lsmod|$r"
/sbin/lsmod --help </dev/null >/dev/null 2>&1; echo "XB|lsmod|$?"
/bin/busybox lsof --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lsof --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lsof|$r|$r2"; r=$r2; fi; echo "XA|lsof|$r"
/usr/bin/lsof --help </dev/null >/dev/null 2>&1; echo "XB|lsof|$?"
/bin/busybox lsusb --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lsusb --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lsusb|$r|$r2"; r=$r2; fi; echo "XA|lsusb|$r"
/usr/bin/lsusb --help </dev/null >/dev/null 2>&1; echo "XB|lsusb|$?"
/bin/busybox lzcat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lzcat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lzcat|$r|$r2"; r=$r2; fi; echo "XA|lzcat|$r"
/usr/bin/lzcat --help </dev/null >/dev/null 2>&1; echo "XB|lzcat|$?"
/bin/busybox lzma --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lzma --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lzma|$r|$r2"; r=$r2; fi; echo "XA|lzma|$r"
/usr/bin/lzma --help </dev/null >/dev/null 2>&1; echo "XB|lzma|$?"
/bin/busybox lzop --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lzop --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lzop|$r|$r2"; r=$r2; fi; echo "XA|lzop|$r"
/bin/lzop --help </dev/null >/dev/null 2>&1; echo "XB|lzop|$?"
/bin/busybox lzopcat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox lzopcat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|lzopcat|$r|$r2"; r=$r2; fi; echo "XA|lzopcat|$r"
/usr/bin/lzopcat --help </dev/null >/dev/null 2>&1; echo "XB|lzopcat|$?"
/bin/busybox makemime --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox makemime --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|makemime|$r|$r2"; r=$r2; fi; echo "XA|makemime|$r"
/bin/makemime --help </dev/null >/dev/null 2>&1; echo "XB|makemime|$?"
/bin/busybox md5sum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox md5sum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|md5sum|$r|$r2"; r=$r2; fi; echo "XA|md5sum|$r"
/usr/bin/md5sum --help </dev/null >/dev/null 2>&1; echo "XB|md5sum|$?"
/bin/busybox mdev --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mdev --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mdev|$r|$r2"; r=$r2; fi; echo "XA|mdev|$r"
/sbin/mdev --help </dev/null >/dev/null 2>&1; echo "XB|mdev|$?"
/bin/busybox mesg --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mesg --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mesg|$r|$r2"; r=$r2; fi; echo "XA|mesg|$r"
/usr/bin/mesg --help </dev/null >/dev/null 2>&1; echo "XB|mesg|$?"
/bin/busybox microcom --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox microcom --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|microcom|$r|$r2"; r=$r2; fi; echo "XA|microcom|$r"
/usr/bin/microcom --help </dev/null >/dev/null 2>&1; echo "XB|microcom|$?"
/bin/busybox mkdir --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mkdir --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mkdir|$r|$r2"; r=$r2; fi; echo "XA|mkdir|$r"
/bin/mkdir --help </dev/null >/dev/null 2>&1; echo "XB|mkdir|$?"
/bin/busybox mkdosfs --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mkdosfs --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mkdosfs|$r|$r2"; r=$r2; fi; echo "XA|mkdosfs|$r"
/sbin/mkdosfs --help </dev/null >/dev/null 2>&1; echo "XB|mkdosfs|$?"
echo "# ACHUNK 7"
/bin/busybox mkfifo --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mkfifo --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mkfifo|$r|$r2"; r=$r2; fi; echo "XA|mkfifo|$r"
/usr/bin/mkfifo --help </dev/null >/dev/null 2>&1; echo "XB|mkfifo|$?"
/bin/busybox mkfs.vfat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mkfs.vfat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mkfs.vfat|$r|$r2"; r=$r2; fi; echo "XA|mkfs.vfat|$r"
/sbin/mkfs.vfat --help </dev/null >/dev/null 2>&1; echo "XB|mkfs.vfat|$?"
/bin/busybox mknod --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mknod --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mknod|$r|$r2"; r=$r2; fi; echo "XA|mknod|$r"
/bin/mknod --help </dev/null >/dev/null 2>&1; echo "XB|mknod|$?"
/bin/busybox mkpasswd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mkpasswd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mkpasswd|$r|$r2"; r=$r2; fi; echo "XA|mkpasswd|$r"
/usr/bin/mkpasswd --help </dev/null >/dev/null 2>&1; echo "XB|mkpasswd|$?"
/bin/busybox mkswap --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mkswap --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mkswap|$r|$r2"; r=$r2; fi; echo "XA|mkswap|$r"
/sbin/mkswap --help </dev/null >/dev/null 2>&1; echo "XB|mkswap|$?"
/bin/busybox mktemp --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mktemp --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mktemp|$r|$r2"; r=$r2; fi; echo "XA|mktemp|$r"
/bin/mktemp --help </dev/null >/dev/null 2>&1; echo "XB|mktemp|$?"
/bin/busybox modinfo --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox modinfo --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|modinfo|$r|$r2"; r=$r2; fi; echo "XA|modinfo|$r"
/sbin/modinfo --help </dev/null >/dev/null 2>&1; echo "XB|modinfo|$?"
/bin/busybox modprobe --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox modprobe --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|modprobe|$r|$r2"; r=$r2; fi; echo "XA|modprobe|$r"
/sbin/modprobe --help </dev/null >/dev/null 2>&1; echo "XB|modprobe|$?"
/bin/busybox more --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox more --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|more|$r|$r2"; r=$r2; fi; echo "XA|more|$r"
/bin/more --help </dev/null >/dev/null 2>&1; echo "XB|more|$?"
/bin/busybox mount --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mount --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mount|$r|$r2"; r=$r2; fi; echo "XA|mount|$r"
/bin/mount --help </dev/null >/dev/null 2>&1; echo "XB|mount|$?"
/bin/busybox mountpoint --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mountpoint --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mountpoint|$r|$r2"; r=$r2; fi; echo "XA|mountpoint|$r"
/bin/mountpoint --help </dev/null >/dev/null 2>&1; echo "XB|mountpoint|$?"
/bin/busybox mpstat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mpstat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mpstat|$r|$r2"; r=$r2; fi; echo "XA|mpstat|$r"
/bin/mpstat --help </dev/null >/dev/null 2>&1; echo "XB|mpstat|$?"
/bin/busybox mv --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox mv --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|mv|$r|$r2"; r=$r2; fi; echo "XA|mv|$r"
/bin/mv --help </dev/null >/dev/null 2>&1; echo "XB|mv|$?"
/bin/busybox nameif --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nameif --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nameif|$r|$r2"; r=$r2; fi; echo "XA|nameif|$r"
/sbin/nameif --help </dev/null >/dev/null 2>&1; echo "XB|nameif|$?"
/bin/busybox nanddump --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nanddump --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nanddump|$r|$r2"; r=$r2; fi; echo "XA|nanddump|$r"
/usr/sbin/nanddump --help </dev/null >/dev/null 2>&1; echo "XB|nanddump|$?"
/bin/busybox nandwrite --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nandwrite --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nandwrite|$r|$r2"; r=$r2; fi; echo "XA|nandwrite|$r"
/usr/sbin/nandwrite --help </dev/null >/dev/null 2>&1; echo "XB|nandwrite|$?"
/bin/busybox nbd-client --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nbd-client --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nbd-client|$r|$r2"; r=$r2; fi; echo "XA|nbd-client|$r"
/usr/sbin/nbd-client --help </dev/null >/dev/null 2>&1; echo "XB|nbd-client|$?"
/bin/busybox nc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nc|$r|$r2"; r=$r2; fi; echo "XA|nc|$r"
/usr/bin/nc --help </dev/null >/dev/null 2>&1; echo "XB|nc|$?"
/bin/busybox netstat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox netstat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|netstat|$r|$r2"; r=$r2; fi; echo "XA|netstat|$r"
/bin/netstat --help </dev/null >/dev/null 2>&1; echo "XB|netstat|$?"
/bin/busybox nice --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nice --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nice|$r|$r2"; r=$r2; fi; echo "XA|nice|$r"
/bin/nice --help </dev/null >/dev/null 2>&1; echo "XB|nice|$?"
/bin/busybox nl --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nl --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nl|$r|$r2"; r=$r2; fi; echo "XA|nl|$r"
/usr/bin/nl --help </dev/null >/dev/null 2>&1; echo "XB|nl|$?"
/bin/busybox nmeter --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nmeter --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nmeter|$r|$r2"; r=$r2; fi; echo "XA|nmeter|$r"
/usr/bin/nmeter --help </dev/null >/dev/null 2>&1; echo "XB|nmeter|$?"
/bin/busybox nohup --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nohup --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nohup|$r|$r2"; r=$r2; fi; echo "XA|nohup|$r"
/usr/bin/nohup --help </dev/null >/dev/null 2>&1; echo "XB|nohup|$?"
/bin/busybox nologin --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nologin --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nologin|$r|$r2"; r=$r2; fi; echo "XA|nologin|$r"
/sbin/nologin --help </dev/null >/dev/null 2>&1; echo "XB|nologin|$?"
/bin/busybox nproc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nproc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nproc|$r|$r2"; r=$r2; fi; echo "XA|nproc|$r"
/usr/bin/nproc --help </dev/null >/dev/null 2>&1; echo "XB|nproc|$?"
echo "# ACHUNK 8"
/bin/busybox nsenter --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nsenter --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nsenter|$r|$r2"; r=$r2; fi; echo "XA|nsenter|$r"
/usr/bin/nsenter --help </dev/null >/dev/null 2>&1; echo "XB|nsenter|$?"
/bin/busybox nslookup --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox nslookup --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|nslookup|$r|$r2"; r=$r2; fi; echo "XA|nslookup|$r"
/usr/bin/nslookup --help </dev/null >/dev/null 2>&1; echo "XB|nslookup|$?"
/bin/busybox ntpd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ntpd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ntpd|$r|$r2"; r=$r2; fi; echo "XA|ntpd|$r"
/usr/sbin/ntpd --help </dev/null >/dev/null 2>&1; echo "XB|ntpd|$?"
/bin/busybox od --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox od --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|od|$r|$r2"; r=$r2; fi; echo "XA|od|$r"
/usr/bin/od --help </dev/null >/dev/null 2>&1; echo "XB|od|$?"
/bin/busybox openvt --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox openvt --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|openvt|$r|$r2"; r=$r2; fi; echo "XA|openvt|$r"
/usr/bin/openvt --help </dev/null >/dev/null 2>&1; echo "XB|openvt|$?"
/bin/busybox partprobe --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox partprobe --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|partprobe|$r|$r2"; r=$r2; fi; echo "XA|partprobe|$r"
/usr/sbin/partprobe --help </dev/null >/dev/null 2>&1; echo "XB|partprobe|$?"
/bin/busybox passwd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox passwd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|passwd|$r|$r2"; r=$r2; fi; echo "XA|passwd|$r"
/usr/bin/passwd --help </dev/null >/dev/null 2>&1; echo "XB|passwd|$?"
/bin/busybox paste --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox paste --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|paste|$r|$r2"; r=$r2; fi; echo "XA|paste|$r"
/usr/bin/paste --help </dev/null >/dev/null 2>&1; echo "XB|paste|$?"
/bin/busybox pgrep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pgrep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pgrep|$r|$r2"; r=$r2; fi; echo "XA|pgrep|$r"
/usr/bin/pgrep --help </dev/null >/dev/null 2>&1; echo "XB|pgrep|$?"
/bin/busybox pidof --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pidof --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pidof|$r|$r2"; r=$r2; fi; echo "XA|pidof|$r"
/bin/pidof --help </dev/null >/dev/null 2>&1; echo "XB|pidof|$?"
/bin/busybox ping --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ping --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ping|$r|$r2"; r=$r2; fi; echo "XA|ping|$r"
/bin/ping --help </dev/null >/dev/null 2>&1; echo "XB|ping|$?"
/bin/busybox ping6 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ping6 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ping6|$r|$r2"; r=$r2; fi; echo "XA|ping6|$r"
/bin/ping6 --help </dev/null >/dev/null 2>&1; echo "XB|ping6|$?"
/bin/busybox pipe_progress --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pipe_progress --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pipe_progress|$r|$r2"; r=$r2; fi; echo "XA|pipe_progress|$r"
/bin/pipe_progress --help </dev/null >/dev/null 2>&1; echo "XB|pipe_progress|$?"
/bin/busybox pivot_root --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pivot_root --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pivot_root|$r|$r2"; r=$r2; fi; echo "XA|pivot_root|$r"
/sbin/pivot_root --help </dev/null >/dev/null 2>&1; echo "XB|pivot_root|$?"
/bin/busybox pkill --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pkill --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pkill|$r|$r2"; r=$r2; fi; echo "XA|pkill|$r"
/usr/bin/pkill --help </dev/null >/dev/null 2>&1; echo "XB|pkill|$?"
/bin/busybox pmap --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pmap --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pmap|$r|$r2"; r=$r2; fi; echo "XA|pmap|$r"
/usr/bin/pmap --help </dev/null >/dev/null 2>&1; echo "XB|pmap|$?"
/bin/busybox poweroff --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox poweroff --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|poweroff|$r|$r2"; r=$r2; fi; echo "XA|poweroff|$r"
/sbin/poweroff --help </dev/null >/dev/null 2>&1; echo "XB|poweroff|$?"
/bin/busybox printenv --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox printenv --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|printenv|$r|$r2"; r=$r2; fi; echo "XA|printenv|$r"
/bin/printenv --help </dev/null >/dev/null 2>&1; echo "XB|printenv|$?"
/bin/busybox printf --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox printf --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|printf|$r|$r2"; r=$r2; fi; echo "XA|printf|$r"
/usr/bin/printf --help </dev/null >/dev/null 2>&1; echo "XB|printf|$?"
/bin/busybox ps --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ps --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ps|$r|$r2"; r=$r2; fi; echo "XA|ps|$r"
/bin/ps --help </dev/null >/dev/null 2>&1; echo "XB|ps|$?"
/bin/busybox pscan --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pscan --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pscan|$r|$r2"; r=$r2; fi; echo "XA|pscan|$r"
/usr/bin/pscan --help </dev/null >/dev/null 2>&1; echo "XB|pscan|$?"
/bin/busybox pstree --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pstree --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pstree|$r|$r2"; r=$r2; fi; echo "XA|pstree|$r"
/usr/bin/pstree --help </dev/null >/dev/null 2>&1; echo "XB|pstree|$?"
/bin/busybox pwd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pwd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pwd|$r|$r2"; r=$r2; fi; echo "XA|pwd|$r"
/bin/pwd --help </dev/null >/dev/null 2>&1; echo "XB|pwd|$?"
/bin/busybox pwdx --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox pwdx --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|pwdx|$r|$r2"; r=$r2; fi; echo "XA|pwdx|$r"
/usr/bin/pwdx --help </dev/null >/dev/null 2>&1; echo "XB|pwdx|$?"
/bin/busybox raidautorun --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox raidautorun --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|raidautorun|$r|$r2"; r=$r2; fi; echo "XA|raidautorun|$r"
/sbin/raidautorun --help </dev/null >/dev/null 2>&1; echo "XB|raidautorun|$?"
echo "# ACHUNK 9"
/bin/busybox rdate --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rdate --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rdate|$r|$r2"; r=$r2; fi; echo "XA|rdate|$r"
/usr/sbin/rdate --help </dev/null >/dev/null 2>&1; echo "XB|rdate|$?"
/bin/busybox rdev --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rdev --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rdev|$r|$r2"; r=$r2; fi; echo "XA|rdev|$r"
/usr/sbin/rdev --help </dev/null >/dev/null 2>&1; echo "XB|rdev|$?"
/bin/busybox readahead --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox readahead --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|readahead|$r|$r2"; r=$r2; fi; echo "XA|readahead|$r"
/usr/sbin/readahead --help </dev/null >/dev/null 2>&1; echo "XB|readahead|$?"
/bin/busybox readlink --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox readlink --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|readlink|$r|$r2"; r=$r2; fi; echo "XA|readlink|$r"
/usr/bin/readlink --help </dev/null >/dev/null 2>&1; echo "XB|readlink|$?"
/bin/busybox realpath --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox realpath --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|realpath|$r|$r2"; r=$r2; fi; echo "XA|realpath|$r"
/usr/bin/realpath --help </dev/null >/dev/null 2>&1; echo "XB|realpath|$?"
/bin/busybox reboot --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox reboot --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|reboot|$r|$r2"; r=$r2; fi; echo "XA|reboot|$r"
/sbin/reboot --help </dev/null >/dev/null 2>&1; echo "XB|reboot|$?"
/bin/busybox reformime --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox reformime --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|reformime|$r|$r2"; r=$r2; fi; echo "XA|reformime|$r"
/bin/reformime --help </dev/null >/dev/null 2>&1; echo "XB|reformime|$?"
/bin/busybox remove-shell --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox remove-shell --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|remove-shell|$r|$r2"; r=$r2; fi; echo "XA|remove-shell|$r"
/usr/sbin/remove-shell --help </dev/null >/dev/null 2>&1; echo "XB|remove-shell|$?"
/bin/busybox renice --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox renice --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|renice|$r|$r2"; r=$r2; fi; echo "XA|renice|$r"
/usr/bin/renice --help </dev/null >/dev/null 2>&1; echo "XB|renice|$?"
/bin/busybox reset --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox reset --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|reset|$r|$r2"; r=$r2; fi; echo "XA|reset|$r"
/usr/bin/reset --help </dev/null >/dev/null 2>&1; echo "XB|reset|$?"
/bin/busybox resize --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox resize --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|resize|$r|$r2"; r=$r2; fi; echo "XA|resize|$r"
/usr/bin/resize --help </dev/null >/dev/null 2>&1; echo "XB|resize|$?"
/bin/busybox rev --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rev --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rev|$r|$r2"; r=$r2; fi; echo "XA|rev|$r"
/bin/rev --help </dev/null >/dev/null 2>&1; echo "XB|rev|$?"
/bin/busybox rfkill --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rfkill --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rfkill|$r|$r2"; r=$r2; fi; echo "XA|rfkill|$r"
/usr/sbin/rfkill --help </dev/null >/dev/null 2>&1; echo "XB|rfkill|$?"
/bin/busybox rm --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rm --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rm|$r|$r2"; r=$r2; fi; echo "XA|rm|$r"
/bin/rm --help </dev/null >/dev/null 2>&1; echo "XB|rm|$?"
/bin/busybox rmdir --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rmdir --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rmdir|$r|$r2"; r=$r2; fi; echo "XA|rmdir|$r"
/bin/rmdir --help </dev/null >/dev/null 2>&1; echo "XB|rmdir|$?"
/bin/busybox rmmod --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox rmmod --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|rmmod|$r|$r2"; r=$r2; fi; echo "XA|rmmod|$r"
/sbin/rmmod --help </dev/null >/dev/null 2>&1; echo "XB|rmmod|$?"
/bin/busybox route --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox route --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|route|$r|$r2"; r=$r2; fi; echo "XA|route|$r"
/sbin/route --help </dev/null >/dev/null 2>&1; echo "XB|route|$?"
/bin/busybox run-parts --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox run-parts --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|run-parts|$r|$r2"; r=$r2; fi; echo "XA|run-parts|$r"
/bin/run-parts --help </dev/null >/dev/null 2>&1; echo "XB|run-parts|$?"
/bin/busybox sed --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sed --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sed|$r|$r2"; r=$r2; fi; echo "XA|sed|$r"
/bin/sed --help </dev/null >/dev/null 2>&1; echo "XB|sed|$?"
/bin/busybox sendmail --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sendmail --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sendmail|$r|$r2"; r=$r2; fi; echo "XA|sendmail|$r"
/usr/sbin/sendmail --help </dev/null >/dev/null 2>&1; echo "XB|sendmail|$?"
/bin/busybox seq --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox seq --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|seq|$r|$r2"; r=$r2; fi; echo "XA|seq|$r"
/usr/bin/seq --help </dev/null >/dev/null 2>&1; echo "XB|seq|$?"
/bin/busybox setconsole --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setconsole --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setconsole|$r|$r2"; r=$r2; fi; echo "XA|setconsole|$r"
/sbin/setconsole --help </dev/null >/dev/null 2>&1; echo "XB|setconsole|$?"
/bin/busybox setfont --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setfont --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setfont|$r|$r2"; r=$r2; fi; echo "XA|setfont|$r"
/usr/sbin/setfont --help </dev/null >/dev/null 2>&1; echo "XB|setfont|$?"
/bin/busybox setkeycodes --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setkeycodes --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setkeycodes|$r|$r2"; r=$r2; fi; echo "XA|setkeycodes|$r"
/usr/bin/setkeycodes --help </dev/null >/dev/null 2>&1; echo "XB|setkeycodes|$?"
/bin/busybox setlogcons --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setlogcons --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setlogcons|$r|$r2"; r=$r2; fi; echo "XA|setlogcons|$r"
/usr/sbin/setlogcons --help </dev/null >/dev/null 2>&1; echo "XB|setlogcons|$?"
echo "# ACHUNK 10"
/bin/busybox setpriv --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setpriv --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setpriv|$r|$r2"; r=$r2; fi; echo "XA|setpriv|$r"
/bin/setpriv --help </dev/null >/dev/null 2>&1; echo "XB|setpriv|$?"
/bin/busybox setserial --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setserial --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setserial|$r|$r2"; r=$r2; fi; echo "XA|setserial|$r"
/bin/setserial --help </dev/null >/dev/null 2>&1; echo "XB|setserial|$?"
/bin/busybox setsid --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox setsid --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|setsid|$r|$r2"; r=$r2; fi; echo "XA|setsid|$r"
/usr/bin/setsid --help </dev/null >/dev/null 2>&1; echo "XB|setsid|$?"
/bin/busybox sh --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sh --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sh|$r|$r2"; r=$r2; fi; echo "XA|sh|$r"
/bin/sh --help </dev/null >/dev/null 2>&1; echo "XB|sh|$?"
/bin/busybox sha1sum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sha1sum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sha1sum|$r|$r2"; r=$r2; fi; echo "XA|sha1sum|$r"
/usr/bin/sha1sum --help </dev/null >/dev/null 2>&1; echo "XB|sha1sum|$?"
/bin/busybox sha256sum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sha256sum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sha256sum|$r|$r2"; r=$r2; fi; echo "XA|sha256sum|$r"
/usr/bin/sha256sum --help </dev/null >/dev/null 2>&1; echo "XB|sha256sum|$?"
/bin/busybox sha3sum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sha3sum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sha3sum|$r|$r2"; r=$r2; fi; echo "XA|sha3sum|$r"
/usr/bin/sha3sum --help </dev/null >/dev/null 2>&1; echo "XB|sha3sum|$?"
/bin/busybox sha512sum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sha512sum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sha512sum|$r|$r2"; r=$r2; fi; echo "XA|sha512sum|$r"
/usr/bin/sha512sum --help </dev/null >/dev/null 2>&1; echo "XB|sha512sum|$?"
/bin/busybox showkey --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox showkey --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|showkey|$r|$r2"; r=$r2; fi; echo "XA|showkey|$r"
/usr/bin/showkey --help </dev/null >/dev/null 2>&1; echo "XB|showkey|$?"
/bin/busybox shred --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox shred --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|shred|$r|$r2"; r=$r2; fi; echo "XA|shred|$r"
/usr/bin/shred --help </dev/null >/dev/null 2>&1; echo "XB|shred|$?"
/bin/busybox shuf --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox shuf --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|shuf|$r|$r2"; r=$r2; fi; echo "XA|shuf|$r"
/usr/bin/shuf --help </dev/null >/dev/null 2>&1; echo "XB|shuf|$?"
/bin/busybox slattach --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox slattach --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|slattach|$r|$r2"; r=$r2; fi; echo "XA|slattach|$r"
/sbin/slattach --help </dev/null >/dev/null 2>&1; echo "XB|slattach|$?"
/bin/busybox sleep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sleep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sleep|$r|$r2"; r=$r2; fi; echo "XA|sleep|$r"
/bin/sleep --help </dev/null >/dev/null 2>&1; echo "XB|sleep|$?"
/bin/busybox sort --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sort --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sort|$r|$r2"; r=$r2; fi; echo "XA|sort|$r"
/usr/bin/sort --help </dev/null >/dev/null 2>&1; echo "XB|sort|$?"
/bin/busybox split --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox split --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|split|$r|$r2"; r=$r2; fi; echo "XA|split|$r"
/usr/bin/split --help </dev/null >/dev/null 2>&1; echo "XB|split|$?"
/bin/busybox stat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox stat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|stat|$r|$r2"; r=$r2; fi; echo "XA|stat|$r"
/bin/stat --help </dev/null >/dev/null 2>&1; echo "XB|stat|$?"
/bin/busybox strings --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox strings --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|strings|$r|$r2"; r=$r2; fi; echo "XA|strings|$r"
/usr/bin/strings --help </dev/null >/dev/null 2>&1; echo "XB|strings|$?"
/bin/busybox stty --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox stty --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|stty|$r|$r2"; r=$r2; fi; echo "XA|stty|$r"
/bin/stty --help </dev/null >/dev/null 2>&1; echo "XB|stty|$?"
/bin/busybox su --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox su --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|su|$r|$r2"; r=$r2; fi; echo "XA|su|$r"
/bin/su --help </dev/null >/dev/null 2>&1; echo "XB|su|$?"
/bin/busybox sum --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sum --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sum|$r|$r2"; r=$r2; fi; echo "XA|sum|$r"
/usr/bin/sum --help </dev/null >/dev/null 2>&1; echo "XB|sum|$?"
/bin/busybox swapoff --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox swapoff --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|swapoff|$r|$r2"; r=$r2; fi; echo "XA|swapoff|$r"
/sbin/swapoff --help </dev/null >/dev/null 2>&1; echo "XB|swapoff|$?"
/bin/busybox swapon --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox swapon --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|swapon|$r|$r2"; r=$r2; fi; echo "XA|swapon|$r"
/sbin/swapon --help </dev/null >/dev/null 2>&1; echo "XB|swapon|$?"
/bin/busybox switch_root --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox switch_root --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|switch_root|$r|$r2"; r=$r2; fi; echo "XA|switch_root|$r"
/sbin/switch_root --help </dev/null >/dev/null 2>&1; echo "XB|switch_root|$?"
/bin/busybox sync --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sync --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sync|$r|$r2"; r=$r2; fi; echo "XA|sync|$r"
/bin/sync --help </dev/null >/dev/null 2>&1; echo "XB|sync|$?"
/bin/busybox sysctl --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox sysctl --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|sysctl|$r|$r2"; r=$r2; fi; echo "XA|sysctl|$r"
/sbin/sysctl --help </dev/null >/dev/null 2>&1; echo "XB|sysctl|$?"
echo "# ACHUNK 11"
/bin/busybox syslogd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox syslogd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|syslogd|$r|$r2"; r=$r2; fi; echo "XA|syslogd|$r"
/sbin/syslogd --help </dev/null >/dev/null 2>&1; echo "XB|syslogd|$?"
/bin/busybox tac --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tac --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tac|$r|$r2"; r=$r2; fi; echo "XA|tac|$r"
/usr/bin/tac --help </dev/null >/dev/null 2>&1; echo "XB|tac|$?"
/bin/busybox tail --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tail --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tail|$r|$r2"; r=$r2; fi; echo "XA|tail|$r"
/usr/bin/tail --help </dev/null >/dev/null 2>&1; echo "XB|tail|$?"
/bin/busybox tar --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tar --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tar|$r|$r2"; r=$r2; fi; echo "XA|tar|$r"
/bin/tar --help </dev/null >/dev/null 2>&1; echo "XB|tar|$?"
/bin/busybox tee --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tee --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tee|$r|$r2"; r=$r2; fi; echo "XA|tee|$r"
/usr/bin/tee --help </dev/null >/dev/null 2>&1; echo "XB|tee|$?"
/bin/busybox test --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox test --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|test|$r|$r2"; r=$r2; fi; echo "XA|test|$r"
/usr/bin/test --help </dev/null >/dev/null 2>&1; echo "XB|test|$?"
/bin/busybox time --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox time --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|time|$r|$r2"; r=$r2; fi; echo "XA|time|$r"
/usr/bin/time --help </dev/null >/dev/null 2>&1; echo "XB|time|$?"
/bin/busybox timeout --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox timeout --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|timeout|$r|$r2"; r=$r2; fi; echo "XA|timeout|$r"
/usr/bin/timeout --help </dev/null >/dev/null 2>&1; echo "XB|timeout|$?"
/bin/busybox top --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox top --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|top|$r|$r2"; r=$r2; fi; echo "XA|top|$r"
/usr/bin/top --help </dev/null >/dev/null 2>&1; echo "XB|top|$?"
/bin/busybox touch --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox touch --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|touch|$r|$r2"; r=$r2; fi; echo "XA|touch|$r"
/bin/touch --help </dev/null >/dev/null 2>&1; echo "XB|touch|$?"
/bin/busybox tr --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tr --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tr|$r|$r2"; r=$r2; fi; echo "XA|tr|$r"
/usr/bin/tr --help </dev/null >/dev/null 2>&1; echo "XB|tr|$?"
/bin/busybox traceroute --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox traceroute --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|traceroute|$r|$r2"; r=$r2; fi; echo "XA|traceroute|$r"
/usr/bin/traceroute --help </dev/null >/dev/null 2>&1; echo "XB|traceroute|$?"
/bin/busybox traceroute6 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox traceroute6 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|traceroute6|$r|$r2"; r=$r2; fi; echo "XA|traceroute6|$r"
/usr/bin/traceroute6 --help </dev/null >/dev/null 2>&1; echo "XB|traceroute6|$?"
/bin/busybox tree --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tree --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tree|$r|$r2"; r=$r2; fi; echo "XA|tree|$r"
/usr/bin/tree --help </dev/null >/dev/null 2>&1; echo "XB|tree|$?"
/bin/busybox true --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox true --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|true|$r|$r2"; r=$r2; fi; echo "XA|true|$r"
/bin/true --help </dev/null >/dev/null 2>&1; echo "XB|true|$?"
/bin/busybox truncate --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox truncate --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|truncate|$r|$r2"; r=$r2; fi; echo "XA|truncate|$r"
/usr/bin/truncate --help </dev/null >/dev/null 2>&1; echo "XB|truncate|$?"
/bin/busybox tty --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tty --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tty|$r|$r2"; r=$r2; fi; echo "XA|tty|$r"
/usr/bin/tty --help </dev/null >/dev/null 2>&1; echo "XB|tty|$?"
/bin/busybox ttysize --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox ttysize --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|ttysize|$r|$r2"; r=$r2; fi; echo "XA|ttysize|$r"
/usr/bin/ttysize --help </dev/null >/dev/null 2>&1; echo "XB|ttysize|$?"
/bin/busybox tunctl --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox tunctl --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|tunctl|$r|$r2"; r=$r2; fi; echo "XA|tunctl|$r"
/sbin/tunctl --help </dev/null >/dev/null 2>&1; echo "XB|tunctl|$?"
/bin/busybox udhcpc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox udhcpc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|udhcpc|$r|$r2"; r=$r2; fi; echo "XA|udhcpc|$r"
/sbin/udhcpc --help </dev/null >/dev/null 2>&1; echo "XB|udhcpc|$?"
/bin/busybox udhcpc6 --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox udhcpc6 --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|udhcpc6|$r|$r2"; r=$r2; fi; echo "XA|udhcpc6|$r"
/usr/bin/udhcpc6 --help </dev/null >/dev/null 2>&1; echo "XB|udhcpc6|$?"
/bin/busybox umount --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox umount --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|umount|$r|$r2"; r=$r2; fi; echo "XA|umount|$r"
/bin/umount --help </dev/null >/dev/null 2>&1; echo "XB|umount|$?"
/bin/busybox uname --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox uname --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|uname|$r|$r2"; r=$r2; fi; echo "XA|uname|$r"
/bin/uname --help </dev/null >/dev/null 2>&1; echo "XB|uname|$?"
/bin/busybox unexpand --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unexpand --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unexpand|$r|$r2"; r=$r2; fi; echo "XA|unexpand|$r"
/usr/bin/unexpand --help </dev/null >/dev/null 2>&1; echo "XB|unexpand|$?"
/bin/busybox uniq --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox uniq --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|uniq|$r|$r2"; r=$r2; fi; echo "XA|uniq|$r"
/usr/bin/uniq --help </dev/null >/dev/null 2>&1; echo "XB|uniq|$?"
echo "# ACHUNK 12"
/bin/busybox unix2dos --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unix2dos --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unix2dos|$r|$r2"; r=$r2; fi; echo "XA|unix2dos|$r"
/usr/bin/unix2dos --help </dev/null >/dev/null 2>&1; echo "XB|unix2dos|$?"
/bin/busybox unlink --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unlink --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unlink|$r|$r2"; r=$r2; fi; echo "XA|unlink|$r"
/usr/bin/unlink --help </dev/null >/dev/null 2>&1; echo "XB|unlink|$?"
/bin/busybox unlzma --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unlzma --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unlzma|$r|$r2"; r=$r2; fi; echo "XA|unlzma|$r"
/usr/bin/unlzma --help </dev/null >/dev/null 2>&1; echo "XB|unlzma|$?"
/bin/busybox unlzop --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unlzop --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unlzop|$r|$r2"; r=$r2; fi; echo "XA|unlzop|$r"
/usr/bin/unlzop --help </dev/null >/dev/null 2>&1; echo "XB|unlzop|$?"
/bin/busybox unshare --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unshare --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unshare|$r|$r2"; r=$r2; fi; echo "XA|unshare|$r"
/usr/bin/unshare --help </dev/null >/dev/null 2>&1; echo "XB|unshare|$?"
/bin/busybox unxz --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unxz --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unxz|$r|$r2"; r=$r2; fi; echo "XA|unxz|$r"
/usr/bin/unxz --help </dev/null >/dev/null 2>&1; echo "XB|unxz|$?"
/bin/busybox unzip --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox unzip --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|unzip|$r|$r2"; r=$r2; fi; echo "XA|unzip|$r"
/usr/bin/unzip --help </dev/null >/dev/null 2>&1; echo "XB|unzip|$?"
/bin/busybox uptime --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox uptime --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|uptime|$r|$r2"; r=$r2; fi; echo "XA|uptime|$r"
/usr/bin/uptime --help </dev/null >/dev/null 2>&1; echo "XB|uptime|$?"
/bin/busybox usleep --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox usleep --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|usleep|$r|$r2"; r=$r2; fi; echo "XA|usleep|$r"
/bin/usleep --help </dev/null >/dev/null 2>&1; echo "XB|usleep|$?"
/bin/busybox uudecode --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox uudecode --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|uudecode|$r|$r2"; r=$r2; fi; echo "XA|uudecode|$r"
/usr/bin/uudecode --help </dev/null >/dev/null 2>&1; echo "XB|uudecode|$?"
/bin/busybox uuencode --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox uuencode --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|uuencode|$r|$r2"; r=$r2; fi; echo "XA|uuencode|$r"
/usr/bin/uuencode --help </dev/null >/dev/null 2>&1; echo "XB|uuencode|$?"
/bin/busybox vconfig --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox vconfig --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|vconfig|$r|$r2"; r=$r2; fi; echo "XA|vconfig|$r"
/sbin/vconfig --help </dev/null >/dev/null 2>&1; echo "XB|vconfig|$?"
/bin/busybox vi --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox vi --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|vi|$r|$r2"; r=$r2; fi; echo "XA|vi|$r"
/usr/bin/vi --help </dev/null >/dev/null 2>&1; echo "XB|vi|$?"
/bin/busybox vlock --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox vlock --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|vlock|$r|$r2"; r=$r2; fi; echo "XA|vlock|$r"
/usr/bin/vlock --help </dev/null >/dev/null 2>&1; echo "XB|vlock|$?"
/bin/busybox volname --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox volname --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|volname|$r|$r2"; r=$r2; fi; echo "XA|volname|$r"
/usr/bin/volname --help </dev/null >/dev/null 2>&1; echo "XB|volname|$?"
/bin/busybox watch --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox watch --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|watch|$r|$r2"; r=$r2; fi; echo "XA|watch|$r"
/bin/watch --help </dev/null >/dev/null 2>&1; echo "XB|watch|$?"
/bin/busybox watchdog --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox watchdog --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|watchdog|$r|$r2"; r=$r2; fi; echo "XA|watchdog|$r"
/sbin/watchdog --help </dev/null >/dev/null 2>&1; echo "XB|watchdog|$?"
/bin/busybox wc --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox wc --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|wc|$r|$r2"; r=$r2; fi; echo "XA|wc|$r"
/usr/bin/wc --help </dev/null >/dev/null 2>&1; echo "XB|wc|$?"
/bin/busybox wget --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox wget --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|wget|$r|$r2"; r=$r2; fi; echo "XA|wget|$r"
/usr/bin/wget --help </dev/null >/dev/null 2>&1; echo "XB|wget|$?"
/bin/busybox which --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox which --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|which|$r|$r2"; r=$r2; fi; echo "XA|which|$r"
/usr/bin/which --help </dev/null >/dev/null 2>&1; echo "XB|which|$?"
/bin/busybox who --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox who --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|who|$r|$r2"; r=$r2; fi; echo "XA|who|$r"
/usr/bin/who --help </dev/null >/dev/null 2>&1; echo "XB|who|$?"
/bin/busybox whoami --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox whoami --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|whoami|$r|$r2"; r=$r2; fi; echo "XA|whoami|$r"
/usr/bin/whoami --help </dev/null >/dev/null 2>&1; echo "XB|whoami|$?"
/bin/busybox whois --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox whois --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|whois|$r|$r2"; r=$r2; fi; echo "XA|whois|$r"
/usr/bin/whois --help </dev/null >/dev/null 2>&1; echo "XB|whois|$?"
/bin/busybox xargs --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox xargs --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|xargs|$r|$r2"; r=$r2; fi; echo "XA|xargs|$r"
/usr/bin/xargs --help </dev/null >/dev/null 2>&1; echo "XB|xargs|$?"
/bin/busybox xxd --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox xxd --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|xxd|$r|$r2"; r=$r2; fi; echo "XA|xxd|$r"
/usr/bin/xxd --help </dev/null >/dev/null 2>&1; echo "XB|xxd|$?"
echo "# ACHUNK 13"
/bin/busybox xzcat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox xzcat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|xzcat|$r|$r2"; r=$r2; fi; echo "XA|xzcat|$r"
/usr/bin/xzcat --help </dev/null >/dev/null 2>&1; echo "XB|xzcat|$?"
/bin/busybox yes --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox yes --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|yes|$r|$r2"; r=$r2; fi; echo "XA|yes|$r"
/usr/bin/yes --help </dev/null >/dev/null 2>&1; echo "XB|yes|$?"
/bin/busybox zcat --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox zcat --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|zcat|$r|$r2"; r=$r2; fi; echo "XA|zcat|$r"
/bin/zcat --help </dev/null >/dev/null 2>&1; echo "XB|zcat|$?"
/bin/busybox zcip --help </dev/null >/dev/null 2>&1; r=$?; if [ "$r" != 0 ]; then /bin/busybox zcip --help </dev/null >/dev/null 2>&1; r2=$?; echo "XAR|zcip|$r|$r2"; r=$r2; fi; echo "XA|zcip|$r"
/sbin/zcip --help </dev/null >/dev/null 2>&1; echo "XB|zcip|$?"
echo "# A-DONE"
