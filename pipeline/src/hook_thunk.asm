; hook_thunk.asm — x64 MASM
;
; HookedPresentThunk: placed in vtable[8] instead of a C++ function.
;
; Key property: g_OriginalPresent is called via a TAILCALL (JMP, not CALL).
; This means at the moment g_OriginalPresent begins executing, the stack
; pointer is IDENTICAL to what it would be if no hook existed at all.
; Zero stack overhead for the Openplanet render chain.
;
; Call order:
;   1. Run cfx_HookedPresentCallback(sc, sync, flags)  [uses our own frame]
;   2. Tear down our frame completely
;   3. JMP to cfx_OriginalPresent                      [tailcall — no frame]

.CODE

EXTERN cfx_OriginalPresent       : QWORD   ; set in hook.cpp Install()
EXTERN cfx_HookedPresentCallback : PROC    ; defined in hook.cpp
EXTERN cfx_CallOrigWithSEH       : PROC    ; SEH wrapper in hook.cpp

; Signature: HRESULT STDMETHODCALLTYPE (IDXGISwapChain*, UINT, UINT)
; x64 args: RCX = pSwapChain, EDX = SyncInterval, R8D = Flags
HookedPresentThunk PROC FRAME
    push    rbp
    .pushreg rbp
    sub     rsp, 80                 ; 32 shadow + 24 saved args + 8 align + 16 extra
    .allocstack 80
    .endprolog

    ; Save the three arguments so we can restore them after the callback.
    mov     QWORD PTR [rsp+48], rcx     ; pSwapChain
    mov     DWORD PTR [rsp+56], edx     ; SyncInterval
    mov     DWORD PTR [rsp+60], r8d     ; Flags

    ; Call our C++ callback — runs on THIS stack frame (not on orig's chain).
    ; RCX/RDX/R8 already contain the right args.
    call    cfx_HookedPresentCallback

    ; Restore arguments for the tailcall to orig.
    mov     rcx, QWORD PTR [rsp+48]
    mov     edx, DWORD PTR [rsp+56]
    mov     r8d, DWORD PTR [rsp+60]

    ; Tear down our frame COMPLETELY before the JMP.
    add     rsp, 80
    pop     rbp

    ; Tailcall into the SEH wrapper which calls orig.
    ; Stack pointer is exactly what the game set before calling vtable[8].
    jmp     cfx_CallOrigWithSEH

HookedPresentThunk ENDP

END
