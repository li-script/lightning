#include <ir/insn.hpp>
#include <ir/proc.hpp>
#include <util/enuminfo.hpp>

namespace li::ir {
	std::string constant::to_string(bool) const {
		switch (vt) {
			case li::ir::type::none:
				return "void";
			case li::ir::type::unk:
				return LI_RED "ERROR!" LI_DEF;
			case li::ir::type::vmtrait: {
				auto t = trait_names[(uint8_t) vmtrait];
				return util::fmt(LI_GRN "%.*s" LI_DEF, t.size(), t.data());
			}
			case li::ir::type::irtype:
				return util::fmt(LI_GRN "%s" LI_DEF, util::name_enum(irtype).data());
			case li::ir::type::vmtype:
				return util::fmt(LI_GRN "%s" LI_DEF, type_names[(uint8_t) vmtype]);
			case li::ir::type::vmopr:
				return util::fmt(LI_GRN "%s" LI_DEF, bc::opcode_details(vmopr).name);
			case li::ir::type::i1:
				return util::fmt(LI_BLU "i1:  %s" LI_DEF, u ? "true" : "false");
			case li::ir::type::i8:
				return util::fmt(LI_BLU "i8:  %llu" LI_DEF, u);
			case li::ir::type::i16:
				return util::fmt(LI_BLU "i16: %llu" LI_DEF, u);
			case li::ir::type::i32:
				return util::fmt(LI_CYN "i32: %lld" LI_DEF, i);
			case li::ir::type::i64:
				return util::fmt(LI_BLU "i64: %llu" LI_DEF, u);
			case li::ir::type::f32:
				return util::fmt(LI_BLU "f32: %lf" LI_DEF, n);
			case li::ir::type::f64:
				return util::fmt(LI_BLU "f64: %lf" LI_DEF, n);
			case li::ir::type::opq:
				return util::fmt(LI_BLU "opq: %llu" LI_DEF, u);
			case li::ir::type::tbl:
				return util::fmt(LI_BLU "tbl: %p" LI_DEF, gc);
			case li::ir::type::udt:
				return util::fmt(LI_BLU "udt: %p" LI_DEF, gc);
			case li::ir::type::arr:
				return util::fmt(LI_BLU "arr: %p" LI_DEF, gc);
			case li::ir::type::vfn:
				return util::fmt(LI_BLU "vfn: %p" LI_DEF, gc);
			case li::ir::type::nfn:
				return util::fmt(LI_BLU "nfn: %p" LI_DEF, gc);
			case li::ir::type::str:
				return util::fmt(LI_BLU "str: %s" LI_DEF, str->c_str());
			case li::ir::type::bb:
				return ir::to_string(bb);
			default:
				assume_unreachable();
		}
	}

	std::string insn::to_string(bool expand) const {
		const char* ret = vt == type::unk ? "?" : util::name_enum(vt).data();

		if (!expand) {
			return util::fmt(LI_YLW "%%%u" LI_DEF ":%s" LI_DEF, name, ret);
		}
		const char* opc = util::name_enum(op).data();

		std::string s;
		const char* op_pfx = is_volatile ? LI_PRP "volatile " : "";
		if (vt == type::none) {
			s = util::fmt("%s" LI_RED "%s " LI_DEF, op_pfx, opc);
		} else {
			s = util::fmt(LI_YLW "%%%u" LI_DEF ":%s = " LI_RED "%s%s " LI_DEF, name, ret, op_pfx, opc);
		}
		
		if (operands.empty()) {
			return s + "()";
		}
		for (auto& opr : operands) {
			s += opr->to_string();
			s += ", ";
		}
		s.pop_back();
		s.pop_back();
		return s;
	}
};