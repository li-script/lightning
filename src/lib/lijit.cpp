#include <lib/std.hpp>
#if LI_JIT
#include <util/common.hpp>
#include <util/user.hpp>
#include <lang/parser.hpp>
#include <bit>
#include <lang/types.hpp>
#include <list>
#include <memory>
#include <vm/bc.hpp>
#include <ir/insn.hpp>
#include <ir/bc2ir.hpp>
#include <ir/ir2mir.hpp>
#include <ir/opt.hpp>
#include <ir/mir.hpp>

namespace li::lib {
	using namespace ir;

	static uint64_t jit_on(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native()) {
			return L->error("expected vfunction.");
		}

		if (!args->as_fn()->proto->jfunc) {

			bool verbose = n > 1 && args[-1].coerce_bool();

			auto proc = lift_bc(L, args->as_fn()->proto);
			opt::lift_phi(proc.get());
			opt::schedule_gc(proc.get());

			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			opt::type_split_cfg(proc.get());
			opt::type_inference(proc.get());
			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			// empty pass
			opt::type_inference(proc.get());
			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			opt::prepare_for_mir(proc.get());
			opt::type_inference(proc.get());
			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			opt::finalize_for_mir(proc.get());
			if (verbose)
				proc->print();

			auto mp = lift_ir(proc.get());

			opt::remove_redundant_setcc(mp.get());
			opt::allocate_registers(mp.get());
			if (verbose)
				mp->print();

			args->as_fn()->proto->jfunc = assemble_ir(mp.get());
			//
			//

			// hoist table fields even if it escapes
			// move stuff out of loops
			// type inference
			// trait inference
			// constant folding
			// escape analysis
			// loop analysis
			// handling of frozen tables + add builtin tables
		}

		args->as_fn()->invoke = (nfunc_t) &args->as_fn()->proto->jfunc->code[0];
		return L->ok();
	}
	static uint64_t jit_off(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native()) {
			return L->error("expected vfunction.");
		}
		args->as_fn()->invoke = &vm_invoke;
		return L->ok();
	}
	static uint64_t jit_bp(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->error("expected vfunction with JIT record.");
		}
		args->as_fn()->proto->jfunc->code[0] = 0xCC;
		return L->ok();
	}
	static uint64_t jit_where(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->ok("N/A");
		}
		return L->ok(string::format(L, "%p", &args->as_fn()->proto->jfunc->code[0]));
	}
	static uint64_t jit_disasm(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->error("expected vfunction with JIT record.");
		}

		auto*       jf     = args->as_fn()->proto->jfunc;

		std::string result = {};
		auto gen = std::span<const uint8_t>(jf->code, jf->object_bytes());
		while (auto i = zy::decode(gen)) {
			if (i->ins.mnemonic == ZYDIS_MNEMONIC_INT3)
				break;
			result += i->to_string();
			result += '\n';
		}
		return L->ok(string::create(L, result));
	}

	// Registers the JIT library.
	//
	void register_jit(vm* L) {
		util::export_as(L, "jit.on", jit_on);
		util::export_as(L, "jit.off", jit_off);
		util::export_as(L, "jit.bp", jit_bp);
		util::export_as(L, "jit.where", jit_where);
		util::export_as(L, "jit.disasm", jit_disasm);
	}
};
#endif