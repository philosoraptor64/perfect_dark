// pdcoro: a minimal arm64 coroutine switch for the i64 module (port/src/i64_module.c).
// The game's main loop runs on its own stack on i64's main thread and gives control back
// after every tick, so SDL, UIKit and Metal see the same thread as in the stand-alone app.
//
//   void pdcoro_switch(void **save_sp, void *load_sp)
//     saves the callee-saved state (x19-x30, d8-d15) on the current stack, stores sp in
//     *save_sp, switches to load_sp and restores the state saved there.
//   A fresh stack is prepared by pdcoro_prepare (i64_module.c): its saved x30 points at
//   pdcoro_trampoline, x19 = entry function, x20 = its argument.
#if defined(__aarch64__) || defined(__arm64__)

    .text
    .p2align 2
    .globl _pdcoro_switch
_pdcoro_switch:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x2, sp
    str x2, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret

    // First switch into a prepared stack lands here: call entry(arg); entry never returns
    // (it switches back for good), but stop cleanly if it does.
    .p2align 2
    .globl _pdcoro_trampoline
_pdcoro_trampoline:
    mov x29, #0
    mov x0, x20
    blr x19
    brk #0x1

#endif
