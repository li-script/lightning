; Win64 callee-saved register probe for tests/context-switch.cpp.
OPTION CASEMAP:NONE

EXTERN context_switch:PROC
PUBLIC li_test_context_switch

.code
ALIGN 16
li_test_context_switch PROC FRAME
	push rbx
	.PUSHREG rbx
	push rdi
	.PUSHREG rdi
	push rsi
	.PUSHREG rsi
	push r12
	.PUSHREG r12
	push r13
	.PUSHREG r13
	push r14
	.PUSHREG r14
	push r15
	.PUSHREG r15
	sub rsp, 112
	.ALLOCSTACK 112
	movdqu XMMWORD PTR [rsp + 48], xmm6
	.SAVEXMM128 xmm6, 48
	movdqu XMMWORD PTR [rsp + 64], xmm7
	.SAVEXMM128 xmm7, 64
	movdqu XMMWORD PTR [rsp + 80], xmm12
	.SAVEXMM128 xmm12, 80
	movdqu XMMWORD PTR [rsp + 96], xmm15
	.SAVEXMM128 xmm15, 96
	.ENDPROLOG

	mov QWORD PTR [rsp + 32], r9
	stmxcsr DWORD PTR [rsp + 40]
	fnstcw WORD PTR [rsp + 44]

	mov rbx, QWORD PTR [r8 + 0]
	mov rdi, QWORD PTR [r8 + 8]
	mov rsi, QWORD PTR [r8 + 16]
	mov r12, QWORD PTR [r8 + 24]
	mov r13, QWORD PTR [r8 + 32]
	mov r14, QWORD PTR [r8 + 40]
	mov r15, QWORD PTR [r8 + 48]
	ldmxcsr DWORD PTR [r8 + 56]
	fldcw WORD PTR [r8 + 60]
	movdqu xmm6, XMMWORD PTR [r8 + 64]
	movdqu xmm7, XMMWORD PTR [r8 + 80]
	movdqu xmm12, XMMWORD PTR [r8 + 96]
	movdqu xmm15, XMMWORD PTR [r8 + 112]

	call context_switch

	mov rax, QWORD PTR [rsp + 32]
	mov QWORD PTR [rax + 0], rbx
	mov QWORD PTR [rax + 8], rdi
	mov QWORD PTR [rax + 16], rsi
	mov QWORD PTR [rax + 24], r12
	mov QWORD PTR [rax + 32], r13
	mov QWORD PTR [rax + 40], r14
	mov QWORD PTR [rax + 48], r15
	stmxcsr DWORD PTR [rax + 56]
	fnstcw WORD PTR [rax + 60]
	movdqu XMMWORD PTR [rax + 64], xmm6
	movdqu XMMWORD PTR [rax + 80], xmm7
	movdqu XMMWORD PTR [rax + 96], xmm12
	movdqu XMMWORD PTR [rax + 112], xmm15

	movdqu xmm6, XMMWORD PTR [rsp + 48]
	movdqu xmm7, XMMWORD PTR [rsp + 64]
	movdqu xmm12, XMMWORD PTR [rsp + 80]
	movdqu xmm15, XMMWORD PTR [rsp + 96]
	ldmxcsr DWORD PTR [rsp + 40]
	fldcw WORD PTR [rsp + 44]
	add rsp, 112
	pop r15
	pop r14
	pop r13
	pop r12
	pop rsi
	pop rdi
	pop rbx
	ret
li_test_context_switch ENDP

END
