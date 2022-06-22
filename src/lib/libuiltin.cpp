#include <cmath>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/object.hpp>

// Include arch-specific header if relevant for optimizations.
//
#if LI_JIT && LI_ARCH_X86 && !LI_32
	#include <ir/x86-64.hpp>
#endif

namespace li::lib {
	static array* LI_CC builtin_new_array_i32(vm* L, msize_t n) { return array::create(L, n, 0); }
	static table* LI_CC builtin_new_table_i32(vm* L, msize_t n) { return table::create(L, n); }

	static any_t LI_CC builtin_null_functor(vm* L) {
		return L->error("invoking uninitialized function");
	}
	static any_t       builtin_null_functor_vm(vm* L, any* args, slot_t nargs) { return builtin_null_functor(L); }

	static table* LI_CC    builtin_dup_table(vm* L, table* a) { return a->duplicate(L); }
	static array* LI_CC    builtin_dup_array(vm* L, array* a) { return a->duplicate(L); }
	static function* LI_CC builtin_dup_function(vm* L, function* a) { return a->duplicate(L); }
	static object* LI_CC   builtin_dup_object(vm* L, object* a) { return a->duplicate(L); }
	static any_t LI_CC     builtin_dup_else(vm* L, any_t v) { return v; }

	static any_t builtin_dup_vm(vm* L, any* args, slot_t nargs) {
		any a = args[1];
		if (a.is_arr()) {
			return any(builtin_dup_array(L, a.as_arr()));
		} else if (a.is_tbl()) {
			return any(builtin_dup_table(L, a.as_tbl()));
		} else if (a.is_fn()) {
			return any(builtin_dup_function(L, a.as_fn()));
		} else if (a.is_obj()) {
			return any(builtin_dup_object(L, a.as_obj()));
		} else {
			return builtin_dup_else(L, a);
		}
	}

	static msize_t LI_CC builtin_len_array(vm* L, array* a) { return a->length; }
	static msize_t LI_CC builtin_len_table(vm* L, table* t) { return t->active_count; }
	static msize_t LI_CC builtin_len_string(vm* L, string* s) { return s->length; }
	static any_t LI_CC   builtin_len_else(vm* L, any_t a) { return L->error("expected iterable"); }

	static any_t builtin_len_vm(vm* L, any* args, slot_t nargs) {
		any a = args[1];
		if (a.is_arr()) {
			return any((number) builtin_len_array(L, a.as_arr()));
		} else if (a.is_tbl()) {
			return any((number) builtin_len_table(L, a.as_tbl()));
		} else if (a.is_str()) {
			return any((number) builtin_len_string(L, a.as_str()));
		} else {
			return builtin_len_else(L, a);
		}
	}

	static string* LI_CC builtin_str_coerce(vm* L, any_t v) { return v.coerce_str(L); }
	static number LI_CC  builtin_num_coerce(any_t v) { return v.coerce_num(); }
	static int32_t LI_CC builtin_int_coerce(any_t v) { return (int32_t) builtin_num_coerce(v); }

	static any_t builtin_str_vm(vm* L, any* args, slot_t nargs) { return any(builtin_str_coerce(L, args[1])); }
	static any_t builtin_num_vm(vm* L, any* args, slot_t nargs) { return any(builtin_num_coerce(args[1])); }
	static any_t builtin_int_vm(vm* L, any* args, slot_t nargs) { return any(trunc(builtin_num_coerce(args[1]))); }

	static table* LI_CC builtin_join_table(vm* L, table* dst, table* src) {
		dst->join(L, src);
		return dst;
	}
	static array* LI_CC builtin_join_array(vm* L, array* dst, array* src) {
		dst->join(L, src);
		return dst;
	}
	static string* LI_CC builtin_join_string(vm* L, string* dst, string* src) { return string::concat(L, dst, src); }
	static any_t LI_CC   builtin_join_else(vm* L, any_t dst, any_t src) {
		  if (dst.type() != src.type()) {
			  return L->error("cannot join different types");
      }
		  return L->error("join expected table, array, or string");
	}
	static any_t builtin_join_vm(vm* L, any* args, slot_t nargs) {
		if (nargs < 1) {
			return L->error("join expects 1 argument");
		}

		any src = args[0];
		any dst = args[1];
		if (src.type() == dst.type()) {
			if (src.is_arr()) {
				return any(builtin_join_array(L, dst.as_arr(), src.as_arr()));
			} else if (src.is_tbl()) {
				return any(builtin_join_table(L, dst.as_tbl(), src.as_tbl()));
			} else if (src.is_str()) {
				return any(builtin_join_string(L, dst.as_str(), src.as_str()));
			}
		}
		return builtin_join_else(L, dst, src);
	}

