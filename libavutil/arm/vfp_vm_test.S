/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "asm.S"

function ff_vfp_vm_test, export=1
        fmrx            r12, fpscr
        orr             r12, r12, #(1 << 16) @ set vector size to 2
        fmxr            fpscr, r12

        mov             r0,  #0
        vmov.f32        s8,  r0
        vmov.f32        s9,  r0
        vmov.f32        s10, r0
        mov             r0,  #(0xff << 24)
        vmov.f32        s0,  r0

        @ Add a nonzero value to s8 and s9
        vadd.f32        s8,  s0

        bic             r12, r12, #(7 << 16)
        fmxr            fpscr, r12

        # Return a nonzero value if s9 did get updated
        vmov.f32        r0,  s9
        bx              lr
endfunc
