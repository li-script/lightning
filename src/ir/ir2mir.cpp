#include <util/common.hpp>
#if LI_JIT && !LI_32
	#include <ir/ir2mir.hpp>
	#include <ir/ownership.hpp>
	#include <ir/runtime.hpp>
	#include <vm/array.hpp>
	#include <vm/object.hpp>
	#include <vm/rc.hpp>
	#include <vm/string.hpp>
	#include <vm/tier.hpp>
	#include <vm/typed_array.hpp>

namespace li::ir {

	// Operand helpers shared by every native backend.
	//
	#define RI(x)    get_ri_for(b, x, false)
	#define RIi(x)   get_ri_for(b, x, true)
	#define RM(x)    get_rm_for(b, x)
	#define REG(x)   get_reg_for(b, x->as<insn>())
	#define REGV(x)  get_reg_for(b, x)
	#define YIELD(x) yield_value(b, i, x)

	#define REF_VM() mop(mreg(vreg_vm))

	static int64_t extract_constant(value* value) {
		auto* constant = value->as<li::ir::constant>();
		if (constant->vt == type::f32)
			return li::bit_cast<uint32_t>(float(constant->n));
		if (is_marker_data(constant->vt))
			return 0;
		return constant->i;
	}

	static mreg get_existing_reg(insn* instruction) { return li::bit_cast<mreg>((msize_t) instruction->visited); }

	static mreg yield_value(mblock& b, insn* instruction, mop source) {
		mreg destination = get_existing_reg(instruction);
		if (destination) {
			b.append(destination.is_fp() ? vop::movf : vop::movi, destination, source);
		} else if (!source.is_reg()) {
			LI_ASSERT(source.is_const());
			destination = instruction->vt == type::f64 ? b->next_fp() : b->next_gp();
			b.append(destination.is_fp() ? vop::movf : vop::movi, destination, source);
		} else {
			destination = source.reg;
		}
		instruction->visited = li::bit_cast<msize_t>(destination);
		return destination;
	}

	static mreg get_reg_for(mblock& b, insn* instruction) {
		if (auto existing = get_existing_reg(instruction))
			return existing;
		return yield_value(b, instruction, is_floating_point_data(instruction->vt) ? b->next_fp() : b->next_gp());
	}

	static mreg get_reg_for(mblock& b, value* value) {
		if (value->is<insn>())
			return get_reg_for(b, value->as<insn>());
		auto result = is_floating_point_data(value->vt) ? b->next_fp() : b->next_gp();
		b.append(result.is_fp() ? vop::movf : vop::movi, result, extract_constant(value));
		return result;
	}

	// Constants have the same unboxed representation as registers. Boxing is
	// always an explicit type_erase operation.
	static mop get_ri_for(mblock& b, value* value, bool) {
		if (value->is<constant>())
			return mop(extract_constant(value));
		return get_reg_for(b, value->as<insn>());
	}

	static mop get_rm_for(mblock& b, value* value) {
		if (value->is<constant>() && is_floating_point_data(value->vt))
			return get_reg_for(b, value);
		return get_ri_for(b, value, false);
	}

	template<typename T>
	static void iadd_inplace(mblock& b, mreg out, T rhs) {
		b.append(vop::iadd, out, out, rhs);
	}
	template<typename T>
	static void iand_inplace(mblock& b, mreg out, T rhs) {
		b.append(vop::iand, out, out, rhs);
	}
	template<typename T>
	static void ior_inplace(mblock& b, mreg out, T rhs) {
		b.append(vop::ior, out, out, rhs);
	}
	template<typename T>
	static void ixor_inplace(mblock& b, mreg out, T rhs) {
		b.append(vop::ixor, out, out, rhs);
	}
	template<typename T>
	static void ishl_inplace(mblock& b, mreg out, T rhs) {
		b.append(vop::ishl, out, out, rhs);
	}
	template<typename T>
	static void ishr_inplace(mblock& b, mreg out, T rhs) {
		b.append(vop::ishr, out, out, rhs);
	}

	static void emit_lea(mblock& b, mreg out, mmem address) { b.append(vop::lea, out, address); }

	static void emit_icmp(mblock& b, mreg out, mop lhs, mop rhs, cond condition, mwidth width = mwidth::i64) {
		b.append_sized(vop::icmp, width, out, lhs, rhs, int64_t(condition));
	}

	static void check_type(mblock& b, value_type expected, mreg out, mreg value) {
		if (expected == type_nil || expected == type_exception) {
			emit_icmp(b, out, value, int64_t(make_tag(expected)), cond::eq);
		} else if (expected == type_number) {
			emit_icmp(b, out, value, int64_t(kvalue_number_limit), cond::ult);
		} else {
			auto tag = b->next_gp();
			b.append(vop::ishr, tag, value, kvalue_payload_bits);
			emit_icmp(b, out, tag, int64_t(make_tag(expected) >> kvalue_payload_bits), cond::eq);
		}
	}

	static void check_type_gc(mblock& b, value_type, mreg out, mreg value) { emit_icmp(b, out, value, int64_t(kvalue_gc_limit), cond::uge); }

	static void gc_type_clear(mblock& b, mreg dst, mreg src) {
	#if LI_KERNEL_MODE
		b.append(vop::ior, dst, src, int64_t(~kvalue_payload_mask));
	#else
		b.append(vop::iand, dst, src, int64_t(kvalue_payload_mask));
	#endif
	}

	static mreg normalize_boxed_zero(mblock& b, mreg value) {
		auto shifted = b->next_gp();
		auto nonzero = b->next_gp();
		auto zero    = b->next_gp();
		auto out     = b->next_gp();
		b.append(vop::ishl, shifted, value, 1);
		emit_icmp(b, nonzero, shifted, 0, cond::ne);
		b.append(vop::movi, zero, 0);
		b.append(vop::select, out, nonzero, value, zero);
		return out;
	}

	static void boxed_equal(mblock& b, mreg out, mreg lhs, mreg rhs, bool negate) {
		b.append(vop::movi, arch::map_gp_arg(0, 0), lhs);
		b.append(vop::movi, arch::map_gp_arg(1, 0), rhs);
		b.append(vop::call, {}, int64_t(&any_value_equals));
		b.append(vop::izx8, out, mreg(arch::gp_retval));
		if (negate)
			b.append(vop::ixor, out, out, 1);
	}

	static void box_number(mblock& b, mreg dst, mreg src) {
		auto magnitude = b->next_gp();
		auto is_nan    = b->next_gp();
		auto canonical = b->next_gp();
		b.append(vop::movi, dst, src);
		b.append(vop::ishl, magnitude, dst, 1);
		b.append(vop::ishr, magnitude, magnitude, 1);
		emit_icmp(b, is_nan, magnitude, int64_t(kvalue_exponent_mask), cond::ugt);
		b.append(vop::movi, canonical, int64_t(kvalue_nan));
		b.append(vop::select, dst, is_nan, canonical, dst);
	}

