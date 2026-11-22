#!/bin/sh
# Build fn_chests.prx (Linux). Needs: clang, ld.lld, OpenOrbis PS4 toolchain, GoldHEN Plugins SDK.
#   export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
#   export GOLDHEN_SDK=/path/to/GoldHEN_Plugins_SDK   (contains include/, build/crtprx.o, libGoldHEN_Hook.a, source/)
set -e
T=${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN}; G=${GOLDHEN_SDK:?set GOLDHEN_SDK}
mkdir -p build
CF="-D__FINAL__=1 -D__USE_PRINTF__ --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -I$G/include -I$T/include -I."
clang $CF -c main.c -o build/main.o
clang $CF -c plugin_common.c -o build/plugin_common.o
ld.lld -m elf_x86_64 -pie --script $T/link.x --eh-frame-hdr -L$T/lib -L$G -L$G/lib \
  $G/build/crtprx.o build/main.o build/plugin_common.o \
  -lSceLibcInternal -lGoldHEN_Hook -lkernel -lSceSysmodule -lSceUserService -lSceNet -o build/fn_chests.elf
rm -f build/fn_chests.oelf build/fn_chests.prx
$T/bin/linux/create-fself -in=build/fn_chests.elf -out=build/fn_chests.oelf --lib=build/fn_chests.prx --paid 0x3800000000000011
ls -la build/fn_chests.prx
# NOTE: give every new build a NEW file name on the console (fn_chestsN.prx), the PS4 can keep loading an old same-name file.
