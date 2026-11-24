;------------------------------------------------------------------------------
;
; Hypercalls to ReBIOS through VMCALL and VMMCALL (ReBiosSandbox.h): the
; number in RAX, the arguments in RBX, RCX, RDX, and RSI, the result in RAX.
;
; Copyright (c) 2026, Mintsuki and contributors.<BR>
; SPDX-License-Identifier: BSD-2-Clause-Patent
;
;------------------------------------------------------------------------------

    DEFAULT REL
    SECTION .text

;------------------------------------------------------------------------------
;  UINT64
;  EFIAPI
;  AsmReBiosVmcall (
;    IN UINT64  Call,
;    IN UINT64  Arg1,
;    IN UINT64  Arg2,
;    IN UINT64  Arg3,
;    IN UINT64  Arg4
;    );
;------------------------------------------------------------------------------
global ASM_PFX(AsmReBiosVmcall)
ASM_PFX(AsmReBiosVmcall):
    push    rbx
    push    rsi
    mov     rax, rcx
    mov     rbx, rdx
    mov     rcx, r8
    mov     rdx, r9
    ; The fifth argument, past the return address, the four arguments' shadow
    ; space, and the two registers pushed.
    mov     rsi, [rsp + 0x38]
    vmcall
    pop     rsi
    pop     rbx
    ret

;------------------------------------------------------------------------------
;  UINT64
;  EFIAPI
;  AsmReBiosVmmcall (
;    IN UINT64  Call,
;    IN UINT64  Arg1,
;    IN UINT64  Arg2,
;    IN UINT64  Arg3,
;    IN UINT64  Arg4
;    );
;------------------------------------------------------------------------------
global ASM_PFX(AsmReBiosVmmcall)
ASM_PFX(AsmReBiosVmmcall):
    push    rbx
    push    rsi
    mov     rax, rcx
    mov     rbx, rdx
    mov     rcx, r8
    mov     rdx, r9
    mov     rsi, [rsp + 0x38]
    vmmcall
    pop     rsi
    pop     rbx
    ret