	static void type_erase(mblock& b, mreg source, mreg out, type source_type) {
		if (source_type == type::nil) {
			b.append(vop::movi, out, nil);
		} else if (source_type == type::exc) {
			b.append(vop::movi, out, exception_marker);
		} else if (source_type == type::i1) {
			auto tag = b->next_gp();
			b.append(vop::movi, tag, int64_t(mix_value(type_bool, 0)));
			b.append(vop::izx8, out, source);
			b.append(vop::ior, out, out, tag);
		} else if (source_type == type::i8 || source_type == type::i16 || source_type == type::i32) {
			auto integer   = b->next_gp();
			auto number    = b->next_fp();
			auto extension = source_type == type::i8 ? vop::isx8 : source_type == type::i16 ? vop::isx16 : vop::isx32;
			b.append(extension, integer, source);
			b.append_sized(vop::fcvt, source_type == type::i32 ? mwidth::i32 : mwidth::i64, number, integer);
			b.append(vop::movi, out, number);
		} else if (source_type == type::i64) {
			auto number = b->next_fp();
			b.append_sized(vop::fcvt, mwidth::i64, number, source);
			b.append(vop::movi, out, number);
		} else if (source_type == type::f32) {
			auto number = b->next_fp();
			b.append(vop::fx64, number, source);
			box_number(b, out, number);
		} else if (source_type == type::f64) {
			box_number(b, out, source);
		} else if (source_type == type::any) {
			b.append(vop::movi, out, source);
		} else {
			auto value_type = to_value_type(source_type);
			auto payload    = b->next_gp();
			b.append(vop::iand, payload, source, int64_t(kvalue_payload_mask));
			b.append(vop::movi, out, int64_t(mix_value(uint8_t(value_type), 0)));
			b.append(vop::ior, out, out, payload);
		}
	}

	static void type_erase(mblock& b, value* value, mreg out) {
		if (value->is<constant>()) {
			b.append(vop::movi, out, value->as<constant>()->to_any());
		} else if (value->vt == type::nil) {
			b.append(vop::movi, out, nil);
		} else if (value->vt == type::exc) {
			b.append(vop::movi, out, exception_marker);
		} else {
			type_erase(b, REG(value), out, value->vt);
		}
	}

	static void value_hash(mblock& b, mreg in, mreg out, value* value = nullptr) {
		if (value && value->is<constant>()) {
			b.append(vop::movi, out, int64_t(value->as<constant>()->to_any().hash()));
			return;
		}
		auto normalized = normalize_boxed_zero(b, in);
	#if LI_32 || !LI_HAS_CRC
		auto tmp = b->next_gp();
		b.append(vop::movi, out, int64_t(0xff51afd7ed558ccdull));
		b.append(vop::ishr, tmp, normalized, 33);
		b.append(vop::ixor, tmp, tmp, normalized);
		b.append(vop::imul, tmp, tmp, out);
		b.append(vop::ishr, out, tmp, 33);
		b.append(vop::ixor, out, out, tmp);
	#else
		b.append(vop::ishr, out, normalized, 8);
		b.append(vop::crc32, out, out, normalized);
	#endif
	}

	static bool lift_intrinsic(mblock& b, insn* instruction, intrinsic id) {
		auto unary      = [&]() { return REGV(instruction->operands[2]); };
		auto binary_lhs = [&]() { return REGV(instruction->operands[2]); };
		auto binary_rhs = [&]() { return REGV(instruction->operands[3]); };
		switch (id) {
			case intrinsic::sqrt:
				b.append(vop::fsqrt, REG(instruction), unary());
				return true;
			case intrinsic::abs:
				b.append(vop::fabs, REG(instruction), unary());
				return true;
			case intrinsic::floor:
				b.append(vop::fround, REG(instruction), unary(), int64_t(round_mode::down));
				return true;
			case intrinsic::ceil:
				b.append(vop::fround, REG(instruction), unary(), int64_t(round_mode::up));
				return true;
			case intrinsic::trunc:
				b.append(vop::fround, REG(instruction), unary(), int64_t(round_mode::toward_zero));
				return true;
			case intrinsic::round: {
				auto half     = b->next_fp();
				auto adjusted = b->next_fp();
				b.append(vop::movf, half, li::bit_cast<int64_t>(0.5));
				b.append(vop::fcopysign, half, half, unary());
				b.append(vop::fadd, adjusted, unary(), half);
				b.append(vop::fround, REG(instruction), adjusted, int64_t(round_mode::toward_zero));
				return true;
			}
			case intrinsic::min:
				b.append(vop::fmin, REG(instruction), binary_lhs(), binary_rhs());
				return true;
			case intrinsic::max:
				b.append(vop::fmax, REG(instruction), binary_lhs(), binary_rhs());
				return true;
			case intrinsic::copysign:
				b.append(vop::fcopysign, REG(instruction), binary_lhs(), binary_rhs());
				return true;
			case intrinsic::cycles: {
				auto cycles = b->next_gp();
				b.append(vop::rdcycle, cycles);
				b.append_sized(vop::fcvt, mwidth::i64, REG(instruction), cycles);
				return true;
			}
			case intrinsic::array_len:
			case intrinsic::string_len:
			case intrinsic::typed_len: {
				int32_t offset = id == intrinsic::array_len    ? offsetof(array, length)
									  : id == intrinsic::string_len ? offsetof(string, length)
																			  : offsetof(typed_array, length);
				auto    length = b->next_gp();
				b.append(vop::loadi32, length, mmem{.base = unary(), .disp = offset});
				b.append(vop::izx32, length, length);
				b.append_sized(vop::fcvt, mwidth::i64, REG(instruction), length);
				return true;
			}
			case intrinsic::crc32:
				b.append(vop::crc32, REG(instruction), binary_lhs(), binary_rhs());
				return true;
			case intrinsic::none:
				return false;
		}
		assume_unreachable();
	}

	static mreg numeric_as_f64(mblock& b, value* input) {
		LI_ASSERT(is_integer_data(input->vt) || is_floating_point_data(input->vt));

		auto source = REGV(input);
		if (input->vt == type::f64)
			return source;

		auto out = b->next_fp();
		if (input->vt == type::f32) {
			b.append(vop::fx64, out, source);
			return out;
		}

		// Internal integers are signed. Narrow values must be sign-extended
		// before the signed integer-to-double conversion; va_count's i32 is
		// already zero-extended, but is constrained to the non-negative range.
		auto width = input->vt == type::i32 ? mwidth::i32 : mwidth::i64;
		if (input->vt == type::i8 || input->vt == type::i16) {
			auto extended = b->next_gp();
			b.append(input->vt == type::i8 ? vop::isx8 : vop::isx16, extended, source);
			source = extended;
		}
		b.append_sized(vop::fcvt, width, out, source);
		return out;
	}

