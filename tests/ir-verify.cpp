#include <ir/proc.hpp>
#include <ir/verify.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {
	using namespace li;
	using namespace li::ir;

	void require(bool condition, std::string_view message) {
		if (!condition)
			throw std::runtime_error(std::string(message));
	}

	void expect_error(procedure& proc, const insn* offending, std::string_view category) {
		auto error = verify_ir(proc);
		require(error.has_value(), "malformed procedure was accepted");
		require(error->instruction == offending, "verifier did not identify the offending instruction");
		require(error->message.find(category) != std::string::npos, "verifier reported the wrong violation category");
		require(error->describe().find("instruction") != std::string::npos, "diagnostic omitted instruction context");
	}

	void valid_probe() {
		procedure proc(nullptr, nullptr);
		auto*     block = proc.add_block();
		builder   b{block};
		auto      retained = b.emit<retain>((array*) nullptr);
		b.emit<release>(retained);
		b.emit<ret>(int32_t(0));
		require(!verify_ir(proc), "valid procedure was rejected");
		proc.validate();
	}

	void valid_phi_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     left  = proc.add_block();
		auto*     right = proc.add_block();
		auto*     join  = proc.add_block();
		proc.add_jump(entry, left);
		proc.add_jump(entry, right);
		proc.add_jump(left, join);
		proc.add_jump(right, join);
		builder{entry}.emit<jcc>(true, left, right);
		auto left_value = builder{left}.emit<move>(int32_t(1));
		builder{left}.emit<jmp>(join);
		auto right_value = builder{right}.emit<move>(int32_t(2));
		builder{right}.emit<jmp>(join);
		auto merged = builder{join}.emit<phi>(left_value, right_value);
		builder{join}.emit<ret>(merged);
		require(!verify_ir(proc), "valid phi-edge definitions were rejected");
	}

	void valid_loop_phi_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry  = proc.add_block();
		auto*     header = proc.add_block();
		auto*     body   = proc.add_block();
		auto*     exit   = proc.add_block();
		proc.add_jump(entry, header);
		proc.add_jump(header, body);
		proc.add_jump(header, exit);
		proc.add_jump(body, header);
		auto initial = builder{entry}.emit<move>(int32_t(0));
		builder{entry}.emit<jmp>(header);
		auto induction = builder{header}.emit<phi>(initial, initial);
		builder{header}.emit<jcc>(true, body, exit);
		auto next = builder{body}.emit<move>(induction);
		builder{body}.emit<jmp>(header);
		induction->operands[1] = next;
		builder{exit}.emit<ret>(induction);
		require(!verify_ir(proc), "valid loop phi was rejected");
	}

	void cfg_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     exit  = proc.add_block();
		proc.add_jump(entry, exit);
		auto terminator = builder{entry}.emit<jmp>(exit);
		builder{exit}.emit<ret>(int32_t(0));
		entry->successors[0] = entry;
		expect_error(proc, terminator.get(), "target does not match successor");
	}

	void dominance_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     left  = proc.add_block();
		auto*     right = proc.add_block();
		auto*     join  = proc.add_block();
		proc.add_jump(entry, left);
		proc.add_jump(entry, right);
		proc.add_jump(left, join);
		proc.add_jump(right, join);
		builder{entry}.emit<jcc>(true, left, right);
		auto definition = builder{left}.emit<move>(int32_t(7));
		builder{left}.emit<jmp>(join);
		builder{right}.emit<jmp>(join);
		auto use = builder{join}.emit<ret>(definition);
		expect_error(proc, use.get(), "does not dominate");
	}

	void phi_arity_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     join  = proc.add_block();
		proc.add_jump(entry, join);
		builder{entry}.emit<jmp>(join);
		auto incoming = builder{join}.emit<phi>();
		builder{join}.emit<ret>(incoming);
		expect_error(proc, incoming.get(), "operand count");
	}

	void phi_placement_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     join  = proc.add_block();
		proc.add_jump(entry, join);
		auto incoming = builder{entry}.emit<move>(int32_t(3));
		builder{entry}.emit<jmp>(join);
		builder{join}.emit<move>(int32_t(4));
		auto misplaced = builder{join}.emit<phi>(incoming);
		builder{join}.emit<ret>(misplaced);
		expect_error(proc, misplaced.get(), "phi appears after");
	}

	void operand_type_probe() {
		procedure proc(nullptr, nullptr);
		auto*     block = proc.add_block();
		builder   b{block};
		auto      malformed = b.create<array_new>(&proc, true);
		block->push_back(malformed);
		b.emit<ret>(malformed);
		expect_error(proc, malformed.get(), "wrong type");
	}

	void orphan_probe() {
		procedure proc(nullptr, nullptr);
		auto*     block = proc.add_block();
		builder   b{block};
		auto      orphan = b.create<move>(&proc, int32_t(1));
		orphan->update();
		b.emit<ret>(orphan);
		expect_error(proc, orphan.get(), "orphaned");
	}

	void ownership_probe() {
		procedure proc(nullptr, nullptr);
		auto*     block = proc.add_block();
		builder   b{block};
		auto      retained = b.emit<retain>((array*) nullptr);
		b.emit<release>(retained);
		auto duplicate = b.emit<release>(retained);
		b.emit<ret>(int32_t(0));
		expect_error(proc, duplicate.get(), "used after it was consumed");
	}

	void ownership_leak_probe() {
		procedure proc(nullptr, nullptr);
		auto*     block = proc.add_block();
		builder   b{block};
		auto      leaked   = b.emit<array_new>(int32_t(0));
		auto      retained = b.emit<retain>((array*) nullptr);
		b.emit<release>(retained);
		b.emit<ret>(int32_t(0));
		expect_error(proc, leaked.get(), "neither consumed nor released");
	}

	void effects_probe() {
		procedure proc(nullptr, nullptr);
		auto*     block = proc.add_block();
		builder   b{block};
		auto      malformed = b.emit<move>(int32_t(1));
		malformed->effects  = effect_may_call_user;
		b.emit<ret>(malformed);
		expect_error(proc, malformed.get(), "effect_may_call_user");
	}

	void scale_probe() {
		procedure proc(nullptr, nullptr);
		auto*     previous = proc.add_block();
		for (size_t index = 0; index != 2048; ++index) {
			auto* next = proc.add_block();
			proc.add_jump(previous, next);
			builder{previous}.emit<jmp>(next);
			previous = next;
		}
		builder{previous}.emit<ret>(int32_t(0));
		require(!verify_ir(proc), "large linear procedure was rejected");
	}
}

int main() {
	valid_probe();
	valid_phi_probe();
	valid_loop_phi_probe();
	cfg_probe();
	dominance_probe();
	phi_arity_probe();
	phi_placement_probe();
	operand_type_probe();
	orphan_probe();
	ownership_probe();
	ownership_leak_probe();
	effects_probe();
	scale_probe();
	return 0;
}
