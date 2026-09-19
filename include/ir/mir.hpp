#pragma once
#include <array>
#include <cstdint>
#include <ir/arch.hpp>
#include <ir/proc.hpp>
#include <limits>
#include <list>
#include <string>
#include <util/bitset.hpp>
#include <util/common.hpp>
#include <util/format.hpp>
#include <utility>
#include <vector>

namespace li::ir {
	// Conditions are target-neutral and describe the source-language comparison.
	// Floating-point relational conditions are ordered, while ne is true for unordered
	// operands. ordered/unordered are the explicit NaN predicates.
	//
	enum class cond : int32_t {
		eq,
		ne,
		slt,
		sle,
		sgt,
		sge,
		ult,
		ule,
		ugt,
		uge,
		ordered,
		unordered,
	};
	inline constexpr std::array cond_names = {
		 "eq",
		 "ne",
		 "slt",
		 "sle",
		 "sgt",
		 "sge",
		 "ult",
		 "ule",
		 "ugt",
		 "uge",
		 "ordered",
		 "unordered",
	};

	enum class round_mode : int32_t {
		nearest,
		down,
		up,
		toward_zero,
	};
	inline constexpr std::array round_mode_names = {"nearest", "down", "up", "toward_zero"};

	// The width is part of the neutral instruction, rather than being inferred from
	// a target mnemonic selected before register allocation.
	//
	enum class mwidth : uint8_t {
		none,
		i8,
		i16,
		i32,
		i64,
		f32,
		f64,
	};
	inline constexpr std::array mwidth_names = {"", "i8", "i16", "i32", "i64", "f32", "f64"};

	// Machine register type. Physical IDs are supplied by the selected ABI adapter:
	// positive IDs are GP registers, negative IDs are FP registers, and zero is none.
	//
	enum class regclass : uint32_t /*:2*/ {
		null,
		virt,
		phys,
	};
	using preg = arch::reg;
	enum vreg : int32_t {
		vreg_vm    = 1,  // arguments[0], vm*
		vreg_args  = 2,  // arguments[1], any*
		vreg_nargs = 3,  // arguments[2], int
		vreg_tos   = 4,  // calculated from vreg_args + the final local size
		vreg_cpool = 5,  // neutral constant-pool base
		vreg_first = 6,
	};
	inline constexpr const char* vreg_names[] = {"$null", "$vm", "$args", "$nargs", "$tos", "$cpool"};

	struct mreg {
		int32_t  id : 30 = 0;
		regclass cl : 2  = regclass::null;

		constexpr mreg(std::nullopt_t) : id(0), cl(regclass::null) {}
		constexpr mreg(preg r) : id(int32_t(r)), cl(regclass::phys) { LI_ASSERT(id != 0); }
		constexpr mreg(vreg r) : id(int32_t(r)), cl(regclass::virt) { LI_ASSERT(id != 0); }
		constexpr mreg()                       = default;
		constexpr mreg(const mreg&)            = default;
		constexpr mreg& operator=(const mreg&) = default;

		constexpr preg phys() const {
			LI_ASSERT(is_phys());
			return preg(id);
		}
		constexpr vreg virt() const {
			LI_ASSERT(is_virt());
			return vreg(id);
		}

		// Gets a zero-based unique identifier. Sign and class are both encoded so
		// virtual and physical GP/FP registers never alias in data-flow bitsets.
		//
		constexpr msize_t uid() const {
			msize_t x = id < 0 ? msize_t(-id) : msize_t(id);
			return (x << 3) + (id < 0) + (msize_t(cl) << 1);
		}
		static constexpr mreg from_uid(msize_t i) {
			mreg result = {};
			result.id   = int32_t(i >> 3);
			if (i & 1)
				result.id = -result.id;
			result.cl = regclass((i >> 1) & 3);
			return result;
		}

		constexpr bool     is_phys() const { return cl == regclass::phys; }
		constexpr bool     is_virt() const { return cl == regclass::virt; }
		constexpr bool     is_null() const { return cl == regclass::null; }
		constexpr bool     is_gp() const { return !is_null() && id > 0; }
		constexpr bool     is_fp() const { return !is_null() && id < 0; }
		explicit constexpr operator bool() const { return !is_null(); }

		constexpr bool operator==(const mreg& o) const { return li::bit_cast<int32_t>(*this) == li::bit_cast<int32_t>(o); }
		constexpr bool operator!=(const mreg& o) const { return !(*this == o); }
		constexpr bool operator<(const mreg& o) const { return li::bit_cast<int32_t>(*this) < li::bit_cast<int32_t>(o); }

