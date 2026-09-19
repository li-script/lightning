#include <ir/a64.hpp>
#include <ir/arch_a64.hpp>
#include <util/code.hpp>
#include <util/common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

#if LI_JIT && LI_ARCH_ARM && !LI_32
	#include <ir/ir2mir.hpp>
	#include <ir/mir.hpp>
	#include <ir/proc.hpp>
	#include <vm/rc.hpp>
	#include <vm/state.hpp>
#endif

namespace {
	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "A64 backend probe: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}

	void instruction_probe() {
		using namespace li::ir::a64;

		emitter instructions;
		instructions.msub(gp_width::x64, 0, 1, 2, 3);
		instructions.sxtb(gp_width::x64, 0, 1);
		instructions.sxth(gp_width::x64, 2, 3);
		instructions.sxtw(4, 5);
		instructions.fcsel(fp_width::d64, 0, 1, 2, condition::ne);
		instructions.crc32c(gp_width::x64, 0, 1, 2);
		instructions.rdcycle(3);
		auto           encoded    = instructions.words();
		const uint32_t expected[] = {
			 0x9b028c20u,
			 0x93401c20u,
			 0x93403c62u,
			 0x93407ca4u,
			 0x1e621c20u,
			 0x9ac25c20u,
			 0xd53be043u,
		};
		require(encoded.size() == std::size(expected), "new backend encodings have the wrong length");
		for (size_t index = 0; index != encoded.size(); ++index)
			require(encoded[index] == expected[index], "new backend encoding differs from its expected instruction word");
	}

	void relaxation_probe() {
		using namespace li::ir::a64;

		emitter branch;
		auto    far = branch.make_label();
		branch.b(condition::ne, far);
		branch.nops(262144);
		branch.bind(far);
		branch.ret();
		auto words = branch.words();
		require(words.size() == 262147, "far conditional branch did not relax to B.cond plus B");
		require((words[0] & 0xff00001fu) == 0x54000000u, "far conditional branch did not begin with inverted B.cond");
		require((words[1] & 0x7c000000u) == 0x14000000u, "far conditional branch is missing its long unconditional leg");

		emitter literal;
		auto    value = literal.literal64(0x0123456789abcdefull);
		literal.ldr_literal(gp_width::x64, 0, value);
		literal.nops(262144);
		literal.ret();
		auto literal_words = literal.words();
		require((literal_words[0] & 0x9f00001fu) == 0x90000000u, "far literal did not relax to ADRP/LDR");
		require(literal.relocations().size() == 2, "far literal lost typed relocation metadata");

		emitter call;
		call.bl_absolute(0x0123456789abcdefull);
		call.ret();
		auto call_words = call.words();
		require(call_words.size() == 6 && call_words[4] == 0xd63f0200u, "absolute call did not use the X16 BLR veneer");
	}

#if LI_JIT && LI_ARCH_ARM && !LI_32
	extern "C" uint64_t LI_CC mixed_native_probe(uint64_t g0, double f0, uint64_t g1, double f1, uint64_t g2, double f2, uint64_t g3, double f3, uint64_t g4,
		 double f4, uint64_t g5, double f5, uint64_t g6, double f6, uint64_t g7, double f7, uint8_t g8, float f8, uint16_t g9, double f9) {
		uint64_t integers = g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9;
		double   floating = f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9;
		return integers + static_cast<uint64_t>(floating);
	}

	li::ir::mreg x(unsigned number) { return li::ir::mreg(li::ir::arch::gp(uint8_t(number))); }
	li::ir::mreg v(unsigned number) { return li::ir::mreg(li::ir::arch::fp(uint8_t(number))); }