	// Emits IEEE comparisons as canonical GP booleans. Floating `ne` includes
	// unordered; every ordered relation excludes unordered operands.
	static void fp_compare(mblock& b, operation operation, value* lhs, value* rhs, mreg out) {
		LI_ASSERT(is_integer_data(lhs->vt) || is_floating_point_data(lhs->vt));
		LI_ASSERT(is_integer_data(rhs->vt) || is_floating_point_data(rhs->vt));

		mreg   left;
		mreg   right;
		mwidth width;
		if (is_integer_data(lhs->vt) || is_integer_data(rhs->vt)) {
			left  = numeric_as_f64(b, lhs);
			right = numeric_as_f64(b, rhs);
			width = mwidth::f64;
		} else {
			left  = REGV(lhs);
			right = REGV(rhs);
			width = lhs->vt == type::f64 || rhs->vt == type::f64 ? mwidth::f64 : mwidth::f32;
			if (width == mwidth::f64 && lhs->vt == type::f32) {
				auto widened = b->next_fp();
				b.append(vop::fx64, widened, left);
				left = widened;
			}
			if (width == mwidth::f64 && rhs->vt == type::f32) {
				auto widened = b->next_fp();
				b.append(vop::fx64, widened, right);
				right = widened;
			}
		}
		cond condition;
		switch (operation) {
			case bc::CEQ:
				condition = cond::eq;
				break;
			case bc::CNE:
				condition = cond::ne;
				break;
			case bc::CLT:
				condition = cond::slt;
				break;
			case bc::CLE:
				condition = cond::sle;
				break;
			case bc::CGT:
				condition = cond::sgt;
				break;
			case bc::CGE:
				condition = cond::sge;
				break;
			default:
				util::abort("invalid floating-point comparison");
		}
		b.append_sized(vop::fcmp, width, out, left, right, int64_t(condition));
	}

	static mreg fp_unary(mblock& b, operation operation, value* rhs, insn* result = nullptr) {
		LI_ASSERT(is_integer_data(rhs->vt) || is_floating_point_data(rhs->vt));
		auto out = result ? REG(result) : b->next_fp();
		if (operation == bc::ANEG) {
			b.append(vop::fneg, out, numeric_as_f64(b, rhs));
			return out;
		}
		return {};
	}

	static mreg fp_binary(mblock& b, operation operation, value* lhs, value* rhs, insn* result = nullptr) {
		LI_ASSERT(is_integer_data(lhs->vt) || is_floating_point_data(lhs->vt));
		LI_ASSERT(is_integer_data(rhs->vt) || is_floating_point_data(rhs->vt));
		auto out   = result ? REG(result) : b->next_fp();
		auto left  = numeric_as_f64(b, lhs);
		auto right = numeric_as_f64(b, rhs);
		switch (operation) {
			case bc::AADD:
				b.append(vop::fadd, out, left, right);
				return out;
			case bc::ASUB:
				b.append(vop::fsub, out, left, right);
				return out;
			case bc::AMUL:
				b.append(vop::fmul, out, left, right);
				return out;
			case bc::ADIV:
				b.append(vop::fdiv, out, left, right);
				return out;
			case bc::AMOD: {
				auto quotient = b->next_fp();
				b.append(vop::fdiv, quotient, left, right);
				b.append(vop::fround, quotient, quotient, int64_t(round_mode::toward_zero));
				b.append(vop::fmul, quotient, quotient, right);
				b.append(vop::fsub, out, left, quotient);
				return out;
			}
			default:
				return {};
		}
	}

	// Load/store from local.
	//
	static void local_load(mblock& b, mop idx, mreg out) {
		if (idx.is_const()) {
			int64_t disp   = (idx.i64 + FRAME_SIZE + 1) * 8;
			int32_t disp32 = int32_t(disp);
			LI_ASSERT(disp32 == disp);

			if (out.is_fp())
				b.append(vop::loadf64, out, mmem{.base = vreg_args, .disp = disp32});
			else
				b.append(vop::loadi64, out, mmem{.base = vreg_args, .disp = disp32});
		} else if (idx.is_reg() && idx.reg.is_gp()) {
			if (out.is_fp())
				b.append(vop::loadf64, out, mmem{.base = vreg_args, .index = idx.reg, .shift = 3, .disp = 8 * (FRAME_SIZE + 1)});
			else
				b.append(vop::loadi64, out, mmem{.base = vreg_args, .index = idx.reg, .shift = 3, .disp = 8 * (FRAME_SIZE + 1)});
		} else {
			util::abort("invalid index value.");
		}
	}
	static void local_store(mblock& b, mop idx, value* in) {
		auto slot  = b->next_gp();
		auto value = b->next_gp();
		if (idx.is_const()) {
			int64_t disp   = (idx.i64 + FRAME_SIZE + 1) * 8;
			int32_t disp32 = int32_t(disp);
			LI_ASSERT(disp32 == disp);
			emit_lea(b, slot, mmem{.base = vreg_args, .disp = disp32});
		} else if (idx.is_reg() && idx.reg.is_gp()) {
			emit_lea(b, slot, mmem{.base = vreg_args, .index = idx.reg, .shift = 3, .disp = 8 * (FRAME_SIZE + 1)});
		} else {
			util::abort("invalid index value.");
		}

		type_erase(b, in, value);
		b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
		b.append(vop::movi, arch::map_gp_arg(1, 0), slot);
		b.append(vop::movi, arch::map_gp_arg(2, 0), value);
		b.append(vop::call, {}, (int64_t) &runtime::slot_replace);
	}

	static bool needs_reference_counting(type vt) { return vt == type::any || is_gc_data(vt); }

	static void read_ccall_result(mblock& b, insn* i, type vt) {
		if (is_floating_point_data(vt)) {
			b.append(vop::movf, REG(i), mreg(arch::fp_retval));
			return;
		}
		if (vt == type::none)
			return;

		auto result = mreg(arch::gp_retval);
		switch (vt) {
			case type::i1:
				b.append(vop::izx8, REG(i), result);
				return;
			case type::i8:
				b.append(vop::isx8, REG(i), result);
				return;
			case type::i16:
				b.append(vop::isx16, REG(i), result);
				return;
			case type::i32:
				b.append(vop::isx32, REG(i), result);
				return;
			default:
				b.append(vop::movi, REG(i), result);
				return;
		}
	}