		std::string to_string() const {
			if (is_null()) {
				return LI_RED "null" LI_DEF;
			} else if (is_virt()) {
				if (is_fp()) {
					return util::fmt(LI_CYN "%%vf%u" LI_DEF, uint32_t(-id));
				} else if (id < vreg_first) {
					return util::fmt(LI_RED "%s" LI_DEF, vreg_names[id]);
				} else {
					return util::fmt(LI_YLW "%%v%u" LI_DEF, uint32_t(id - vreg_first));
				}
			} else {
				return std::string(arch::name_reg(phys()));
			}
		}
	};

	// Memory addressing is base + index * (1 << shift) + displacement. A null
	// index means that the indexed component is absent, including when shift is 0.
	//
	struct mmem {
		mreg    base  = {};
		mreg    index = {};
		int8_t  shift = 0;
		int32_t disp  = 0;

		std::string to_string() const {
			std::string result = "[";
			bool        any    = false;
			if (base) {
				result += base.to_string();
				any = true;
			}
			if (index) {
				if (any)
					result += "+";
				result += index.to_string();
				if (shift)
					result += util::fmt("<<%u", uint32_t(shift));
				any = true;
			}
			if (disp > 0) {
				if (any)
					result += "+";
				result += util::fmt(LI_BRG "0x%x" LI_DEF, uint32_t(disp));
			} else if (disp < 0) {
				result += util::fmt(LI_BRG "-0x%x" LI_DEF, uint32_t(-int64_t(disp)));
			} else if (!any) {
				result += "0";
			}
			result += "]";
			return result;
		}
	};

	struct mop {
		union {
			int64_t i64;
			mmem    mem;
			mreg    reg;
		};

		// The negative shift values are private operand tags. Valid memory shifts
		// are 0..3, which cover every scalar address scale supported by the MIR.
		//
		mop() { mem.shift = -3; }
		mop(std::nullopt_t) : mop() {}
		mop(int64_t i) : i64(i) { mem.shift = -2; }
		mop(cond c) : mop(int64_t(c)) {}
		mop(round_mode mode) : mop(int64_t(mode)) {}
		mop(mreg r) : reg(r) { mem.shift = -1; }
		mop(mmem m) : mem(m) {
			LI_ASSERT(mem.shift >= 0 && mem.shift <= 3);
			LI_ASSERT(mem.index || mem.shift == 0);
		}
		mop(any x) : mop(int64_t(x.value)) {}
		mop(const mop& o) : mem(o.mem) {}
		mop& operator=(const mop& o) {
			mem = o.mem;
			return *this;
		}

		bool     is_null() const { return mem.shift <= -3; }
		bool     is_const() const { return mem.shift == -2; }
		bool     is_reg() const { return mem.shift == -1; }
		bool     is_mem() const { return mem.shift >= 0; }
		explicit operator bool() const { return !is_null(); }

		std::string to_string() const {
			if (is_reg())
				return reg.to_string();
			if (is_mem())
				return mem.to_string();
			if (is_const())
				return util::fmt(LI_GRN "0x%llx" LI_DEF, i64);
			return LI_RED "null" LI_DEF;
		}
	};

	// Additional register effects are expressed in neutral physical-register ID
	// masks. Bit N denotes physical ID N+1 for GP and -(N+1) for FP registers.
	//
	struct mins_effects {
		uint64_t implicit_gp_read  = 0;
		uint64_t implicit_gp_write = 0;
		uint64_t implicit_fp_read  = 0;
		uint64_t implicit_fp_write = 0;
		bool     side_effects      = false;
	};

	enum class vop : uint8_t {
		null,

		movf,
		movi,
		izx8,
		izx16,
		izx32,
		isx8,
		isx16,
		isx32,
		fx32,
		fx64,
		icvt,
		fcvt,

		loadi8,
		loadi16,
		loadi32,
		loadi64,
		loadf32,
		loadf64,
		storei8,
		storei16,
		storei32,
		storei64,
		storef32,
		storef64,

		iadd,
		isub,
		imul,
		idiv,
		iudiv,
		imod,
		iand,
		ior,
		ixor,
		ineg,
		inot,
		ishl,
		ishr,
		isar,

		fadd,
		fsub,
		fmul,
		fdiv,
		fneg,
		fabs,
		fsqrt,
		fround,
		fmin,
		fmax,
		fcopysign,

		icmp,
		fcmp,
		lea,
		select,
		crc32,
		rdcycle,

