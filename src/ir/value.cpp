#include <ir/insn.hpp>
#include <ir/proc.hpp>
#include <util/enuminfo.hpp>

namespace li::ir {
	// String conversion.
	//
	std::string constant::to_string(bool) const {
		switch (vt) {
			case li::type::none:
				return "void";
			case li::type::any:
				return LI_RED "ERROR!" LI_DEF;
			case li::type::dty:
				return util::fmt(LI_GRN "%s" LI_DEF, get_type_name(nullptr, dty).data());
			case li::type::vty:
				return util::fmt(LI_GRN "%s" LI_DEF, type_names[(uint8_t) vty]);
			case li::type::vmopr:
				return util::fmt(LI_GRN "%s" LI_DEF, bc::opcode_details(vmopr).name);
			case li::type::i1:
				return util::fmt(LI_BLU "i1:  %s" LI_DEF, u ? "true" : "false");
			case li::type::nil:
				return util::fmt(LI_BLU "nil" LI_DEF);
			case li::type::exc:
				return util::fmt(LI_BLU "exc" LI_DEF);
			case li::type::i8:
				return util::fmt(LI_BLU "i8:  %llu" LI_DEF, u);
			case li::type::i16:
				return util::fmt(LI_BLU "i16: %llu" LI_DEF, u);
			case li::type::i32:
				return util::fmt(LI_CYN "i32: %lld" LI_DEF, i);
			case li::type::i64:
				return util::fmt(LI_BLU "i64: %llu" LI_DEF, u);
			case li::type::f32:
				return util::fmt(LI_BLU "f32: %lf" LI_DEF, n);
			case li::type::f64:
				return util::fmt(LI_BLU "f64: %lf" LI_DEF, n);
			case li::type::tbl:
				return util::fmt(LI_BLU "tbl: %p" LI_DEF, gc);
			case li::type::vcl:
				return util::fmt(LI_BLU "vcl: %p" LI_DEF, gc);
			case li::type::arr:
				return util::fmt(LI_BLU "arr: %p" LI_DEF, gc);
			case li::type::fn:
				return util::fmt(LI_BLU "fn:  %p" LI_DEF, gc);
			case li::type::nfni:
				if (nfni->name)
					return util::fmt(LI_BLU "nfi: %s" LI_DEF, nfni->name);
				else
					return util::fmt(LI_BLU "nfi: %p" LI_DEF, nfni);
			case li::type::str:
				return util::fmt(LI_BLU "str: %s" LI_DEF, str->c_str());
			case li::type::bb:
				return ir::to_string(bb);
			default:
				if (vt <= li::type::obj)
					return util::fmt(LI_BLU "obj: %p" LI_DEF, gc);
				assume_unreachable();
		}
	}
	std::string insn::to_string(bool expand) const {
		const char* ret = vt == type::any ? "?" : get_type_name(nullptr, vt).data();

		if (!expand) {
			return util::fmt(LI_YLW "%%%u" LI_DEF ":%s" LI_DEF, name, ret);
		}
		const char* opcode_name = util::name_enum(opc).data();

		std::string s;
		const char* op_pfx = is_volatile ? LI_PRP "volatile " : "";
		if (vt == type::none) {
			s = util::fmt("%s" LI_RED "%s " LI_DEF, op_pfx, opcode_name);
		} else {
			s = util::fmt(LI_YLW "%%%u" LI_DEF ":%s = " LI_RED "%s%s " LI_DEF, name, ret, op_pfx, opcode_name);
		}
		
		if (operands.empty()) {
			s += "()";
		} else {
			for (size_t i = 0; i != operands.size(); i++) {
				if (opc == opcode::phi)
					s += util::fmt(LI_PRP "$%x" LI_DEF "->", parent->predecessors[i]->uid);
				s += operands[i]->to_string();
				s += ", ";
			}
			s.pop_back();
			s.pop_back();
		}

		// Pad to 50 characters.
		//
		size_t n = util::display_length(s);
		if (n < 50) {
			s.resize(s.size() + (50 - n), ' ');
		}
		return s;
	}
};