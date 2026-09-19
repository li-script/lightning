#include <util/context.hpp>

#if defined(__aarch64__) && !defined(_WIN32)

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
.p2align 2
.globl LI_CONTEXT_SWITCH
#if defined(__ELF__)
.type LI_CONTEXT_SWITCH, %function
#endif
LI_CONTEXT_SWITCH:
	stp x19, x20, [x0, #LI_CONTEXT_A64_X19_OFFSET]
	stp x21, x22, [x0, #LI_CONTEXT_A64_X21_OFFSET]
	stp x23, x24, [x0, #LI_CONTEXT_A64_X23_OFFSET]
	stp x25, x26, [x0, #LI_CONTEXT_A64_X25_OFFSET]
	stp x27, x28, [x0, #LI_CONTEXT_A64_X27_OFFSET]
	stp x29, x30, [x0, #LI_CONTEXT_A64_X29_OFFSET]
	mov x2, sp
	str x2, [x0, #LI_CONTEXT_A64_SP_OFFSET]
	stp d8, d9, [x0, #LI_CONTEXT_A64_D8_OFFSET]
	stp d10, d11, [x0, #LI_CONTEXT_A64_D10_OFFSET]
	stp d12, d13, [x0, #LI_CONTEXT_A64_D12_OFFSET]
	stp d14, d15, [x0, #LI_CONTEXT_A64_D14_OFFSET]

	ldr x2, [x1, #LI_CONTEXT_A64_SP_OFFSET]
	ldp d8, d9, [x1, #LI_CONTEXT_A64_D8_OFFSET]
	ldp d10, d11, [x1, #LI_CONTEXT_A64_D10_OFFSET]
	ldp d12, d13, [x1, #LI_CONTEXT_A64_D12_OFFSET]
	ldp d14, d15, [x1, #LI_CONTEXT_A64_D14_OFFSET]
	ldp x19, x20, [x1, #LI_CONTEXT_A64_X19_OFFSET]
	ldp x21, x22, [x1, #LI_CONTEXT_A64_X21_OFFSET]
	ldp x23, x24, [x1, #LI_CONTEXT_A64_X23_OFFSET]
	ldp x25, x26, [x1, #LI_CONTEXT_A64_X25_OFFSET]
	ldp x27, x28, [x1, #LI_CONTEXT_A64_X27_OFFSET]
	ldp x29, x30, [x1, #LI_CONTEXT_A64_X29_OFFSET]
	mov sp, x2
	ret
#if defined(__ELF__)
.size LI_CONTEXT_SWITCH, .-LI_CONTEXT_SWITCH
#endif

.p2align 2
.globl LI_CONTEXT_BOOTSTRAP
#if defined(__ELF__)
.type LI_CONTEXT_BOOTSTRAP, %function
.hidden LI_CONTEXT_BOOTSTRAP
#endif
LI_CONTEXT_BOOTSTRAP:
	// BTI c. Encoded directly so this source remains accepted by assemblers predating armv8.5-a.
	.inst 0xd503245f
	mov x0, x19
	mov x1, x20
	mov x2, x21
	mov x3, x22
	b LI_CONTEXT_RUN
#if defined(__ELF__)
.size LI_CONTEXT_BOOTSTRAP, .-LI_CONTEXT_BOOTSTRAP
#endif

#if defined(__APPLE__)
.subsections_via_symbols
#elif defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

#endif