		call,
		js,
		jmp,
		ret,
		unreachable,
	};
	inline constexpr std::array vop_names = {
		 "null",
		 "movf",
		 "movi",
		 "izx8",
		 "izx16",
		 "izx32",
		 "isx8",
		 "isx16",
		 "isx32",
		 "fx32",
		 "fx64",
		 "icvt",
		 "fcvt",
		 "loadi8",
		 "loadi16",
		 "loadi32",
		 "loadi64",
		 "loadf32",
		 "loadf64",
		 "storei8",
		 "storei16",
		 "storei32",
		 "storei64",
		 "storef32",
		 "storef64",
		 "iadd",
		 "isub",
		 "imul",
		 "idiv",
		 "iudiv",
		 "imod",
		 "iand",
		 "ior",
		 "ixor",
		 "ineg",
		 "inot",
		 "ishl",
		 "ishr",
		 "isar",
		 "fadd",
		 "fsub",
		 "fmul",
		 "fdiv",
		 "fneg",
		 "fabs",
		 "fsqrt",
		 "fround",
		 "fmin",
		 "fmax",
		 "fcopysign",
		 "icmp",
		 "fcmp",
		 "lea",
		 "select",
		 "crc32",
		 "rdcycle",
		 "call",
		 "js",
		 "jmp",
		 "ret",
		 "unreachable",
	};
	static_assert(vop_names.size() == size_t(vop::unreachable) + 1);

	constexpr mwidth default_width(vop v) {
		switch (v) {
			case vop::izx8:
			case vop::isx8:
			case vop::loadi8:
			case vop::storei8:
				return mwidth::i8;
			case vop::izx16:
			case vop::isx16:
			case vop::loadi16:
			case vop::storei16:
				return mwidth::i16;
			case vop::izx32:
			case vop::isx32:
			case vop::loadi32:
			case vop::storei32:
				return mwidth::i32;
			case vop::fx32:
			case vop::loadf32:
			case vop::storef32:
				return mwidth::f32;
			case vop::movf:
			case vop::fx64:
			case vop::loadf64:
			case vop::storef64:
			case vop::fadd:
			case vop::fsub:
			case vop::fmul:
			case vop::fdiv:
			case vop::fneg:
			case vop::fabs:
			case vop::fsqrt:
			case vop::fround:
			case vop::fmin:
			case vop::fmax:
			case vop::fcopysign:
			case vop::fcmp:
				return mwidth::f64;
			case vop::movi:
			case vop::icvt:
			case vop::fcvt:
			case vop::loadi64:
			case vop::storei64:
			case vop::iadd:
			case vop::isub:
			case vop::imul:
			case vop::idiv:
			case vop::iudiv:
			case vop::imod:
			case vop::iand:
			case vop::ior:
			case vop::ixor:
			case vop::ineg:
			case vop::inot:
			case vop::ishl:
			case vop::ishr:
			case vop::isar:
			case vop::icmp:
			case vop::lea:
			case vop::select:
			case vop::crc32:
			case vop::rdcycle:
				return mwidth::i64;
			default:
				return mwidth::none;
		}
	}
	constexpr uint8_t default_hint(vop v) {
		switch (v) {
			case vop::iadd:
			case vop::isub:
			case vop::imul:
			case vop::idiv:
			case vop::iudiv:
			case vop::imod:
			case vop::iand:
			case vop::ior:
			case vop::ixor:
			case vop::ineg:
			case vop::inot:
			case vop::ishl:
			case vop::ishr:
			case vop::isar:
			case vop::fadd:
			case vop::fsub:
			case vop::fmul:
			case vop::fdiv:
			case vop::fneg:
			case vop::fabs:
			case vop::fsqrt:
			case vop::fround:
			case vop::fmin:
			case vop::fmax:
			case vop::fcopysign:
				return 0;
			default:
				return std::numeric_limits<uint8_t>::max();
		}
	}

	struct minsn {
		static constexpr uint8_t no_arg                 = std::numeric_limits<uint8_t>::max();
		static constexpr size_t  maximum_argument_count = 3;

		vop          op                          = vop::null;
		mop          arg[maximum_argument_count] = {std::nullopt};
		mreg         out                         = {};
		mwidth       width                       = mwidth::none;
		mins_effects effects                     = {};
		uint8_t      tied_arg                    = no_arg;  // Hard out/argument tie; normalized before coloring.
		uint8_t      hint_arg                    = no_arg;  // Preferred out/argument coalescing.
		bool         no_spill                    = false;

		minsn()                        = default;
		minsn(const minsn&)            = default;
		minsn& operator=(const minsn&) = default;