	void native_backend_probe() {
		using namespace li;
		using namespace li::ir;

		vm* machine = vm::create();
		require(machine != nullptr, "VM allocation failed");
		const std::array<bc::insn, 1> bytecode = {bc::insn{.o = bc::opcode::NOP}};
		function_proto*               owner    = function_proto::create(machine, bytecode, std::span<const any>{}, std::span<const line_info>{});
		procedure                     source(machine, owner);
		mprocedure                    mir;
		mir.source            = &source;
		mir.used_stack_length = 0x13000;
		mir.used_gp_mask      = arch::mask_of(arch::gp(19)) | arch::mask_of(arch::gp(20)) | arch::mask_of(arch::gp(21));
		mir.used_fp_mask      = arch::mask_of(arch::fp(8)) | arch::mask_of(arch::fp(9)) | arch::mask_of(arch::fp(10));

		mblock* entry      = mir.add_block();
		mblock* body       = mir.add_block();
		mblock* fail_block = mir.add_block();
		entry->append(vop::movi, x(9), 1);
		entry->append(vop::js, {}, x(9), int64_t(body->uid), int64_t(fail_block->uid));

		for (unsigned index = 0; index != 8; ++index) {
			body->append(vop::movi, x(index), int64_t(index + 1));
			double value = double(index) + 0.5;
			body->append(vop::movf, v(index), int64_t(li::bit_cast<uint64_t>(value)));
		}

	#if defined(__APPLE__)
		constexpr int32_t g8_offset = 0;
		constexpr int32_t f8_offset = 4;
		constexpr int32_t g9_offset = 8;
		constexpr int32_t f9_offset = 16;
	#else
		constexpr int32_t g8_offset = 0;
		constexpr int32_t f8_offset = 8;
		constexpr int32_t g9_offset = 16;
		constexpr int32_t f9_offset = 24;
	#endif
		body->append(vop::movi, x(19), 9);
		body->append(vop::storei8, {}, mmem{.base = mreg(arch::sp), .disp = g8_offset}, x(19));
		body->append_sized(vop::movf, mwidth::f32, v(8), int64_t(li::bit_cast<uint32_t>(8.5f)));
		body->append(vop::storef32, {}, mmem{.base = mreg(arch::sp), .disp = f8_offset}, v(8));
		body->append(vop::movi, x(20), 10);
		body->append(vop::storei16, {}, mmem{.base = mreg(arch::sp), .disp = g9_offset}, x(20));
		body->append(vop::movf, v(9), int64_t(li::bit_cast<uint64_t>(9.5)));
		body->append(vop::storef64, {}, mmem{.base = mreg(arch::sp), .disp = f9_offset}, v(9));

		constexpr uint64_t large = 0x123456789abcdef0ull;
		body->append(vop::movi, x(21), int64_t(large));
		for (int32_t offset = 0x100; offset != 0x400; offset += 8)
			body->append(vop::storei64, {}, mmem{.base = mreg(arch::sp), .disp = offset}, x(21));
		for (int32_t offset = 0x100; offset != 0x400; offset += 8)
			body->append(vop::loadi64, x(21), mmem{.base = mreg(arch::sp), .disp = offset});
		body->append(vop::storei64, {}, mmem{.base = mreg(arch::sp), .disp = 0x12345}, x(21));
		body->append(vop::loadi64, x(21), mmem{.base = mreg(arch::sp), .disp = 0x12345});

		constexpr uint64_t preserved_fp = 0x3ff4000000000000ull;
		body->append(vop::movf, v(10), int64_t(preserved_fp));
		body->append(vop::call, {}, int64_t(li::bit_cast<uintptr_t>(&mixed_native_probe)));
		body->append(vop::movi, x(20), v(10));
		body->append(vop::iadd, x(0), x(0), x(21));
		body->append(vop::iadd, x(0), x(0), x(20));
		body->append(vop::ret, {}, x(0));

		fail_block->append(vop::movi, x(0), 0);
		fail_block->append(vop::ret, {}, x(0));

		jfunction* generated = assemble_ir(&mir);
		require(generated != nullptr, "assemble_ir rejected full physical MIR");
		require(!generated->code_bytes().empty() && (generated->code_bytes().size() & 3) == 0, "published A64 code size is invalid");
		std::string disassembly = disassemble_code(*generated);
		require(disassembly.find("blr x16") != std::string::npos, "A64 disassembler did not decode the absolute-call veneer");
		require(disassembly.find("rax") == std::string::npos && disassembly.find("xmm") == std::string::npos, "A64 code was decoded as x86");

		const void* entry_address = generated->begin_invoke();
		require(entry_address != nullptr, "published A64 entry is unavailable");
		auto result = platform::invoke_generated_code<nfunc_t>(entry_address, machine, nullptr, 0);
		generated->end_invoke();
		constexpr uint64_t expected = 105 + large + preserved_fp;
		require(result.value == expected, "native mixed-ABI/spill probe returned the wrong value");

		rc::release(machine, generated);
		rc::release(machine, owner);
		machine->close();
	}
#endif
}

int main() {
	static_assert(li::ir::a64::abi::stack_alignment == 16);
	static_assert(li::ir::a64::abi::home_size == 0);
	static_assert(li::ir::a64::abi::gp_argument.size() == 8);
	static_assert(li::ir::a64::abi::fp_argument.size() == 8);
	static_assert((li::ir::a64::abi::reserved_gp_mask & (uint32_t{1} << 16)) != 0);
	static_assert((li::ir::a64::abi::reserved_gp_mask & (uint32_t{1} << 17)) != 0);
#if defined(__APPLE__)
	static_assert(li::ir::a64::abi::x18_reserved);
	static_assert(li::ir::a64::abi::packed_stack_arguments);
#else
	static_assert(!li::ir::a64::abi::packed_stack_arguments);
#endif

	instruction_probe();
	relaxation_probe();
#if LI_JIT && LI_ARCH_ARM && !LI_32
	native_backend_probe();
#endif
	return 0;
}
