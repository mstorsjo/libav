#!/bin/sh

: ${SRCDIR:=.}

$SRCDIR/configure --enable-cross-compile --arch=arm --target-os=symbian --cross-prefix=arm-none-symbianelf- --cpu=armv5t --extra-cflags="-mapcs -msoft-float -I${EPOCROOT}/epoc32/include -I${EPOCROOT}/epoc32/include/stdapis -fno-inline-functions" --extra-ldflags="-L${EPOCROOT}/epoc32/release/armv5/lib -L${EPOCROOT}/epoc32/release/armv5/urel" --sysinclude=$EPOCROOT/epoc32/include

