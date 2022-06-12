#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/function.hpp>

namespace li::lib {
	static table* LI_CC    builtin_dup_table(vm* L, table* a) { return a->duplicate(L); }
	static array* LI_CC    builtin_dup_array(vm* L, array* a) { return a->duplicate(L); }
	static function* LI_CC builtin_dup_function(vm* L, function* a) { return a->duplicate(L); }
	static array* LI_CC    builtin_new_array(vm* L, msize_t n) { return array::create(L, n, 0); }
	static table* LI_CC    builtin_new_table(vm* L, msize_t n) { return table::create(L, n); }
	
	nfunc_info detail::builtin_new_array_info = {
		 .is_pure   = false,
		 .is_const  = false,
		 .no_throw  = true,
		 .takes_vm  = true,
		 .name      = nullptr,  // Private.
		 .invoke    = nullptr,
		 .ret       = ir::type::arr,
		 .overloads = {nfunc_overload{li::bit_cast<const void*>(&builtin_new_array), {ir::type::i32}}}
	};
	nfunc_info detail::builtin_new_table_info = {
		 .is_pure   = false,
		 .is_const  = false,
		 .no_throw  = true,
		 .takes_vm  = true,
		 .name      = nullptr,  // Private.
		 .invoke    = nullptr,
		 .ret       = ir::type::tbl,
		 .overloads = {nfunc_overload{li::bit_cast<const void*>(&builtin_new_table), {ir::type::i32}}}
	};
	nfunc_info detail::builtin_dup_table_info = {
		 .is_pure   = false,
		 .is_const  = false,
		 .no_throw  = true,
		 .takes_vm  = true,
		 .name      = nullptr,  // Private.
		 .invoke    = nullptr,
		 .ret       = ir::type::tbl,
		 .overloads = {nfunc_overload{li::bit_cast<const void*>(&builtin_dup_table), {ir::type::tbl}}}
	};
	nfunc_info detail::builtin_dup_array_info = {
		 .is_pure   = false,
		 .is_const  = false,
		 .no_throw  = true,
		 .takes_vm  = true,
		 .name      = nullptr,  // Private.
		 .invoke    = nullptr,
		 .ret       = ir::type::arr,
		 .overloads = {nfunc_overload{li::bit_cast<const void*>(&builtin_dup_array), {ir::type::arr}}}
	};
	nfunc_info detail::builtin_dup_function_info = {
		 .is_pure   = false,
		 .is_const  = false,
		 .no_throw  = true,
		 .takes_vm  = true,
		 .name      = nullptr,  // Private.
		 .invoke    = nullptr,
		 .ret       = ir::type::fn,
		 .overloads = {nfunc_overload{li::bit_cast<const void*>(&builtin_dup_function), {ir::type::fn}}}
	};


	// Registers the builtins, this is called by the VM creation as it is required.
	//
	void detail::register_builtin(vm* L) {
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
			vm_stack_guard _g{L, args};
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
			vm_stack_guard _g{L, args};
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (!res.is_fn()) {
				return L->error(res);
			}
			return L->call(0, res).value;
		});
		util::export_as(L, "builtin.@table", [](vm* L, any* args, slot_t n) {
			uint16_t r = 0;
			if (n && args->is_num()) {
				r = (uint16_t) (uint64_t) std::abs(args->as_num());
			}
			return L->ok(table::create(L, r));
		});
		util::export_as(L, "builtin.assert", [](vm* L, any* args, slot_t n) {
			vm_stack_guard _g{L, args};
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
