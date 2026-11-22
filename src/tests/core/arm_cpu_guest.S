// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "arm_cpu_guest_layout.h"

#ifdef __APPLE__
.section __TEXT,__const
.p2align 4
.globl _arm_cpu_guest_begin
_arm_cpu_guest_begin:
#else
.section .rodata.arm_cpu_guest,"a",%progbits
.p2align 4
.global arm_cpu_guest_begin
.type arm_cpu_guest_begin, %object
arm_cpu_guest_begin:
#endif
.Limage:
    b .Lmain
    .long 0
    .ascii "HOMEBREW"
    .ascii "NRO0"
    .long 0
    .long ARM_GUEST_IMAGE_SIZE
    .long 0
    .long 0, ARM_GUEST_PAGE_SIZE
    .long ARM_GUEST_PAGE_SIZE, ARM_GUEST_PAGE_SIZE
    .long ARM_GUEST_DATA_OFFSET, 3 * ARM_GUEST_PAGE_SIZE
    .long 0
    .org .Limage + 0x80

.macro stage number
    mov w9, #\number
    str w9, [x19, #ARM_GUEST_STAGE]
.endm

.Lmain:
    adr x19, .Ldata
    mov x20, #ARM_GUEST_TLS_MARKER
    msr tpidr_el0, x20

    stage ARM_GUEST_CHECK_SVC
    mov x1, #0
    svc #0x25
    cbz w0, .Lexit
    mov w1, #0xffff8000
    svc #0x25
    cbnz w0, .Lexit
    cbz x1, .Lexit
    str x1, [x19, #ARM_GUEST_THREAD_ID]

    ldr w0, [x19, #ARM_GUEST_MODE]
    cmp w0, #ARM_GUEST_FAULT
    b.eq .Lfault
    cmp w0, #ARM_GUEST_PERMISSION_FAULT
    b.eq .Lfault
    cmp w0, #ARM_GUEST_EXECUTE_FAULT
    b.eq .Lfault
    cmp w0, #ARM_GUEST_REVOKE_FAULT
    b.eq .Lfault
    cmp w0, #ARM_GUEST_BENCHMARK
    b.eq .Lbenchmark

    stage ARM_GUEST_CHECK_TLS
    mrs x21, tpidrro_el0
    cbz x21, .Lexit
    mrs x0, tpidr_el0
    cmp x0, x20
    b.ne .Lexit

    stage ARM_GUEST_CHECK_TIMER
    mrs x0, cntfrq_el0
    str x0, [x19, #ARM_GUEST_FREQUENCY]
    mrs x22, cntpct_el0
    svc #0x1e
    mrs x1, cntpct_el0
    cmp x0, x22
    b.lo .Lexit
    cmp x0, x1
    b.hi .Lexit
    ldr w0, [x19, #ARM_GUEST_MODE]
    cmp w0, #ARM_GUEST_CHECKS_NATIVE
    b.ne 3f
    mrs x22, cntvct_el0
    svc #0x1e
    mrs x1, cntvct_el0
    cmp x0, x22
    b.lo .Lexit
    cmp x0, x1
    b.hi .Lexit
3:
    movz w0, #(3000000 & 0xffff)
    movk w0, #(3000000 >> 16), lsl #16
    mrs x22, cntpct_el0
    svc #0xb
    mrs x0, cntpct_el0
    subs x0, x0, x22
    b.lo .Lexit
    cmp x0, #10, lsl #12
    b.lo .Lexit

    stage ARM_GUEST_CHECK_EXCLUSIVES
    add x1, x19, #ARM_GUEST_EXCLUSIVE_WORD
    ldxr x2, [x1]
    clrex
    mov x3, #42
    stxr w4, x3, [x1]
    cbz w4, .Lexit
    ldr x4, [x1]
    cbnz x4, .Lexit
    mov w5, #128
1:
    ldxr x2, [x1]
    stxr w4, x3, [x1]
    cbz w4, 2f
    subs w5, w5, #1
    b.ne 1b
    b .Lexit
2:
    ldr x4, [x1]
    cmp x4, x3
    b.ne .Lexit

    stage ARM_GUEST_CHECK_PREEMPTION
    adr x1, .Lworker
    mov x2, #0
    add x3, x19, #2, lsl #12
    mov w4, #59
    mov w5, #0
    svc #0x8
    cbnz w0, .Lexit
    str w1, [x19, #ARM_GUEST_HANDLE_A]

    adr x1, .Lworker
    mov x2, #1
    add x3, x19, #3, lsl #12
    mov w4, #59
    mov w5, #0
    svc #0x8
    cbnz w0, .Lexit
    str w1, [x19, #ARM_GUEST_HANDLE_B]

    ldr w0, [x19, #ARM_GUEST_HANDLE_A]
    svc #0x9
    cbnz w0, .Lexit
    ldr w0, [x19, #ARM_GUEST_HANDLE_B]
    svc #0x9
    cbnz w0, .Lexit

    add x1, x19, #ARM_GUEST_HANDLE_A
    bl .Lwait
    add x1, x19, #ARM_GUEST_HANDLE_B
    bl .Lwait

    ldr x0, [x19, #ARM_GUEST_PREEMPT_READY]
    cbz x0, .Lexit
    cmp x0, x21
    b.eq .Lexit
    ldr x1, [x19, #ARM_GUEST_PREEMPT_PEER]
    cbz x1, .Lexit
    cmp x1, x21
    b.eq .Lexit
    cmp x0, x1
    b.eq .Lexit
    mrs x0, tpidrro_el0
    cmp x0, x21
    b.ne .Lexit
    mrs x0, tpidr_el0
    cmp x0, x20
    b.ne .Lexit
    b .Lpass

.Lwait:
    mov w2, #1
    movz w3, #(2000000000 & 0xffff)
    movk w3, #(2000000000 >> 16), lsl #16
    svc #0x18
    cbnz w0, .Lexit
    cbnz w1, .Lexit
    ret

.Lworker:
    adr x19, .Ldata
    add x1, x19, #ARM_GUEST_PREEMPT_READY
    add x2, x19, #ARM_GUEST_PREEMPT_PEER
    mrs x3, tpidrro_el0
    add x4, x0, #0x652
    msr tpidr_el0, x4
    cbnz x0, 2f
    stlr x3, [x1]
1:
    ldar x5, [x2]
    cbz x5, 1b
    mrs x5, tpidr_el0
    cmp x5, x4
    b.eq .Lworker_exit
    stlr xzr, [x1]
    b .Lworker_exit
2:
    ldar x5, [x1]
    cbz x5, 2b
    stlr x3, [x2]
.Lworker_exit:
    svc #0xa
    b .

.Lfault:
    stage ARM_GUEST_CHECK_FAULT
    mov w2, w0
    adr x1, .Lfault_worker
    add x3, x19, #2, lsl #12
    mov w4, #43
    cmp w2, #ARM_GUEST_REVOKE_FAULT
    cset w5, eq
    svc #0x8
    cbnz w0, .Lexit
    str w1, [x19, #ARM_GUEST_HANDLE_A]
    ldr w8, [x19, #ARM_GUEST_MODE]
    cmp w8, #ARM_GUEST_REVOKE_FAULT
    b.ne 1f
    add x8, x19, #1, lsl #12
    ldr x2, [x8]
1:
    mov w0, w1
    svc #0x9
    cbnz w0, .Lexit
    ldr w0, [x19, #ARM_GUEST_MODE]
    cmp w0, #ARM_GUEST_REVOKE_FAULT
    b.ne .Lpass
    add x1, x19, #ARM_GUEST_REVOKE_FINISHED
1:
    ldar w0, [x1]
    cbz w0, 1b
    b .Lpass

.Lfault_worker:
    adr x19, .Ldata
    cmp w0, #ARM_GUEST_REVOKE_FAULT
    b.eq .Lrevoke
    mov w1, #1
    str w1, [x19, #ARM_GUEST_FAULT_ENTERED]
    cmp w0, #ARM_GUEST_PERMISSION_FAULT
    b.eq 1f
    cmp w0, #ARM_GUEST_EXECUTE_FAULT
    b.eq 3f
    mov x0, #0
    ldr x2, [x0]
    b .Lfault_continued
1:
    adr x0, .Lrodata
    ldr x2, [x0]
    str w1, [x19, #ARM_GUEST_RO_READ_DONE]
    str x2, [x0]
.Lfault_continued:
    str w1, [x19, #ARM_GUEST_FAULT_CONTINUED]
    b .Lworker_exit
3:
    add x0, x19, #1, lsl #12
    blr x0
    b .Lfault_continued

.Lrevoke:
    add x0, x19, #1, lsl #12
    ldr x2, [x0]
    mov w1, #1
    add x3, x19, #ARM_GUEST_FAULT_ENTERED
    stlr w1, [x3]
    add x3, x19, #ARM_GUEST_REVOKE_READY
1:
    ldar w4, [x3]
    cbz w4, 1b
    ldr x2, [x0]
    b .Lfault_continued

.Lbenchmark:
    stage ARM_GUEST_CHECK_BENCHMARK
    mrs x0, cntfrq_el0
    str x0, [x19, #ARM_GUEST_FREQUENCY]
    bl .Lcompute
    bl .Lcompute
    str x0, [x19, #ARM_GUEST_COMPUTE_TICKS]
    bl .Lsvc_loop
    bl .Lsvc_loop
    str x0, [x19, #ARM_GUEST_SVC_TICKS]
    b .Lpass

.Lcompute:
    mrs x21, cntpct_el0
    mov x0, #1
    mov w1, #ARM_GUEST_COMPUTE_ITERATIONS
1:
    add x0, x0, x0, lsl #1
    subs w1, w1, #1
    b.ne 1b
    mrs x0, cntpct_el0
    sub x0, x0, x21
    ret

.Lsvc_loop:
    mrs x21, cntpct_el0
    mov w22, #ARM_GUEST_SVC_ITERATIONS
1:
    svc #0x1e
    subs w22, w22, #1
    b.ne 1b
    mrs x0, cntpct_el0
    sub x0, x0, x21
    ret

.Lpass:
    stage ARM_GUEST_PASS
.Lexit:
    svc #0x7
    b .

    .org .Limage + ARM_GUEST_PAGE_SIZE
.Lrodata:
    .org .Limage + ARM_GUEST_DATA_OFFSET
.Ldata:
    .org .Limage + ARM_GUEST_IMAGE_SIZE
#ifndef __APPLE__
.size arm_cpu_guest_begin, . - arm_cpu_guest_begin

.section .note.GNU-stack,"",%progbits
#endif
