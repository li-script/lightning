#include <ir/a64.hpp>
#include <ir/arch_a64.hpp>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>

#if defined(__aarch64__) && (defined(__APPLE__) || defined(__unix__))
	#include <sys/mman.h>
	#include <unistd.h>
#endif

namespace {
	using namespace li::ir::a64;

	[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

	void expect_words(emitter& code, std::initializer_list<uint32_t> expected, const char* name) {
		auto actual = code.words();
		if (actual.size() != expected.size())
			fail(std::string(name) + ": unexpected word count");
		size_t index = 0;
		for (uint32_t word : expected) {
			if (actual[index] != word)
				fail(std::string(name) + ": word " + std::to_string(index) + " differs");
			++index;
		}
	}

	template<typename Fn>
	void expect_invalid(Fn&& fn, const char* name) {
		try {
			fn();
		} catch (const std::invalid_argument&) {
			return;
		}
		fail(std::string(name) + ": invalid operand was accepted");
	}

	void integer_probe() {
		emitter code;
		code.add(gp_width::x64, 0, 1, 2);
		code.sub(gp_width::w32, 3, 4, 5, shift::lsl, 1);
		code.mul(gp_width::x64, 6, 7, 8);
		code.sdiv(gp_width::x64, 9, 10, 11);
		code.udiv(gp_width::w32, 12, 13, 14);
		code.bit_and(gp_width::x64, 0, 1, 2);
		code.bit_or(gp_width::x64, 0, 1, 2);
		code.bit_xor(gp_width::x64, 0, 1, 2);
		code.add_imm(gp_width::x64, 3, 4, 0x123);
		code.and_imm(gp_width::x64, 0, 1, 0xff);
		code.lsl_imm(gp_width::x64, 0, 1, 8);
		code.lsr_imm(gp_width::x64, 0, 1, 8);
		code.asr_imm(gp_width::x64, 0, 1, 8);
		code.neg(gp_width::x64, 0, 1);
		code.bit_not(gp_width::x64, 0, 1);
		code.cmp(gp_width::x64, 0, 1);
		code.csel(gp_width::x64, 0, 1, 2, condition::eq);
		code.cset(gp_width::w32, 0, condition::ne);
		expect_words(code,
			 {
				  0x8b020020,
				  0x4b050483,
				  0x9b087ce6,
				  0x9acb0d49,
				  0x1ace09ac,
				  0x8a020020,
				  0xaa020020,
				  0xca020020,
				  0x91048c83,
				  0x92401c20,
				  0xd378dc20,
				  0xd348fc20,
				  0x9348fc20,
				  0xcb0103e0,
				  0xaa2103e0,
				  0xeb01001f,
				  0x9a820020,
				  0x1a9f07e0,
			 },
			 "integer encodings");
	}

	void floating_probe() {
		emitter code;
		code.fadd(fp_width::d64, 0, 1, 2);
		code.fsub(fp_width::s32, 3, 4, 5);
		code.fmul(fp_width::d64, 6, 7, 8);
		code.fdiv(fp_width::s32, 9, 10, 11);
		code.fabs(fp_width::d64, 0, 1);
		code.fneg(fp_width::s32, 2, 3);
		code.fsqrt(fp_width::d64, 4, 5);
		code.frint(fp_width::d64, 6, 7, fp_round::toward_zero);
		code.fmin(fp_width::d64, 8, 9, 10);
		code.fmax(fp_width::s32, 11, 12, 13);
		code.fcmp(fp_width::d64, 0, 1);
		code.scvtf(fp_width::d64, gp_width::x64, 0, 1);
		code.ucvtf(fp_width::s32, gp_width::w32, 2, 3);
		code.fcvtzs(gp_width::x64, fp_width::d64, 4, 5);
		code.fcvtzu(gp_width::w32, fp_width::s32, 6, 7);
		code.fmov_from_gp(fp_width::d64, 8, gp_width::x64, 9);
		code.fmov_to_gp(gp_width::w32, 10, fp_width::s32, 11);
		expect_words(code,
			 {
				  0x1e622820,
				  0x1e253883,
				  0x1e6808e6,
				  0x1e2b1949,
				  0x1e60c020,
				  0x1e214062,
				  0x1e61c0a4,
				  0x1e65c0e6,
				  0x1e6a5928,
				  0x1e2d498b,
				  0x1e612000,
				  0x9e620020,
				  0x1e230062,
				  0x9e7800a4,
				  0x1e3900e6,
				  0x9e670128,
				  0x1e26016a,
			 },
			 "floating encodings");
	}

	void memory_probe() {
		emitter code;
		code.ldr(memory_size::u64, 0, 1, 16);
		code.str(memory_size::u32, 2, 3, -4);
		code.ldr(memory_size::u64, 0, 1, 2, index_extend::lsl, 3);
		code.ldr(fp_width::d64, 0, 1, 16);
		code.str(fp_width::s32, 2, 3, -4);
		code.stp(gp_width::x64, 29, 30, 31, -16, pair_mode::pre_index);
		code.ldp(gp_width::x64, 29, 30, 31, 16, pair_mode::post_index);
		code.stp(fp_width::d64, 8, 9, 31, -16, pair_mode::pre_index);
		expect_words(code,
			 {
				  0xf9400820,
				  0xb81fc062,
				  0xf8627820,
				  0xfd400820,
				  0xbc1fc062,
				  0xa9bf7bfd,
				  0xa8c17bfd,
				  0x6dbf27e8,
			 },
			 "memory encodings");
	}

	void control_flow_probe() {
		emitter direct;
		direct.b(8);
		direct.bl(-4);
		direct.b(condition::eq, 8);
		direct.cbz(gp_width::x64, 0, 8);
		direct.cbnz(gp_width::w32, 1, -4);
		direct.br(16);
		direct.blr(16);
		direct.ret();
		direct.brk(7);
		expect_words(direct,
			 {
				  0x14000002,
				  0x97ffffff,
				  0x54000040,
				  0xb4000040,
				  0x35ffffe1,
				  0xd61f0200,
				  0xd63f0200,
				  0xd65f03c0,
				  0xd42000e0,
			 },
			 "control-flow encodings");

		emitter labels;
		auto    target = labels.make_label();
		labels.b(target);
		labels.nop();
		labels.bind(target);
		labels.ret();
		expect_words(labels, {0x14000002, 0xd503201f, 0xd65f03c0}, "near label fixup");

		emitter relaxed;
		auto    far_target = relaxed.make_label();
		relaxed.b(condition::eq, far_target);
		relaxed.nops(262143);
		relaxed.bind(far_target);
		auto words = relaxed.words();
		if (words[0] != 0x54000041 || words[1] != 0x14040000)
			fail("out-of-range conditional branch was not relaxed");
		if (relaxed.relocations().size() != 1 || relaxed.relocations()[0].kind != emitter::relocation_kind::branch26)
			fail("relaxed conditional branch relocation metadata differs");

		emitter absolute;
		absolute.b_absolute(0x1122334455667788ull);
		expect_words(absolute,
			 {
				  0xd28ef110,
				  0xf2aaacd0,
				  0xf2c66890,
				  0xf2e22450,
				  0xd61f0200,
			 },
			 "absolute branch veneer");
		if (absolute.relocations().size() != 4)
			fail("absolute veneer must expose four typed move-wide relocations");
	}

	void literal_probe() {
		emitter nearby;
		auto    value = nearby.literal64(0x1122334455667788ull);
		nearby.ldr_literal(gp_width::x64, 0, value);
		expect_words(nearby,
			 {
				  0x58000040,
				  0x14000003,
				  0x55667788,
				  0x11223344,
			 },
			 "near literal pool");
		auto bytes = nearby.bytes();
		if (bytes[0] != 0x40 || bytes[1] != 0x00 || bytes[2] != 0x00 || bytes[3] != 0x58)
			fail("encoder byte stream is not little endian");

		emitter distant;
		auto    distant_value = distant.literal64(0x0123456789abcdefull);
		distant.ldr_literal(gp_width::x64, 0, distant_value);
		distant.nops(262143);
		auto distant_words = distant.words();
		if (distant_words[0] != 0x90000800 || distant_words[1] != 0xf9400400)
			fail("out-of-range literal was not expanded to ADRP/LDR");
		if (distant.relocations().size() != 2 || distant.relocations()[0].kind != emitter::relocation_kind::adr_page21 ||
			 distant.relocations()[1].kind != emitter::relocation_kind::load_pageoff12)
			fail("long literal relocation metadata differs");
	}

	void validation_probe() {
		expect_invalid(
			 [] {
				 emitter code;
				 code.add(gp_width::x64, 32, 0, 1);
			 },
			 "register range");
		expect_invalid(
			 [] {
				 emitter code;
				 code.add_imm(gp_width::x64, 0, 1, 4096);
			 },
			 "add immediate range");
		expect_invalid(
			 [] {
				 emitter code;
				 code.stp(gp_width::x64, 0, 1, 31, 4);
			 },
			 "pair alignment");
		expect_invalid(
			 [] {
				 emitter code;
				 code.ldr(memory_size::u64, 0, 1, 2, index_extend::lsl, 2);
			 },
			 "register offset shift");
		expect_invalid(
			 [] {
				 emitter code;
				 code.b(condition::eq, int64_t{1} << 21);
			 },
			 "conditional branch range");
		expect_invalid(
			 [] {
				 emitter code;
				 code.movz(gp_width::w32, 0, 1, 32);
			 },
			 "move-wide shift");

		emitter unbound;
		unbound.b(unbound.make_label());
		try {
			(void) unbound.words();
		} catch (const std::logic_error&) {
			return;
		}
		fail("unbound A64 label was accepted");
	}

#if defined(__aarch64__) && (defined(__APPLE__) || defined(__unix__))
	void hardware_probe() {
		emitter code;
		code.movz(gp_width::x64, 0, 40);
		code.movz(gp_width::x64, 1, 2);
		code.add(gp_width::x64, 0, 0, 1);
		code.ret();
		auto bytes     = code.bytes();
		long page_size = sysconf(_SC_PAGESIZE);
		if (page_size <= 0)
			fail("cannot determine page size for A64 execution probe");
		void* memory = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (memory == MAP_FAILED)
			fail("mmap failed for A64 execution probe");
		std::memcpy(memory, bytes.data(), bytes.size());
		if (mprotect(memory, static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
			munmap(memory, static_cast<size_t>(page_size));
			fail("mprotect failed for A64 execution probe");
		}
		__builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + bytes.size());
		using function  = uint64_t (*)();
		uint64_t result = reinterpret_cast<function>(memory)();
		munmap(memory, static_cast<size_t>(page_size));
		if (result != 42)
			fail("A64 execution probe returned the wrong value");
	}
#endif
}

int main() {
	static_assert(li::ir::a64::abi::home_size == 0);
	static_assert(li::ir::a64::abi::stack_alignment == 16);
	static_assert(li::ir::a64::abi::gp_argument[7] == 7);
	static_assert(li::ir::a64::abi::fp_argument[7] == 7);
	static_assert((li::ir::a64::abi::reserved_gp_mask & (uint32_t{1} << 16)) != 0);
#if defined(__APPLE__)
	static_assert(li::ir::a64::abi::x18_reserved);
	static_assert(li::ir::a64::abi::packed_stack_arguments);
#endif

	integer_probe();
	floating_probe();
	memory_probe();
	control_flow_probe();
	literal_probe();
	validation_probe();
#if defined(__aarch64__) && (defined(__APPLE__) || defined(__unix__))
	hardware_probe();
#endif
	return 0;
}