		template<typename... Tx>
		minsn(vop v, mreg result, Tx... operands)
			 : op(v), arg{mop(operands)...}, out(result), width(v == vop::select && result.is_fp() ? mwidth::f64 : default_width(v)), hint_arg(default_hint(v)) {
			static_assert(sizeof...(Tx) <= maximum_argument_count, "MIR instructions have at most three arguments");
			validate_shape();
		}
		template<typename... Tx>
		minsn(vop v, mreg result, mwidth w, Tx... operands) : op(v), arg{mop(operands)...}, out(result), width(w), hint_arg(default_hint(v)) {
			static_assert(sizeof...(Tx) <= maximum_argument_count, "MIR instructions have at most three arguments");
			validate_shape();
		}

		void validate_shape() const {
			if (op == vop::icmp || op == vop::fcmp) {
				LI_ASSERT(out.is_gp() && num_args() == 3 && arg[2].is_const());
				LI_ASSERT(arg[2].i64 >= int64_t(cond::eq) && arg[2].i64 <= int64_t(cond::unordered));
			} else if (op == vop::fround) {
				LI_ASSERT(num_args() == 2 && arg[1].is_const());
				LI_ASSERT(arg[1].i64 >= int64_t(round_mode::nearest) && arg[1].i64 <= int64_t(round_mode::toward_zero));
			} else if (op == vop::select || op == vop::js) {
				LI_ASSERT(num_args() == 3 && (arg[0].is_const() || (arg[0].is_reg() && arg[0].reg.is_gp())));
			}
		}

		bool is(vop value) const { return op == value; }
		bool is_null() const { return op == vop::null; }
		vop  getv() const {
			LI_ASSERT(!is_null());
			return op;
		}
		size_t num_args() const {
			for (size_t i = 0; i != std::size(arg); i++) {
				if (!arg[i])
					return i;
			}
			return std::size(arg);
		}
		explicit operator bool() const { return !is_null(); }

		template<typename F>
		void for_each_reg(F&& fn) const {
			for (const auto& a : arg) {
				if (!a)
					break;
				if (a.is_reg()) {
					fn(a.reg, true);
				} else if (a.is_mem()) {
					if (a.mem.base)
						fn(a.mem.base, true);
					if (a.mem.index)
						fn(a.mem.index, true);
				}
			}
			if (out)
				fn(out, false);
		}

		template<typename F>
		static void for_each_mask_reg(uint64_t mask, bool fp, bool read, F&& fn) {
			while (mask) {
				uint32_t bit = std::countr_zero(mask);
				fn(mreg(preg(fp ? -int32_t(bit + 1) : int32_t(bit + 1))), read);
				mask &= mask - 1;
			}
		}
		template<typename F>
		void for_each_reg_w_implicit(F&& fn) const {
			if (is(vop::call)) {
				for (preg r : arch::gp_argument)
					fn(mreg(r), true);
				for (preg r : arch::fp_argument)
					fn(mreg(r), true);
			}
			for_each_mask_reg(effects.implicit_gp_read, false, true, fn);
			for_each_mask_reg(effects.implicit_fp_read, true, true, fn);
			for_each_reg(fn);
			if (is(vop::call)) {
				for (preg r : arch::gp_volatile)
					fn(mreg(r), false);
				for (preg r : arch::fp_volatile)
					fn(mreg(r), false);
			}
			for_each_mask_reg(effects.implicit_gp_write, false, false, fn);
			for_each_mask_reg(effects.implicit_fp_write, true, false, fn);
		}

		bool uses_register(mreg r) const {
			bool found = false;
			for_each_reg([&](mreg value, bool) { found |= value == r; });
			return found;
		}
		bool writes_to_register(mreg r) const {
			bool found = false;
			for_each_reg([&](mreg value, bool read) { found |= !read && value == r; });
			return found;
		}
		bool reads_from_register(mreg r) const {
			bool found = false;
			for_each_reg([&](mreg value, bool read) { found |= read && value == r; });
			return found;
		}
		bool writes_to_memory() const { return vop::storei8 <= op && op <= vop::storef64; }
		bool has_side_effects() const {
			return effects.side_effects || writes_to_memory() || op == vop::rdcycle || op == vop::call || op == vop::js || op == vop::jmp || op == vop::ret ||
					 op == vop::unreachable;
		}
		bool is_compare() const { return op == vop::icmp || op == vop::fcmp; }
		bool is_move_between_same_class() const {
			if (!arg[0].is_reg() || !out || out.is_fp() != arg[0].reg.is_fp())
				return false;
			return op == vop::movf || op == vop::movi || (vop::izx8 <= op && op <= vop::fx64);
		}

