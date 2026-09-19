#include <lib/std.hpp>
#if LI_JIT
	#include <bit>
	#include <chrono>
	#include <cstdint>
	#include <cstdlib>
	#include <ir/bc2ir.hpp>
	#include <ir/insn.hpp>
	#include <ir/ir2mir.hpp>
	#include <ir/mir.hpp>
	#include <ir/opt.hpp>
	#include <ir/ownership.hpp>
	#include <lang/parser.hpp>
	#include <list>
	#include <memory>
	#include <string_view>
	#include <util/common.hpp>
	#include <util/user.hpp>
	#include <vm/bc.hpp>
	#include <vm/shared.hpp>
	#include <vm/tier.hpp>
	#include <vm/types.hpp>

namespace li::lib {
	using namespace ir;

	// Optional-pass controls keep benchmark and differential comparisons on one binary.
	static bool pass_enabled(std::string_view name) {
		const char* disabled = std::getenv("LI_JIT_DISABLE");
		if (!disabled)
			return true;
		std::string_view remaining{disabled};
		while (!remaining.empty()) {
			auto separator = remaining.find(',');
			auto item      = remaining.substr(0, separator);
			if (item == name || item == "all")
				return false;
			if (separator == std::string_view::npos)
				break;
			remaining.remove_prefix(separator + 1);
		}
		return true;
	}

	struct jit_compile_timer {
		vm*                                   L;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

		~jit_compile_timer() {
			auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
			L->jit_compile_ns += uint64_t(elapsed.count());
		}
	};

	static const char* unsupported_opcode(function_proto* f) {
		for (const auto& insn : f->opcodes()) {
			switch (insn.o) {
				case bc::UD:
					return bc::opcode_details(insn.o).name;
				case bc::NOP:
				case bc::LNOT:
				case bc::ANEG:
				case bc::AADD:
				case bc::ASUB:
				case bc::AMUL:
				case bc::ADIV:
				case bc::AMOD:
				case bc::APOW:
				case bc::LAND:
				case bc::LOR:
				case bc::NCS:
				case bc::CTY:
				case bc::CTYX:
				case bc::CTYID:
				case bc::CEQ:
				case bc::CNE:
				case bc::CLT:
				case bc::CGE:
				case bc::CGT:
				case bc::CLE:
				case bc::CCAT:
				case bc::SETEH:
				case bc::SETEX:
				case bc::GETEX:
				case bc::KIMM:
				case bc::UGET:
				case bc::USET:
				case bc::STRIV:
				case bc::SGET:
				case bc::SSET:
				case bc::VACNT:
				case bc::VACHK:
				case bc::VAGET:
				case bc::ANEW:
				case bc::TNEW:
				case bc::TGET:
				case bc::TSET:
				case bc::TGETR:
				case bc::TSETR:
				case bc::FDUP:
				case bc::PUSHR:
				case bc::PUSHI:
				case bc::TOBOOL:
				case bc::CALL:
				case bc::RET:
				case bc::JMP:
				case bc::JS:
				case bc::JNS:
				case bc::ITER:
				case bc::MOV:
					break;
				default:
					return bc::opcode_details(insn.o).name;
			}
		}
		return nullptr;
	}