	static void LI_CC  builtin_push_array(vm* L, array* dst, any_t val) { dst->push(L, val); }
	static any_t LI_CC builtin_push_else(vm* L) { return L->error("push expected array"); }

	static any_t builtin_push_vm(vm* L, any* args, slot_t nargs) {
		if (nargs < 1) {
			return L->error("push expects 1 argument");
		}
		any val = args[0];
		any dst = args[1];
		if (dst.is_arr()) {
			builtin_push_array(L, dst.as_arr(), val);
			return nil;
		}
		return builtin_push_else(L);
	}

	static any_t LI_CC builtin_pop_array(vm* L, array* dst) { return dst->pop(); }
	static any_t LI_CC builtin_pop_else(vm* L) { return L->error("pop expected array"); }

	static any_t builtin_pop_vm(vm* L, any* args, slot_t nargs) {
		any dst = args[1];
		if (dst.is_arr()) {
			return builtin_pop_array(L, dst.as_arr());
		}
		return builtin_pop_else(L);
	}

	static bool LI_CC builtin_in_arr_unk(vm* L, array* i, any_t v) {
		for (auto& k : *i)
			if (k == v)
				return true;
		return false;
	}
	static bool LI_CC  builtin_in_tbl_unk(vm* L, table* i, any_t v) { return v != nil && i->get(L, v) != nil; }
	static bool LI_CC  builtin_in_str_num(vm*, string* i, uint32_t v) { return v <= 0xFF && i->view().find((char) (v & 0xFF)) != std::string::npos; }
	static bool LI_CC  builtin_in_str_str(vm*, string* i, string* v) { return i == v || i->view().find(v->view()) != std::string::npos; }
	static any_t LI_CC builtin_in_else(vm* L, any_t iv) {
		if (iv.is_str()) {
			return L->error("expected string or character");
		} else {
			return L->error("expected iterable");
		}
	}

	static any_t builtin_in_vm(vm* L, any* args, slot_t nargs) {
		if (nargs < 1) {
			return L->error("in expects 1 argument");
		}

		any v = args[0];
		any i = args[1];
		if (i.is_str()) {
			if (v.is_str())
				return any(builtin_in_str_str(L, i.as_str(), v.as_str()));
			else if (v.is_num())
				return any(builtin_in_str_num(L, i.as_str(), (uint32_t) v.as_num()));
		} else if (i.is_tbl()) {
			return any(builtin_in_tbl_unk(L, i.as_tbl(), v));
		} else if (i.is_arr()) {
			return any(builtin_in_arr_unk(L, i.as_arr(), v));
		}
		return builtin_in_else(L, i);
	}

