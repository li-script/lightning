#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/function.hpp>

namespace li::lib {
	// Registers the builtins, this is called by the VM creation as it is required.
	//
	void detail::register_builtin(vm* L) {
		util::export_as(L, "builtin.pcall", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};
			if (n < 2) {
				return L->error("expected 2 or more arguments.");
			}
			auto f = args[0];
			if (!f.is_fn()) {
				args[-1] = any(false);
				return L->error("invoking non-function");
			}

			auto apos = &args[0] - L->stack;
			for (slot_t i = -(n - 1); i < -1; i++)
				L->push_stack(L->stack[apos + i]);

			bool res           = L->call(n - 2, f);
			L->stack[apos - 1] = res;
			return L->ok(L->pop_stack());
		});
		util::export_as(L, "builtin.print", [](vm* L, any* args, slot_t n) {
			for (int32_t i = 0; i != n; i++) {
				if (args[-i].is_traitful() && ((traitful_node<>*) args[-i].as_gc())->has_trait<trait::str>()) [[unlikely]] {
					fputs(args[-i].to_string(L)->c_str(), stdout);
				} else {
					args[-i].print();
				}
				printf("\t");
			}
			printf("\n");
			return L->ok();
		});
		util::export_as(L, "builtin.loadstring", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (!res.is_fn())
				return L->error(res);
			else
				return L->ok(res);
		});
		util::export_as(L, "builtin.eval", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (!res.is_fn()) {
				return L->error(res);
			}
			if (L->call(0, res))
				return L->ok(L->pop_stack());
			else
				return L->error(L->pop_stack());
		});
		util::export_as(L, "builtin.@table", [](vm* L, any* args, slot_t n) {
			uint16_t r = 0;
			if (n && args->is_num()) {
				r = (uint16_t) (uint64_t) std::abs(args->as_num());
			}
			return L->ok(table::create(L, r));
		});
		util::export_as(L, "builtin.assert", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};
			if (!n || args->coerce_bool())
				return L->ok();

			if (n >= 2 && args[-1].is_str()) {
				return L->error(args[-1]);
			} else {
				call_frame  frame = L->last_vm_caller;
				const char* fn    = "C";
				msize_t     line  = 0;
				if (frame.stack_pos >= FRAME_SIZE) {
					auto& target = L->stack[frame.stack_pos + FRAME_TARGET];
					if (target.is_fn() && target.as_fn()->is_virtual()) {
						auto vfn = target.as_fn();
						fn       = vfn->proto->src_chunk->c_str();
						line     = vfn->proto->lookup_line(frame.caller_pc & ~FRAME_C_FLAG);
					}
				}
				return L->error("assertion failed at %s, line %u", fn, line);
			}
		});
	}
};