	// Turns jit on or off.
	//
	static std::optional<std::string> compile_and_enable_jit(vm* L, function* f, bool verbose) {
		if (!f->proto->load_jfunc()) {
			jit_compile_timer timer{L};
			if (const char* opcode = unsupported_opcode(f->proto))
				return std::string(opcode);
			// Stage names print in verbose mode so a verifier failure is attributable.
			auto stage = [&](const char* name) {
				if (verbose)
					printf("-- stage %s\n", name);
			};
			stage("lift_bc");
			auto proc          = lift_bc(L, f->proto, tier::pending_osr_target());
			proc->cache_fields = pass_enabled("ic");
			stage("lift_phi");
			opt::lift_phi(proc.get());

			auto fold_identical = [&] {
				if (pass_enabled("cse")) {
					stage("fold_identical");
					opt::fold_identical(proc.get());
				}
			};
			auto cleanup = [&] {
				stage("fold_constant");
				opt::fold_constant(proc.get());
				fold_identical();
				stage("dce");
				opt::dce(proc.get());
				stage("cfg");
				opt::cfg(proc.get());
			};
			cleanup();

			stage("inline_calls");
			const auto inlined = pass_enabled("inline") ? opt::inline_calls(proc.get()) : 0;
			stage("scalar_replace");
			const auto scalarized = pass_enabled("escape") ? opt::scalar_replace(proc.get()) : 0;
			stage("fold_constant");
			opt::fold_constant(proc.get());
			stage("dce");
			opt::dce(proc.get());
			stage("cfg");
			opt::cfg(proc.get());

			stage("type_split_cfg");
			opt::type_split_cfg(proc.get());
			stage("type_inference");
			opt::type_inference(proc.get());
			cleanup();

			stage("type_inference");
			opt::type_inference(proc.get());
			stage("specialize_integer_ranges");
			const auto specialized = pass_enabled("ranges") ? opt::specialize_integer_ranges(proc.get()) : 0;
			if (pass_enabled("licm")) {
				stage("licm");
				opt::licm(proc.get());
			}
			cleanup();

			stage("ownership");
			auto rc_stats = opt::ownership(proc.get());
			stage("prepare_for_mir");
			opt::prepare_for_mir(proc.get());
			stage("finalize_for_mir");
			opt::finalize_for_mir(proc.get());
			uint32_t guard_count = 0;
			for (const auto& block : proc->basic_blocks) {
				for (const auto* instruction : block->insns()) {
					if (!instruction->is<test_type>())
						continue;
					const auto source = instruction->source_bc;
					if (source < f->proto->length && f->proto->opcode_array[source].o == bc::CTY)
						continue;
					++guard_count;
				}
			}
			if (verbose) {
				printf("-- Optimizations inlined=%zu scalarized=%zu integer=%zu\n", inlined, scalarized, specialized);
				printf("-- RC retains=%zu releases=%zu moves=%zu elided=%zu/%zu pairs=%zu edges=%zu ops=%zu->%zu\n", rc_stats.retains_inserted,
					 rc_stats.releases_inserted, rc_stats.ownership_moves, rc_stats.retains_elided, rc_stats.releases_elided, rc_stats.pairs_elided,
					 rc_stats.edges_split, rc_stats.rc_ops_before_elision, rc_stats.rc_ops_after_elision);
				proc->print();
			}

			auto mp = lift_ir(proc.get());

			opt::remove_redundant_setcc(mp.get());
			opt::allocate_registers(mp.get());
			if (verbose)
				mp->print();

			auto* compiled = assemble_ir(mp.get());
			if (!compiled) {
				if (!mp->assembly_error.empty())
					return mp->assembly_error;
				return std::string("native code generation failed without a backend diagnostic");
			}

			// Shared prototypes are guarded by jit_on and become visible only after
			// native compilation has completed successfully.
			f->proto->publish_jfunc(compiled);
			f->proto->jit_guards = guard_count;
			L->generated_code_bytes += uint64_t(compiled->code_bytes().size());
			L->jit_spill_slots += uint64_t(mp->spill_slot_count());
			L->jit_guards += guard_count;
			L->jit_compiled++;
		}

		if (!f->shared)
			f->invoke = &jit_dispatch;
		return std::nullopt;
	}

	// Keeps the last compilation diagnostic on the prototype so jit.disasm can
	// explain why no code exists. Shared prototypes only store shared strings.
	static void record_jit_rejection(vm* L, function_proto* proto, const std::optional<std::string>& error) {
		string* diagnostic = error ? string::create(L, *error) : nullptr;
		if (diagnostic && proto->shared) {
			string* shared_diagnostic = shared::copy_string(L, diagnostic);
			rc::release(L, diagnostic);
			diagnostic = shared_diagnostic;
		}
		string* previous = std::exchange(proto->jit_rejection, diagnostic);
		rc::release(L, previous);
	}