	util::native_function detail::builtin_in = {
		 func_attr_pure | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.in",
		 &builtin_in_vm,

		 {
			  nfunc_overload{li::bit_cast<const void*>(&builtin_in_arr_unk), {type::arr, type::any}, type::i1},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_in_tbl_unk), {type::tbl, type::any}, type::i1},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_in_str_num), {type::str, type::i32}, type::i1},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_in_str_str), {type::str, type::str}, type::i1},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_in_else), {type::any}, type::exc},
		 },
	};
	util::native_function detail::builtin_push = {
		 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.push",
		 &builtin_push_vm,

		 {
			  nfunc_overload{li::bit_cast<const void*>(&builtin_push_array), {type::arr, type::any}, type::none},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_push_else), {}, type::exc},
		 },
	};
	util::native_function detail::builtin_pop = {
		 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.pop",
		 &builtin_pop_vm,

		 {
			  nfunc_overload{li::bit_cast<const void*>(&builtin_pop_array), {type::arr}, type::any},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_pop_else), {}, type::exc},
		 },
	};
	util::native_function detail::builtin_str = {
		 func_attr_pure | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.str",
		 &builtin_str_vm,
		 {nfunc_overload{li::bit_cast<const void*>(&builtin_str_coerce), {type::any}, type::str}},
	};
	util::native_function detail::builtin_num = {
		 func_attr_pure | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.num",
		 &builtin_num_vm,
		 {nfunc_overload{li::bit_cast<const void*>(&builtin_num_coerce), {type::any}, type::f64}},
	};
	util::native_function detail::builtin_int = {
		 func_attr_pure | func_attr_c_takes_self,
		 "builtin.int",
		 &builtin_int_vm,
		 {nfunc_overload{li::bit_cast<const void*>(&builtin_int_coerce), {type::any}, type::i32}},
	};
	util::native_function detail::builtin_join = {
		 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.join",
		 &builtin_join_vm,

		 {
			  nfunc_overload{li::bit_cast<const void*>(&builtin_join_array), {type::arr, type::arr}, type::arr},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_join_table), {type::tbl, type::tbl}, type::tbl},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_join_string), {type::str, type::str}, type::str},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_join_else), {type::any, type::any}, type::exc},
		 },
	};
	util::native_function detail::builtin_len = {
		 func_attr_pure | func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.len",
		 &builtin_len_vm,
		 {
			  nfunc_overload{li::bit_cast<const void*>(&builtin_len_array), {type::arr}, type::i32},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_len_table), {type::tbl}, type::i32},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_len_string), {type::str}, type::i32},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_len_else), {type::any}, type::any},
		 },
	};
	util::native_function detail::builtin_dup = {
		 func_attr_c_takes_vm | func_attr_c_takes_self,
		 "builtin.dup",
		 &builtin_dup_vm,
		 {
			  nfunc_overload{li::bit_cast<const void*>(&builtin_dup_array), {type::arr}, type::arr},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_dup_table), {type::tbl}, type::tbl},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_dup_function), {type::fn}, type::fn},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_dup_object), {type::obj}, type::obj},
			  nfunc_overload{li::bit_cast<const void*>(&builtin_dup_else), {type::any}, type::any},
		 },
	};
	util::native_function detail::builtin_new_array = {
		 func_attr_c_takes_vm,
		 nullptr,  // Private.
		 nullptr,
		 {nfunc_overload{li::bit_cast<const void*>(&builtin_new_array_i32), {type::i32}, type::arr}},
	};
	util::native_function detail::builtin_new_table = {
		 func_attr_c_takes_vm,
		 nullptr,  // Private.
		 nullptr,
		 {nfunc_overload{li::bit_cast<const void*>(&builtin_new_table_i32), {type::i32}, type::tbl}},
	};
	util::native_function detail::builtin_null_function = {
		 func_attr_c_takes_vm,
		 "builtin.nullfunc",  // Private.
		 &builtin_null_functor_vm,
		 {nfunc_overload{li::bit_cast<const void*>(&builtin_null_functor), {}, type::exc}},
	};

	// Registers the builtins, this is called by the VM creation as it is required.
	//
	void detail::register_builtin(vm* L) {
		builtin_in.export_into(L);
		builtin_len.export_into(L);
		builtin_dup.export_into(L);
		builtin_str.export_into(L);
		builtin_num.export_into(L);
		builtin_int.export_into(L);
		builtin_join.export_into(L);
		builtin_push.export_into(L);
		builtin_pop.export_into(L);

		util::export_as(L, "builtin.print", [](vm* L, any* args, slot_t n) {
			for (int32_t i = 0; i != n; i++) {
				args[-i].print();
				printf("\t");
			}
			printf("\n");
			return L->ok();
		});
		util::export_as(L, "builtin.loadstring", [](vm* L, any* args, slot_t n) -> any_t {
			vm_stack_guard _g{L, args};
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}

			auto res = load_script(L, args->as_str()->view());
			if (res.is_exc()) {
				return res;
			}
			return L->ok(res);
		});
		util::export_as(L, "builtin.eval", [](vm* L, any* args, slot_t n) -> any_t {
			vm_stack_guard _g{L, args};
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (res.is_exc()) {
				return res;
			}
			return L->call(0, res);
		});
		// TODO: v-- table.reserve, also add array.fill + bunch of functional programming primitives are missing.
		util::export_as(L, "builtin.@table", [](vm* L, any* args, slot_t n) {
			uint32_t r = 0;
			if (n && args->is_num()) {
				r = (uint32_t) (uint64_t) std::abs(args->as_num());
			}
			return L->ok(table::create(L, r));
		});
		util::export_as(L, "builtin.@array", [](vm* L, any* args, slot_t n) {
			uint32_t r = 0;
			if (n && args->is_num()) {
				r = (uint32_t) (uint64_t) std::abs(args->as_num());
			}
			return L->ok(array::create(L, r));
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

		// Arch specific optimization.
		//
#if LI_JIT && LI_ARCH_X86 && !LI_32
		using namespace ir;
		builtin_len.nfi.overloads[0].mir_lifter = [](mblock& b, insn* i) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::loadi32, tg, mmem{.base = REG(i->operands[0]), .disp = offsetof(array, length)});
			b.append(vop::izx32, tg, tg);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, REG(i), tf);
			return true;
		};
		builtin_len.nfi.overloads[1].mir_lifter = [](mblock& b, insn* i) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::loadi32, tg, mmem{.base = REG(i->operands[0]), .disp = offsetof(table, active_count)});
			b.append(vop::izx32, tg, tg);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, REG(i), tf);
			return true;
		};
		builtin_len.nfi.overloads[2].mir_lifter = [](mblock& b, insn* i) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::loadi32, tg, mmem{.base = REG(i->operands[0]), .disp = offsetof(string, length)});
			b.append(vop::izx32, tg, tg);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, REG(i), tf);
			return true;
		};
#endif
	}
};