	// Main lifter switch.
	//
	static void mlift(mblock& b, insn* i) {
		switch (i->opc) {
			// Locals.
			//
			case opcode::load_local: {
				local_load(b, RIi(i->operands[0]), REG(i));
				return;
			}
			case opcode::store_local: {
				local_store(b, RIi(i->operands[0]), i->operands[1]);
				return;
			}

			// Complex types.
			//
			// TODO: None of this is right, just testing...
			//
			case opcode::field_set: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[1], arch::map_gp_arg(1, 0));
				type_erase(b, i->operands[2], arch::map_gp_arg(2, 0));
				type_erase(b, i->operands[3], arch::map_gp_arg(3, 0));
				auto target = i->operands[0]->as<constant>()->i1 ? &runtime::field_set_raw : &runtime::field_set;
				b.append(vop::call, {}, (int64_t) target);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::field_get: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[1], arch::map_gp_arg(1, 0));
				type_erase(b, i->operands[2], arch::map_gp_arg(2, 0));
				auto target = i->operands[0]->as<constant>()->i1 ? &runtime::field_get_raw_borrowed : &runtime::field_get;
				b.append(vop::call, {}, (int64_t) target);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::struct_array_get: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[0], arch::map_gp_arg(1, 0));
				type_erase(b, i->operands[1], arch::map_gp_arg(2, 0));
				b.append(vop::call, {}, (int64_t) &runtime::field_get);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::struct_array_class_test: {
				auto* expected         = i->operands[1]->as<constant>()->vcl;
				auto  array            = REGV(i->operands[0]);
				auto  kind             = b->next_gp();
				auto  kind_matches     = b->next_gp();
				auto  actual_class     = b->next_gp();
				auto  class_nonnull    = b->next_gp();
				auto  safe_class       = b->next_gp();
				auto  fallback_class   = b->next_gp();
				auto  actual_identity  = b->next_gp();
				auto  identity_matches = b->next_gp();
				b.append(vop::loadi8, kind, mmem{.base = array, .disp = offsetof(typed_array, element_kind)});
				b.append(vop::izx8, kind, kind);
				emit_icmp(b, kind_matches, kind, static_cast<int64_t>(typed_array_kind::struct_elements), cond::eq);
				b.append(vop::loadi64, actual_class, mmem{.base = array, .disp = offsetof(typed_array, element_class)});
				emit_icmp(b, class_nonnull, actual_class, 0, cond::ne);
				b.append(vop::movi, fallback_class, int64_t(reinterpret_cast<intptr_t>(expected)));
				b.append(vop::select, safe_class, class_nonnull, actual_class, fallback_class);
				b.append(vop::loadi64, actual_identity, mmem{.base = safe_class, .disp = offsetof(vclass, identity)});
				emit_icmp(b, identity_matches, actual_identity, int64_t(expected->identity), cond::eq);
				b.append(vop::iand, REG(i), kind_matches, class_nonnull);
				iand_inplace(b, REG(i), identity_matches);
				return;
			}
			case opcode::struct_array_bounds_test: {
				auto array       = REGV(i->operands[0]);
				auto index       = REGV(i->operands[1]);
				auto length      = b->next_gp();
				auto nonnegative = b->next_gp();
				auto below       = b->next_gp();
				b.append(vop::loadi32, length, mmem{.base = array, .disp = offsetof(typed_array, length)});
				b.append(vop::izx32, length, length);
				emit_icmp(b, nonnegative, index, 0, cond::sge, mwidth::i32);
				emit_icmp(b, below, index, length, cond::ult, mwidth::i32);
				b.append(vop::iand, REG(i), nonnegative, below);
				return;
			}
			case opcode::struct_array_field_load: {
				auto array        = REGV(i->operands[0]);
				auto index        = REGV(i->operands[1]);
				auto stride       = i->operands[2]->as<constant>()->i32;
				auto offset       = i->operands[3]->as<constant>()->i32;
				auto storage      = b->next_gp();
				auto scaled       = b->next_gp();
				auto address      = b->next_gp();
				auto storage_type = i->operands[4]->as<constant>()->dty;
				b.append(vop::loadi64, storage, mmem{.base = array, .disp = offsetof(typed_array, storage)});
				b.append_sized(vop::imul, mwidth::i64, scaled, index, int64_t(stride));
				emit_lea(b, address, mmem{.base = storage, .disp = offsetof(typed_array_store, entries)});
				iadd_inplace(b, address, scaled);
				auto memory = mmem{.base = address, .disp = offset};
				switch (storage_type) {
					case type::i1:
					case type::i8:
						b.append(vop::loadi8, REG(i), memory);
						if (i->vt == type::i1)
							b.append(vop::izx8, REG(i), REG(i));
						else
							b.append(vop::isx8, REG(i), REG(i));
						return;
					case type::i16:
						b.append(vop::loadi16, REG(i), memory);
						b.append(vop::isx16, REG(i), REG(i));
						return;
					case type::i32:
						b.append(vop::loadi32, REG(i), memory);
						b.append(vop::isx32, REG(i), REG(i));
						return;
					case type::i64:
						b.append(vop::loadi64, REG(i), memory);
						return;
					case type::f32: {
						auto narrow = b->next_fp();
						b.append(vop::loadf32, narrow, memory);
						b.append(vop::fx64, REG(i), narrow);
						return;
					}
					case type::f64:
						b.append(vop::loadf64, REG(i), memory);
						return;
					default:
						util::abort("invalid fused struct field type");
				}
			}
			case opcode::class_new: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[0], arch::map_gp_arg(1, 0));
				b.append(vop::call, {}, (int64_t) &runtime::class_new);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::class_is: {
				type_erase(b, i->operands[0], arch::map_gp_arg(0, 0));
				type_erase(b, i->operands[1], arch::map_gp_arg(1, 0));
				b.append(vop::call, {}, (int64_t) &runtime::class_is);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				iand_inplace(b, REG(i), 1);
				return;
			}
			case opcode::iter_next: {
				auto local0 = b->next_gp();
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[0], arch::map_gp_arg(1, 0));
				emit_lea(b, local0, mmem{.base = vreg_args, .disp = 8 * (FRAME_SIZE + 1)});
				b.append(vop::movi, arch::map_gp_arg(2, 0), local0);
				b.append(vop::movi, arch::map_gp_arg(3, 0), RIi(i->operands[1]));
				b.append(vop::call, {}, (int64_t) &runtime::iter_next);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}

			// Operators.
			//
			case opcode::unop: {
				auto op = i->operands[0]->as<constant>()->vmopr;
				if (i->vt == type::f64 && fp_unary(b, op, i->operands[1], i))
					return;

				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[1], arch::map_gp_arg(1, 0));
				b.append(vop::movi, arch::map_gp_arg(2, 0), int32_t(op));
				b.append(vop::call, {}, (int64_t) &runtime::unary);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::binop: {
				auto op = i->operands[0]->as<constant>()->vmopr;
				if (i->as<binop>()->is_exact_integer_arithmetic()) {
					vop native_op;
					switch (op) {
						case bc::AADD:
							native_op = vop::iadd;
							break;
						case bc::ASUB:
							native_op = vop::isub;
							break;
						case bc::AMUL:
							native_op = vop::imul;
							break;
						default:
							util::abort("invalid exact integer arithmetic");
					}
					auto width = i->vt == type::i32 ? mwidth::i32 : mwidth::i64;
					b.append_sized(native_op, width, REG(i), REGV(i->operands[1]), RIi(i->operands[2]));
					if (i->vt == type::i32)
						b.append(vop::isx32, REG(i), REG(i));
					return;
				}
				if (i->vt == type::f64 && fp_binary(b, op, i->operands[1], i->operands[2], i))
					return;

				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				type_erase(b, i->operands[1], arch::map_gp_arg(1, 0));
				type_erase(b, i->operands[2], arch::map_gp_arg(2, 0));
				b.append(vop::movi, arch::map_gp_arg(3, 0), int32_t(op));
				b.append(vop::call, {}, (int64_t) &runtime::binary);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}

			// Runtime coordination.
			//
			case opcode::safepoint: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				emit_lea(b, mreg(arch::map_gp_arg(1, 0)), mmem{.base = vreg_tos});
				b.append(vop::call, {}, (int64_t) &runtime::safepoint);
				return;
			}
			case opcode::va_count: {
				b.append(vop::movi, REG(i), mreg(vreg_nargs));
				b.append(vop::izx32, REG(i), REG(i));
				return;
			}
			case opcode::va_get: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), mreg(vreg_args));
				b.append(vop::movi, arch::map_gp_arg(2, 0), mreg(vreg_nargs));
				type_erase(b, i->operands[0], arch::map_gp_arg(3, 0));
				b.append(vop::call, {}, (int64_t) &runtime::va_get);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::extract: {
				auto index = i->operands[1]->as<constant>()->i32;
				if (index == 0) {
					YIELD(RI(i->operands[0]));
				} else if (i->operands[0]->vt == type::exc) {
					b.append(vop::movi, REG(i), 0);
				} else if (i->operands[0]->vt != type::any) {
					b.append(vop::movi, REG(i), 1);
				} else {
					auto marker = b->next_gp();
					b.append(vop::movi, marker, exception_marker);
					emit_icmp(b, REG(i), REG(i->operands[0]), marker, cond::ne);
				}
				return;
			}

			// Upvalue.
			//
			case opcode::uval_get: {
				mmem mem;
				auto idx = RIi(i->operands[1]);

				// Compute index into function uvalue table and load from it.
				//
				auto base = REG(i->operands[0]);
				if (idx.is_const()) {
					mem = {.base = base, .disp = int32_t(sizeof(function) + idx.i64 * 8)};
				} else {
					mem = {.base = base, .index = idx.reg, .shift = 3, .disp = sizeof(function)};
				}

				// Load the result.
				//
				b.append(vop::loadi64, REG(i), mem);
				return;
			}
			case opcode::uval_set: {
				auto base = REG(i->operands[0]);
				auto idx  = RIi(i->operands[1]);
				auto val  = b->next_gp();
				auto slot = b->next_gp();
				type_erase(b, i->operands[2], val);

				mmem mem;
				if (idx.is_const()) {
					mem = {.base = base, .disp = int32_t(sizeof(function) + idx.i64 * 8)};
				} else {
					mem = {.base = base, .index = idx.reg, .shift = 3, .disp = sizeof(function)};
				}
				emit_lea(b, slot, mem);
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), slot);
				b.append(vop::movi, arch::map_gp_arg(2, 0), val);
				b.append(vop::call, {}, (int64_t) &runtime::slot_try_replace);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}

			// Casts.
			//
			case opcode::assume_cast: {
				auto* op  = i->operands[0].get();
				auto  out = REG(i);
				switch (i->vt) {
					case type::i1: {
						LI_ASSERT(op->vt == type::i1 || is_integer_data(op->vt));
						b.append(vop::movi, out, RIi(op));
						iand_inplace(b, out, 1);
						return;
					}
					case type::i8:
					case type::i16:
					case type::i32:
					case type::i64: {
						if (is_integer_data(op->vt)) {
							b.append(vop::movi, out, RIi(op));
							if (i->vt == type::i8)
								b.append(vop::isx8, out, out);
							else if (i->vt == type::i16)
								b.append(vop::isx16, out, out);
							else if (i->vt == type::i32)
								b.append(vop::isx32, out, out);
							return;
						}
						auto in = b->next_fp();
						b.append(vop::movf, in, REGV(op));
						auto width = i->vt == type::i8 ? mwidth::i8 : i->vt == type::i16 ? mwidth::i16 : i->vt == type::i32 ? mwidth::i32 : mwidth::i64;
						b.append_sized(vop::icvt, width, out, in);
						return;
					}
					case type::f32:
					case type::f64: {
						LI_ASSERT(is_floating_point_data(op->vt) || op->vt == type::any);
						b.append(vop::movf, out, REGV(op));
						if (i->vt == type::f32 && op->vt != type::f32)
							b.append(vop::fx32, out, out);
						else if (i->vt == type::f64 && op->vt == type::f32)
							b.append(vop::fx64, out, out);
						return;
					}
					// GC types.
					default: {
						gc_type_clear(b, out, REGV(op));
						return;
					}
					// Invalid types.
					//
					case type::exc:
					case type::nil:
					case type::any: {
						util::abort("invalid assume_cast");
						break;
					}
				}
			}
			case opcode::coerce_bool: {
				switch (i->operands[0]->vt) {
					case type::none:
					case type::exc:
					case type::nil: {
						b.append(vop::movi, REG(i), 0);
						return;
					}
					case type::any: {
						static_assert(type_bool == (type_nil - 1), "Outdated constants.");

						auto tmp = REG(i);
						b.append(vop::movi, tmp, (int64_t) ~mix_value(type_bool, 0));
						iadd_inplace(b, tmp, RIi(i->operands[0]));
						emit_icmp(b, tmp, tmp, -2ll, cond::ult);
						return;
					}
					case type::i1: {
						b.append(vop::movi, REG(i), RIi(i->operands[0]));
						return;
					}
					default: {
						b.append(vop::movi, REG(i), 1);
						return;
					}
				}
			}

			// Helpers used before transitioning to MIR.
			//
			case opcode::keep_alive:
				// Semantic lifetime marker. Ownership lowering consumes its
				// liveness; it deliberately has no machine-level effect.
				return;
			case opcode::move: {
				YIELD(RI(i->operands[0]));
				return;
			}
			case opcode::erase_type: {
				type_erase(b, i->operands[0], REG(i));
				return;
			}
			case opcode::retain: {
				auto* source = i->operands[0].get();
				if (needs_reference_counting(source->vt)) {
					auto erased = b->next_gp();
					type_erase(b, source, erased);
					b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
					b.append(vop::movi, arch::map_gp_arg(1, 0), erased);
					auto target = static_cast<void (*)(vm*, any_t)>(&rc::retain_frame);
					b.append(vop::call, {}, (int64_t) li::bit_cast<uintptr_t>(target));
				}
				YIELD(RI(source));
				return;
			}
			case opcode::release: {
				auto* source = i->operands[0].get();
				if (!needs_reference_counting(source->vt))
					return;
				auto erased = b->next_gp();
				type_erase(b, source, erased);
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), erased);
				auto target = static_cast<void (*)(vm*, any_t)>(&rc::release);
				b.append(vop::call, {}, (int64_t) li::bit_cast<uintptr_t>(target));
				return;
			}

			// Conditionals.
			//
			case opcode::test_type: {
				auto* source = i->operands[0].get();
				auto  vt     = i->operands[1]->as<constant>()->vty;
				auto  out    = REG(i);
				if (source->vt != type::any) {
					b.append(vop::movi, out, to_value_type(source->vt) == vt);
				} else {
					check_type(b, vt, out, REG(source));
				}
				return;
			}
			case opcode::compare: {
				auto   cc  = i->operands[0]->as<constant>()->vmopr;
				value* lhs = i->operands[1];
				value* rhs = i->operands[2];

				// Mixed internal integers and floating-point language numbers compare
				// after conversion to f64. This keeps relational, equality, and NaN
				// behavior identical to ordinary language-number comparisons.
				//
				auto lhs_numeric = is_integer_data(lhs->vt) || is_floating_point_data(lhs->vt);
				auto rhs_numeric = is_integer_data(rhs->vt) || is_floating_point_data(rhs->vt);
				if (lhs_numeric && rhs_numeric && (is_floating_point_data(lhs->vt) || is_floating_point_data(rhs->vt))) {
					fp_compare(b, cc, lhs, rhs, REG(i));
					return;
				}

				// Integer relations are signed over the IR's normalized integer
				// representation. va_count and indices use the non-negative subset.
				//
				if (is_integer_data(lhs->vt) && is_integer_data(rhs->vt)) {
					cond condition;
					switch (cc) {
						case bc::CEQ:
							condition = cond::eq;
							break;
						case bc::CNE:
							condition = cond::ne;
							break;
						case bc::CLT:
							condition = cond::slt;
							break;
						case bc::CLE:
							condition = cond::sle;
							break;
						case bc::CGT:
							condition = cond::sgt;
							break;
						case bc::CGE:
							condition = cond::sge;
							break;
						default:
							util::abort("invalid integer comparison");
					}
					emit_icmp(b, REG(i), REGV(lhs), RIi(rhs), condition);
					return;
				}

				// If equality comparison:
				//
				if (cc == bc::CEQ || cc == bc::CNE) {
					auto condition = cc == bc::CEQ ? cond::eq : cond::ne;

					// Strings from different shared/private pools compare by content.
					//
					if (lhs->vt == type::str && rhs->vt == type::str) {
						b.append(vop::movi, arch::map_gp_arg(0, 0), REGV(lhs));
						b.append(vop::movi, arch::map_gp_arg(1, 0), REGV(rhs));
						b.append(vop::call, {}, int64_t(&string_value_equals));
						b.append(vop::izx8, REG(i), mreg(arch::gp_retval));
						if (cc == bc::CNE)
							b.append(vop::ixor, REG(i), REG(i), 1);
						return;
					}

					// If same type.
					//
					if (lhs->vt == rhs->vt) {
						// LHS cannot be a constant.
						//
						if (lhs->is<constant>())
							std::swap(lhs, rhs);

						if (lhs->vt == type::any) {
							auto o1 = REG(lhs);
							mreg o2;
							if (rhs->is<constant>()) {
								o2 = b->next_gp();
								b.append(vop::movi, o2, (int64_t) rhs->as<constant>()->to_any().value);
							} else {
								o2 = REG(rhs);
							}
							boxed_equal(b, REG(i), o1, o2, cc == bc::CNE);
						} else {
							emit_icmp(b, REG(i), REG(lhs), RM(rhs), condition);
						}
						return;
					}
					// If it requires type erasure:
					//
					else if (lhs->vt == type::any || rhs->vt == type::any) {
						// Erase type if needed.
						//
						mreg o1;
						mreg o2;
						if (lhs->vt != type::any) {
							o1 = b->next_gp();
							type_erase(b, lhs, o1);
						} else {
							o1 = REG(lhs);
						}
						if (rhs->vt != type::any) {
							o2 = b->next_gp();
							type_erase(b, rhs, o2);
						} else {
							o2 = REG(rhs);
						}
						boxed_equal(b, REG(i), o1, o2, cc == bc::CNE);
						return;
					}

					// Distinct statically known types are never equal.
					b.append(vop::movi, REG(i), cc == bc::CNE);
					return;
				}
				break;  // NYI
			}
			case opcode::select: {
				auto cc    = REGV(i->operands[0]);
				auto input = [&](value* operand) {
					if (i->vt != type::any || operand->vt == type::any)
						return REGV(operand);
					auto boxed = b->next_gp();
					type_erase(b, operand, boxed);
					return boxed;
				};
				auto lhs = input(i->operands[1]);
				auto rhs = input(i->operands[2]);
				b.append(vop::select, REG(i), cc, lhs, rhs);
				return;
			}
			case opcode::bool_xor:
			case opcode::bool_or:
			case opcode::bool_and: {
				auto lhs = RI(i->operands[0]);
				auto rhs = RI(i->operands[1]);
				auto out = REG(i);
				if (lhs.is_const())
					std::swap(lhs, rhs);
				b.append(vop::movi, out, lhs);

				if (i->opc == opcode::bool_and)
					iand_inplace(b, out, rhs);
				else if (i->opc == opcode::bool_xor)
					ixor_inplace(b, out, rhs);
				else if (i->opc == opcode::bool_or)
					ior_inplace(b, out, rhs);
				return;
			}
			case opcode::phi: {
				auto r = get_existing_reg(i->operands[0]->as<insn>());
				for (auto& op : i->operands) {
					LI_ASSERT(get_existing_reg(op->as<insn>()) == r);
				}
				YIELD(r);
				return;
			}

			// Call types.
			//
			case opcode::ccall: {
				auto*   info           = i->operands[0]->as<constant>()->nfni;
				int32_t overload_index = i->operands[1]->as<constant>()->i32;
				auto&   overload       = info->overloads[overload_index];

				if (lift_intrinsic(b, i, overload.intrinsic_id))
					return;

				int32_t gp_index     = 0;
				int32_t fp_index     = 0;
				int32_t stack_offset = 0;
				auto    stack_slot   = [&](type argument_type) {
					int32_t size = 8;
					if (argument_type == type::i1 || argument_type == type::i8)
						size = 1;
					else if (argument_type == type::i16)
						size = 2;
					else if (argument_type == type::i32 || argument_type == type::f32)
						size = 4;
					if constexpr (arch::packed_stack_arguments)
						stack_offset = (stack_offset + size - 1) & -size;
					int32_t displacement = arch::stack_arg_begin + stack_offset;
					stack_offset += arch::packed_stack_arguments ? size : 8;
					b->used_stack_length = std::max(b->used_stack_length, displacement + size);
					return mmem{.base = arch::sp, .disp = displacement};
				};
				auto store_stack_argument = [&](type argument_type, value* argument) {
					auto slot = stack_slot(argument_type);
					if (argument_type == type::any && argument->vt != type::any) {
						auto erased = b->next_gp();
						type_erase(b, argument, erased);
						b.append(vop::storei64, {}, slot, erased);
					} else if (argument_type == type::f32) {
						b.append(vop::storef32, {}, slot, REGV(argument));
					} else if (argument_type == type::f64) {
						b.append(vop::storef64, {}, slot, REGV(argument));
					} else {
						auto source = RIi(argument);
						if (source.is_const()) {
							auto temporary = b->next_gp();
							b.append(vop::movi, temporary, source);
							source = temporary;
						}
						auto operation = argument_type == type::i1 || argument_type == type::i8 ? vop::storei8
											  : argument_type == type::i16                           ? vop::storei16
											  : argument_type == type::i32                           ? vop::storei32
																														: vop::storei64;
						b.append(operation, {}, slot, source);
					}
				};

				if (info->attr & func_attr_c_takes_vm) {
					b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
					gp_index++;
				}

				for (msize_t n = 2; n != i->operands.size(); n++) {
					auto* argument      = i->operands[n].get();
					type  argument_type = overload.args[n - 2];
					if (argument_type == type::any) {
						auto target = arch::map_gp_arg(gp_index++, fp_index);
						if (target) {
							if (argument->vt == type::any)
								b.append(vop::movi, mreg(target), RIi(argument));
							else
								type_erase(b, argument, mreg(target));
						} else {
							store_stack_argument(argument_type, argument);
						}
					} else if (is_floating_point_data(argument_type)) {
						LI_ASSERT(argument->vt == argument_type);
						auto target = arch::map_fp_arg(gp_index, fp_index++);
						if (target)
							b.append(vop::movf, mreg(target), RI(argument));
						else
							store_stack_argument(argument_type, argument);
					} else {
						LI_ASSERT(argument->vt == argument_type);
						auto target = arch::map_gp_arg(gp_index++, fp_index);
						if (target)
							b.append(vop::movi, mreg(target), RIi(argument));
						else
							store_stack_argument(argument_type, argument);
					}
				}

				b.append(vop::call, {}, intptr_t(overload.cfunc));
				read_ccall_result(b, i, overload.ret);
				return;
			}
			case opcode::get_exception: {
				b.append(vop::loadi64, REG(i), mmem{.base = vreg_vm, .disp = offsetof(vm, last_ex)});
				return;
			}
			case opcode::set_exception: {
				auto tmp = b->next_gp();
				type_erase(b, i->operands[0], tmp);
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), tmp);
				emit_lea(b, mreg(arch::map_gp_arg(2, 0)), mmem{.base = vreg_args, .disp = 8 * (FRAME_SIZE + 1)});
				b.append(vop::movi, arch::map_gp_arg(3, 0), i->source_bc);
				b.append(vop::call, {}, (int64_t) &runtime::set_exception);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			case opcode::vcall: {
				const int32_t owned_slots  = int32_t(i->operands.size());
				const int32_t call_slots   = owned_slots + 1;
				auto          child_local0 = b->next_gp();
				emit_lea(b, child_local0, mmem{.base = vreg_tos, .disp = call_slots * 8});

				// Lay out arguments below the child's local0 exactly as vm_invoke
				// expects: ... arg1, arg0, self, target, caller. Dynamic borrowed
				// values get a frame reference here; ownership-transfer markers are
				// adopted without a retain. JIT constants are pinned by the compiled
				// function and are cleared before frame truncation.
				//
				std::vector<int32_t> borrowed_constant_slots;
				int32_t              next_index = FRAME_TARGET;
				auto                 write_arg  = [&](value* v) {
					int32_t idx = next_index--;
					mreg    val;
					if (v->is<constant>()) {
						val = b->next_gp();
						b.append(vop::movi, val, v->as<constant>()->to_any());
					} else if (v->vt != type::any) {
						val = b->next_gp();
						type_erase(b, v, val);
					} else {
						val = REG(v);
					}
					b.append(vop::storei64, {}, mmem{.base = child_local0, .disp = idx * 8}, val);

					if (is_ownership_transfer(v) || !value_may_need_reference_counting(v))
						return;
					// Only a statically known function target is immutable for the whole
					// invocation. Callees may overwrite self/argument slots, and dynamic
					// target dispatch may replace target/self before returning.
					if (v->is<constant>() && idx == FRAME_TARGET && v->vt == type::fn) {
						borrowed_constant_slots.emplace_back(idx);
						return;
					}
					b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
					b.append(vop::movi, arch::map_gp_arg(1, 0), val);
					auto retain_target = static_cast<void (*)(vm*, any_t)>(&rc::retain_frame);
					b.append(vop::call, {}, (int64_t) li::bit_cast<uintptr_t>(retain_target));
				};

				// Link the child frame to this method's local0. Compute stack_pos
				// through the runtime so movable/coroutine stacks are supported.
				//
				auto caller         = b->next_gp();
				auto current_local0 = b->next_gp();
				emit_lea(b, current_local0, mmem{.base = vreg_args, .disp = 8 * (FRAME_SIZE + 1)});
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), current_local0);
				b.append(vop::movi, arch::map_gp_arg(2, 0), i->source_bc);
				b.append(vop::call, {}, (int64_t) &runtime::make_call_frame);
				b.append(vop::movi, caller, mreg(arch::gp_retval));
				b.append(vop::storei64, {}, mmem{.base = child_local0, .disp = FRAME_CALLER * 8}, caller);
				for (auto& op : i->operands)
					write_arg(op);

				// Publish the complete frame only after every borrowed dynamic value
				// has gained its frame reference.
				//
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				emit_lea(b, mreg(arch::map_gp_arg(1, 0)), mmem{.base = vreg_tos});
				b.append(vop::movi, arch::map_gp_arg(2, 0), owned_slots);
				b.append(vop::movi, arch::map_gp_arg(3, 0), child_local0);
				b.append(vop::call, {}, (int64_t) &runtime::call_prepare);

				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				emit_lea(b, mreg(arch::map_gp_arg(1, 0)), mmem{.base = child_local0, .disp = -8 * (FRAME_SIZE + 1)});
				b.append(vop::movi, arch::map_gp_arg(2, 0), owned_slots - 2);
				b.append(vop::call, {}, (int64_t) &vm_invoke);

				// vm_invoke returns an owned result. Remove borrowed constants from
				// the published range before truncation so their pinned references are
				// not released as if the frame owned them.
				//
				auto result = b->next_gp();
				b.append(vop::movi, result, mreg(arch::gp_retval));
				if (!borrowed_constant_slots.empty()) {
					auto nil_value = b->next_gp();
					b.append(vop::movi, nil_value, int64_t(any(nil).value));
					for (int32_t idx : borrowed_constant_slots)
						b.append(vop::storei64, {}, mmem{.base = child_local0, .disp = idx * 8}, nil_value);
				}
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				emit_lea(b, mreg(arch::map_gp_arg(1, 0)), mmem{.base = vreg_tos});
				b.append(vop::movi, arch::map_gp_arg(2, 0), result);
				b.append(vop::call, {}, (int64_t) &runtime::call_finish);
				b.append(vop::movi, REG(i), mreg(arch::gp_retval));
				return;
			}
			// Block terminators.
			//
			case opcode::jcc: {
				b.append(vop::js, {}, REG(i->operands[0]), i->operands[1]->as<constant>()->bb->uid, i->operands[2]->as<constant>()->bb->uid);
				return;
			}
			case opcode::jmp: {
				b.append(vop::jmp, {}, i->operands[0]->as<constant>()->bb->uid);
				return;
			}

			// Specials.
			//
			// Procedure terminators.
			//
			case opcode::ret: {
				auto result = b->next_gp();
				auto local0 = b->next_gp();
				type_erase(b, i->operands[0], result);
				emit_lea(b, local0, mmem{.base = vreg_args, .disp = 8 * (FRAME_SIZE + 1)});

				// Returning transfers an owned value after releasing this frame's
				// owning local slots.
				//
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), local0);
				b.append(vop::movi, arch::map_gp_arg(2, 0), result);
				b.append(vop::call, {}, (int64_t) &runtime::frame_leave);

				auto r = mreg(arch::gp_retval);
				b.append(vop::ret, {}, mop(r));
				return;
			}
			case opcode::unreachable: {
				b.append(vop::unreachable, {});
				return;
			}
			case opcode::deopt: {
				// Every consumed value moves into its owning frame slot, then the
				// interpreter resumes this same frame and produces the result.
				//
				auto* exit = i->as<deopt>();
				for (size_t n = 1; n + 1 < i->operands.size(); n += 2) {
					auto slot  = b->next_gp();
					auto value = b->next_gp();
					auto index = int64_t(i->operands[n]->as<constant>()->i32);
					emit_lea(b, slot, mmem{.base = vreg_args, .disp = int32_t((index + FRAME_SIZE + 1) * 8)});
					type_erase(b, i->operands[n + 1], value);
					b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
					b.append(vop::movi, arch::map_gp_arg(1, 0), slot);
					b.append(vop::movi, arch::map_gp_arg(2, 0), value);
					b.append(vop::call, {}, (int64_t) &tier::deopt_store);
				}
				auto state = tier::pack_resume_state(uint32_t(i->operands[0]->as<constant>()->i32), exit->exception_handler_pc, exit->cleanup_handler_pc);
				b.append(vop::movi, arch::map_gp_arg(0, 0), REF_VM());
				b.append(vop::movi, arch::map_gp_arg(1, 0), mreg(vreg_args));
				b.append(vop::movi, arch::map_gp_arg(2, 0), mreg(vreg_nargs));
				b.append(vop::movi, arch::map_gp_arg(3, 0), int64_t(state));
				b.append(vop::call, {}, (int64_t) &tier::deopt_resume);
				b.append(vop::ret, {}, mop(mreg(arch::gp_retval)));
				return;
			}
			case opcode::osr_load: {
				// Adopt the interpreter's owning slot: the native frame now owns
				// the value and the slot is cleared without a release.
				//
				auto index     = int64_t(i->operands[0]->as<constant>()->i32);
				auto disp      = int32_t((index + FRAME_SIZE + 1) * 8);
				auto nil_value = b->next_gp();
				b.append(vop::loadi64, REG(i), mmem{.base = vreg_args, .disp = disp});
				b.append(vop::movi, nil_value, int64_t(any(nil).value));
				b.append(vop::storei64, {}, mmem{.base = vreg_args, .disp = disp}, nil_value);
				return;
			}
			default:
				break;
		}
		util::abort("Opcode NYI: %s", i->to_string(true).c_str());
	}

	// Generates crude machine IR from the SSA IR.
	//
	std::unique_ptr<mprocedure> lift_ir(procedure* p) {
		// Set basic information.
		//
		auto m            = std::make_unique<mprocedure>();
		m->source         = p;
		m->max_stack_slot = FRAME_SIZE + p->f->num_locals;

		// Clear all visitor state, we use both fields for mapping to machine structures.
		//
		m->source->clear_all_visitor_state();

		// Pre-allocate the block list and coalesce PHI nodes.
		//
		for (auto& b : m->source->basic_blocks) {
			auto* mb = m->add_block();
			mb->hot  = int32_t(b->loop_depth) - int32_t(b->cold_hint);

			b->visited = (uint64_t) mb;
			for (auto* phi : b->phis()) {
				// Allocate a register.
				//
				mreg r;
				if (is_floating_point_data(phi->vt))
					r = m->next_fp();
				else
					r = m->next_gp();

				// Force into all incoming blocks.
				//
				for (auto& op : phi->operands) {
					LI_ASSERT(op->is<insn>());

					auto* src = op->as<insn>();
					if (auto r2 = get_existing_reg(src)) {
						LI_ASSERT(r == r2);
					} else {
						src->visited = li::bit_cast<msize_t>(r);
					}
				}
			}
		}

		// For each block:
		//
		for (auto& b : m->source->basic_blocks) {
			// printf("-- Block $%x", b->uid);
			// if (b->cold_hint)
			//	printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) b->cold_hint);
			// if (b->loop_depth)
			//	printf(LI_RED " [LOOP %u]" LI_DEF, (uint32_t) b->loop_depth);
			// putchar('\n');

			// Fix the jumps.
			//
			auto* mb = (mblock*) b->visited;
			for (auto& suc : b->successors)
				m->add_jump(mb, (mblock*) suc->visited);

			// Initialize this method's owning local area once. The scratch region
			// is reserved and nil-filled but remains above the logical stack top.
			//
			if (b.get() == m->source->get_entry()) {
				auto local0 = m->next_gp();
				mb->append(vop::movi, arch::map_gp_arg(0, 0), mreg(vreg_vm));
				emit_lea(*mb, local0, mmem{.base = vreg_args, .disp = 8 * (FRAME_SIZE + 1)});
				mb->append(vop::movi, arch::map_gp_arg(1, 0), local0);
				mb->append(vop::movi, arch::map_gp_arg(2, 0), int32_t(p->f->num_locals));
				mb->append(vop::movi, arch::map_gp_arg(3, 0), int32_t(p->max_stack_slot));
				mb->append(vop::call, {}, (int64_t) &tier::prepare_jit_frame);
			}

			// Lift each instruction.
			//
			for (auto* i : b->insns()) {
				// printf(LI_GRN "#%-5x" LI_DEF "\t\t %s\n", i->source_bc, i->to_string(true).c_str());
				// size_t n = mb->instructions.size();
				mlift(*mb, i);
				// while (n != mb->instructions.size()) {
				//	puts(mb->instructions[n++].to_string().c_str());
				//}
			}
		}
		return m;
	}

};

#endif
