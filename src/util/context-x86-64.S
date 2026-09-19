#include <util/context.hpp>

#if defined(__x86_64__) || defined(_M_X64)

#if defined(__APPLE__)
	#define LI_CONTEXT_SWITCH _context_switch
	#define LI_CONTEXT_BOOTSTRAP _li_context_bootstrap
	#define LI_CONTEXT_RUN _li_context_run
#else
	#define LI_CONTEXT_SWITCH context_switch
	#define LI_CONTEXT_BOOTSTRAP li_context_bootstrap
	#define LI_CONTEXT_RUN li_context_run
#endif

.text
.p2align 4, 0x90
.globl LI_CONTEXT_SWITCH
#if defined(__ELF__)
.type LI_CONTEXT_SWITCH, @function
#elif defined(_WIN32)
.def LI_CONTEXT_SWITCH; .scl 2; .type 32; .endef
.seh_proc LI_CONTEXT_SWITCH
#endif
LI_CONTEXT_SWITCH:
#if defined(_WIN32)
.seh_endprologue
	movq %rbx, LI_CONTEXT_WIN64_RBX_OFFSET(%rcx)
	movq %rbp, LI_CONTEXT_WIN64_RBP_OFFSET(%rcx)
	movq %rdi, LI_CONTEXT_WIN64_RDI_OFFSET(%rcx)
	movq %rsi, LI_CONTEXT_WIN64_RSI_OFFSET(%rcx)
	movq %r12, LI_CONTEXT_WIN64_R12_OFFSET(%rcx)
	movq %r13, LI_CONTEXT_WIN64_R13_OFFSET(%rcx)
	movq %r14, LI_CONTEXT_WIN64_R14_OFFSET(%rcx)
	movq %r15, LI_CONTEXT_WIN64_R15_OFFSET(%rcx)
	leaq 8(%rsp), %rax
	movq %rax, LI_CONTEXT_WIN64_RSP_OFFSET(%rcx)
	movq (%rsp), %rax
	movq %rax, LI_CONTEXT_WIN64_PC_OFFSET(%rcx)
	stmxcsr LI_CONTEXT_WIN64_MXCSR_OFFSET(%rcx)
	fnstcw LI_CONTEXT_WIN64_X87CW_OFFSET(%rcx)
	movdqu %xmm6, LI_CONTEXT_WIN64_XMM6_OFFSET(%rcx)
	movdqu %xmm7, LI_CONTEXT_WIN64_XMM7_OFFSET(%rcx)
	movdqu %xmm8, LI_CONTEXT_WIN64_XMM8_OFFSET(%rcx)
	movdqu %xmm9, LI_CONTEXT_WIN64_XMM9_OFFSET(%rcx)
	movdqu %xmm10, LI_CONTEXT_WIN64_XMM10_OFFSET(%rcx)
	movdqu %xmm11, LI_CONTEXT_WIN64_XMM11_OFFSET(%rcx)
	movdqu %xmm12, LI_CONTEXT_WIN64_XMM12_OFFSET(%rcx)
	movdqu %xmm13, LI_CONTEXT_WIN64_XMM13_OFFSET(%rcx)
	movdqu %xmm14, LI_CONTEXT_WIN64_XMM14_OFFSET(%rcx)
	movdqu %xmm15, LI_CONTEXT_WIN64_XMM15_OFFSET(%rcx)
	movq %gs:8, %rax
	movq %rax, LI_CONTEXT_WIN64_STACKBASE_OFFSET(%rcx)
	movq %gs:16, %rax
	movq %rax, LI_CONTEXT_WIN64_STACKLIMIT_OFFSET(%rcx)

	ldmxcsr LI_CONTEXT_WIN64_MXCSR_OFFSET(%rdx)
	fldcw LI_CONTEXT_WIN64_X87CW_OFFSET(%rdx)
	movdqu LI_CONTEXT_WIN64_XMM6_OFFSET(%rdx), %xmm6
	movdqu LI_CONTEXT_WIN64_XMM7_OFFSET(%rdx), %xmm7
	movdqu LI_CONTEXT_WIN64_XMM8_OFFSET(%rdx), %xmm8
	movdqu LI_CONTEXT_WIN64_XMM9_OFFSET(%rdx), %xmm9
	movdqu LI_CONTEXT_WIN64_XMM10_OFFSET(%rdx), %xmm10
	movdqu LI_CONTEXT_WIN64_XMM11_OFFSET(%rdx), %xmm11
	movdqu LI_CONTEXT_WIN64_XMM12_OFFSET(%rdx), %xmm12
	movdqu LI_CONTEXT_WIN64_XMM13_OFFSET(%rdx), %xmm13
	movdqu LI_CONTEXT_WIN64_XMM14_OFFSET(%rdx), %xmm14
	movdqu LI_CONTEXT_WIN64_XMM15_OFFSET(%rdx), %xmm15
	movq LI_CONTEXT_WIN64_STACKBASE_OFFSET(%rdx), %r8
	movq LI_CONTEXT_WIN64_STACKLIMIT_OFFSET(%rdx), %r9
	movq LI_CONTEXT_WIN64_PC_OFFSET(%rdx), %rax
	movq LI_CONTEXT_WIN64_RBX_OFFSET(%rdx), %rbx
	movq LI_CONTEXT_WIN64_RBP_OFFSET(%rdx), %rbp
	movq LI_CONTEXT_WIN64_RDI_OFFSET(%rdx), %rdi
	movq LI_CONTEXT_WIN64_RSI_OFFSET(%rdx), %rsi
	movq LI_CONTEXT_WIN64_R12_OFFSET(%rdx), %r12
	movq LI_CONTEXT_WIN64_R13_OFFSET(%rdx), %r13
	movq LI_CONTEXT_WIN64_R14_OFFSET(%rdx), %r14
	movq LI_CONTEXT_WIN64_R15_OFFSET(%rdx), %r15
	movq %r8, %gs:8
	movq %r9, %gs:16
	movq LI_CONTEXT_WIN64_RSP_OFFSET(%rdx), %rsp
	pushq %rax
	ret
