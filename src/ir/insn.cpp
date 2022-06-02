#include <ir/insn.hpp>
#include <ir/proc.hpp>
#include <util/enuminfo.hpp>

namespace li::ir {
	std::string insn::to_string(bool expand) const {
		if (!expand) {
			return util::fmt(LI_YLW "%%%u" LI_DEF, name);
		}
		const char* opc = util::name_enum(op).data();

		std::string s;
		if (is_volatile) {
			if (vt == type::none) {
				s = util::fmt(LI_PRP "volatile " LI_RED "%s " LI_DEF, opc);
			} else if (vt == type::unk) {
				s = util::fmt(LI_YLW "%%%u" LI_DEF " = " LI_PRP "volatile " LI_RED "%s " LI_DEF, name, opc);
			} else {
				const char* ret = util::name_enum(vt).data();
				s               = util::fmt(LI_YLW "%%%u" LI_DEF ":%s = " LI_PRP "volatile " LI_RED "%s " LI_DEF, name, ret, opc);
			}
		} else {
			if (vt == type::none) {
				s = util::fmt(LI_RED "%s " LI_DEF, opc);
			} else if (vt == type::unk) {
				s = util::fmt(LI_YLW "%%%u" LI_DEF " = " LI_RED "%s " LI_DEF, name, opc);
			} else {
				const char* ret = util::name_enum(vt).data();
				s               = util::fmt(LI_YLW "%%%u" LI_DEF ":%s = " LI_RED "%s " LI_DEF, name, ret, opc);
			}
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