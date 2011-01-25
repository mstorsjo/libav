#!/bin/sh

set -e

: ${SRCDIR:=.}

$SRCDIR/configure-symbian.sh
make -f $SRCDIR/Makefile.symbian clean
make clean
make -j5
# On CSL GCC 2005q1 or 2009q1, linking the full static avconv.exe
# may fail due to thumb function call relocations that are truncated
# to fit, but building the shared libraries version still works fine.
make -f $SRCDIR/Makefile.symbian avconv_shared.exe avconv_static.exe
makesis $SRCDIR/avconv_shared.pkg
signsis $SRCDIR/avconv_shared.sis
makesis $SRCDIR/avconv_static.pkg
signsis $SRCDIR/avconv_static.sis