#else
	movq %rbx, LI_CONTEXT_SYSV64_RBX_OFFSET(%rdi)
	movq %rbp, LI_CONTEXT_SYSV64_RBP_OFFSET(%rdi)
	movq %r12, LI_CONTEXT_SYSV64_R12_OFFSET(%rdi)
	movq %r13, LI_CONTEXT_SYSV64_R13_OFFSET(%rdi)
	movq %r14, LI_CONTEXT_SYSV64_R14_OFFSET(%rdi)
	movq %r15, LI_CONTEXT_SYSV64_R15_OFFSET(%rdi)
	leaq 8(%rsp), %rax
	movq %rax, LI_CONTEXT_SYSV64_RSP_OFFSET(%rdi)
	movq (%rsp), %rax
	movq %rax, LI_CONTEXT_SYSV64_PC_OFFSET(%rdi)
	stmxcsr LI_CONTEXT_SYSV64_MXCSR_OFFSET(%rdi)
	fnstcw LI_CONTEXT_SYSV64_X87CW_OFFSET(%rdi)

	ldmxcsr LI_CONTEXT_SYSV64_MXCSR_OFFSET(%rsi)
	fldcw LI_CONTEXT_SYSV64_X87CW_OFFSET(%rsi)
	movq LI_CONTEXT_SYSV64_PC_OFFSET(%rsi), %rax
	movq LI_CONTEXT_SYSV64_RBX_OFFSET(%rsi), %rbx
	movq LI_CONTEXT_SYSV64_RBP_OFFSET(%rsi), %rbp
	movq LI_CONTEXT_SYSV64_R12_OFFSET(%rsi), %r12
	movq LI_CONTEXT_SYSV64_R13_OFFSET(%rsi), %r13
	movq LI_CONTEXT_SYSV64_R14_OFFSET(%rsi), %r14
	movq LI_CONTEXT_SYSV64_R15_OFFSET(%rsi), %r15
	movq LI_CONTEXT_SYSV64_RSP_OFFSET(%rsi), %rsp
	pushq %rax
	ret
#endif
#if defined(__ELF__)
.size LI_CONTEXT_SWITCH, .-LI_CONTEXT_SWITCH
#elif defined(_WIN32)
.seh_endproc
#endif

.p2align 4, 0x90
.globl LI_CONTEXT_BOOTSTRAP
#if defined(__ELF__)
.type LI_CONTEXT_BOOTSTRAP, @function
.hidden LI_CONTEXT_BOOTSTRAP
#elif defined(_WIN32)
.def LI_CONTEXT_BOOTSTRAP; .scl 2; .type 32; .endef
.seh_proc LI_CONTEXT_BOOTSTRAP
#endif
LI_CONTEXT_BOOTSTRAP:
#if defined(_WIN32)
.seh_endprologue
	movq %r12, %rcx
	movq %r13, %rdx
	movq %r14, %r8
	movq %r15, %r9
	jmp LI_CONTEXT_RUN
#else
	movq %r12, %rdi
	movq %r13, %rsi
	movq %r14, %rdx
	movq %r15, %rcx
	jmp LI_CONTEXT_RUN
#endif
#if defined(__ELF__)
.size LI_CONTEXT_BOOTSTRAP, .-LI_CONTEXT_BOOTSTRAP
#elif defined(_WIN32)
.seh_endproc
#endif

#if defined(__APPLE__)
.subsections_via_symbols
#elif defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

#endif