		std::string to_string() const {
			if (is_null())
				return "INVALID";

			std::string result;
			if (no_spill)
				result = LI_BLU "nospill " LI_DEF;
			if (out)
				result += out.to_string() + LI_DEF " = ";
			result += LI_PRP;
			result += vop_names[size_t(op)];
			if (width != mwidth::none) {
				result += ".";
				result += mwidth_names[size_t(width)];
			}
			result += LI_DEF;
			for (size_t i = 0; i != num_args(); i++) {
				result += " ";
				if ((op == vop::icmp || op == vop::fcmp) && i == 2 && arg[i].is_const() && arg[i].i64 >= 0 && size_t(arg[i].i64) < cond_names.size()) {
					result += cond_names[size_t(arg[i].i64)];
				} else if (op == vop::fround && i == 1 && arg[i].is_const() && arg[i].i64 >= 0 && size_t(arg[i].i64) < round_mode_names.size()) {
					result += round_mode_names[size_t(arg[i].i64)];
				} else {
					result += arg[i].to_string();
				}
			}
			result += LI_DEF;
			return result;
		}
	};

	struct mprocedure;
	struct mblock {
		mprocedure*          parent       = nullptr;
		msize_t              uid          = 0;
		int32_t              hot          = 0;
		std::vector<minsn>   instructions = {};
		std::vector<mblock*> predecessors;
		std::vector<mblock*> successors;
		uint64_t             visited = 0;
		util::bitset         df_def, df_ref;
		util::bitset         df_in_live, df_out_live;
		size_t               asm_loc = 0;

		template<typename... Tx>
		size_t append(vop v, mreg out, Tx... arg) {
			size_t n = instructions.size();
			instructions.emplace_back(v, out, arg...);
			return n;
		}
		template<typename... Tx>
		size_t append_sized(vop v, mwidth width, mreg out, Tx... arg) {
			size_t n = instructions.size();
			instructions.emplace_back(v, out, width, arg...);
			return n;
		}
		mprocedure* operator->() const { return parent; }
		void        print() const {
			for (const auto& i : instructions)
				printf("\t%s\n", i.to_string().c_str());
		}
	};

	struct mprocedure {
		procedure*        source       = nullptr;
		std::list<mblock> basic_blocks = {};
		int32_t           next_reg_i   = 0;
		int32_t           next_reg_f   = 0;
		uint32_t          next_block   = 0;
		std::vector<any>  const_pool   = {};
		std::string       assembly_error;

		uint64_t used_gp_mask      = 0;
		uint64_t used_fp_mask      = 0;
		int32_t  used_stack_length = arch::home_size;
		uint32_t spill_slots       = 0;
		msize_t  max_stack_slot    = 0;
		uint64_t next_visited_mark = 0x50eaeb7446b52b12;

		mreg   next_gp() { return vreg(vreg_first + next_reg_i++); }
		mreg   next_fp() { return vreg(-++next_reg_f); }
		size_t spill_slot_count() const { return spill_slots; }

		mmem add_const(any c) {
			auto it = range::find(const_pool, c);
			if (it == const_pool.end()) {
				const_pool.emplace_back(c);
				it = const_pool.end() - 1;
			}
			size_t idx = it - const_pool.begin();
			return mmem{.base = vreg_cpool, .disp = int32_t(idx * sizeof(any))};
		}
		mmem add_const(uint64_t c) { return add_const(any_t{c}); }

		void add_jump(mblock* from, mblock* to) {
			from->successors.emplace_back(to);
			to->predecessors.emplace_back(from);
		}
		void del_jump(mblock* from, mblock* to) {
			auto sit = range::find(from->successors, to);
			auto pit = range::find(to->predecessors, from);
			from->successors.erase(sit);
			to->predecessors.erase(pit);
		}
		mblock* add_block() {
			auto* bb   = &basic_blocks.emplace_back();
			bb->parent = this;
			bb->uid    = next_block++;
			return bb;
		}
		void print() const {
			for (const auto& b : basic_blocks) {
				printf("-- Block $%x", b.uid);
				if (b.hot < 0)
					printf(LI_CYN " [COLD %u]" LI_DEF, uint32_t(-b.hot));
				if (b.hot > 0)
					printf(LI_RED " [HOT  %u]" LI_DEF, uint32_t(b.hot));
				putchar('\n');
				b.print();
			}
		}
	};
}