	std::optional<std::string> jit_on(vm* L, function* f, bool verbose) {
		(void) tier::enable(f);
		std::optional<std::string> error;
		if (f->proto->shared) {
			shared::recursive_guard guard(L, f->proto);
			error = compile_and_enable_jit(L, f, verbose);
			record_jit_rejection(L, f->proto, error);
		} else {
			error = compile_and_enable_jit(L, f, verbose);
			record_jit_rejection(L, f->proto, error);
		}
		return error;
	}
	void jit_off(vm* L, function* f) {
		if (tier::disable(f))
			f->invoke = &vm_invoke;
	}

	static any_t jit_on_v(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native()) {
			return L->error("expected vfunction.");
		}
		bool verbose = n > 1 && args[-1].coerce_bool();
		if (auto error = jit_on(L, args->as_fn(), verbose))
			return L->error("JIT compilation failed (unsupported opcode or backend operation: %s).", error->c_str());
		return L->ok();
	}
	static any_t jit_off_v(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native()) {
			return L->error("expected vfunction.");
		}
		function* f = args->as_fn();
		if (f->shared)
			return L->error("cannot disable JIT code for a shared function.");
		jit_off(L, f);
		return L->ok();
	}
	static any_t jit_bp(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->error("expected vfunction with JIT record.");
		}
		function*  f  = args->as_fn();
		jfunction* jf = f->proto->load_jfunc();
		if (!jf)
			return L->error("expected vfunction with JIT record.");
		if (f->shared || f->proto->shared || jf->shared)
			return L->error("cannot publish a breakpoint into shared JIT code.");
		if (auto error = jf->publish_breakpoint())
			return L->error("cannot publish JIT breakpoint: %s", error.message().c_str());
		return L->ok();
	}
	static any_t jit_where(vm* L, any* args, slot_t n) {
		jfunction* jf = nullptr;
		if (args->is_fn() && !args->as_fn()->is_native() && args->as_fn()->is_jit())
			jf = args->as_fn()->proto->load_jfunc();
		if (!jf)
			return L->take(any(string::create(L, "N/A")));
		return L->take(any(string::format(L, "%p", const_cast<void*>(jf->entry_address()))));
	}
	static any_t jit_guards(vm* L, any* args, slot_t n) {
		if (n != 1 || !args->is_fn() || args->as_fn()->is_native())
			return L->error("expected vfunction.");
		function_proto* proto = args->as_fn()->proto;
		if (!proto->load_jfunc())
			return L->error("expected vfunction with JIT record.");
		return L->ok(number(proto->jit_guards));
	}
	static any_t jit_disasm(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native())
			return L->error("expected vfunction.");
		function_proto* proto = args->as_fn()->proto;
		if (auto* jf = args->as_fn()->is_jit() ? proto->load_jfunc() : nullptr)
			return L->take(any(string::create(L, disassemble_code(*jf))));
		// Rejection publication is guarded by the prototype lock for shared code.
		shared::recursive_guard guard(L, proto);
		if (proto->jit_rejection)
			return L->ok(any(proto->jit_rejection));
		return L->error("expected vfunction with JIT record or rejection diagnostic.");
	}

	// Registers the JIT library.
	//
	void register_jit(vm* L) {
		util::export_as(L, "jit.on", jit_on_v);
		util::export_as(L, "jit.off", jit_off_v);
		util::export_as(L, "jit.bp", jit_bp);
		util::export_as(L, "jit.where", jit_where);
		util::export_as(L, "jit.guards", jit_guards);
		util::export_as(L, "jit.disasm", jit_disasm);
	}
};
#endif