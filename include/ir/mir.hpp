#pragma once
#include <list>
#include <vector>
#include <util/common.hpp>
#include <util/format.hpp>
#include <util/bitset.hpp>
#include <ir/arch.hpp>
#include <ir/proc.hpp>

// Common definitions.
//
namespace li::ir {
	// Target defined enum, xor with 1 should reverse the condition.
	//
	enum flag_id : int32_t {};

	// Machine register type.
	//
	enum class regclass : uint32_t /*:2*/ {
		null,
		flag,
		virt,
		phys,
	};
	using preg = arch::reg;
	enum vreg : int32_t {
		//vreg_vm    = 1,  // arguments[0], vm*
		vreg_args  = 1,  // arguments[1], any*
		vreg_nargs = 2,  // arguments[2], int
		vreg_tos   = 3,  // calculated from vreg_args + the final local size
		vreg_cpool = 4,  // constant pool, rip-rel
		vreg_first = 5,  // ...
	};
	static constexpr const char* vreg_names[] = {"$null", "$args", "$nargs", "$tos", "$cpool"};
	struct mreg {
		int32_t  id : 30 = 0;
		regclass cl : 2  = regclass::null;

		// Constructed from preg, vreg, flag and nullopt.
		//
		constexpr mreg(flag_id id) : id(int32_t(id)), cl(regclass::flag) {}
		constexpr mreg(std::nullopt_t) : id(0), cl(regclass::null) {}
		constexpr mreg(preg r) : id(int32_t(r)), cl(regclass::phys) { LI_ASSERT(id != 0); }
		constexpr mreg(vreg r) : id(int32_t(r)), cl(regclass::virt) { LI_ASSERT(id != 0); }

		// Default copy and construction.
		//
		constexpr mreg()                       = default;
		constexpr mreg(const mreg&)            = default;
		constexpr mreg& operator=(const mreg&) = default;

		// Observers.
		//
		constexpr preg phys() const {
			LI_ASSERT(is_phys());
			return preg(id);
		}
		constexpr vreg virt() const {
			LI_ASSERT(is_virt());
			return vreg(id);
		}
		constexpr flag_id flag() const {
			LI_ASSERT(is_flag());
			return flag_id(id);
		}

		// Gets a zero-based unique identifier.
		//
		constexpr msize_t uid() const {
			msize_t x = id < 0 ? msize_t(-id) : msize_t(id);
			return (x << 3) + (id < 0) + (msize_t(cl) << 1);
		}
		static constexpr mreg from_uid(msize_t i) {
			mreg result = {};
			result.id   = i >> 3;
			if (i & 1)
				result.id = -result.id;
			result.cl = regclass((i >> 1) & 3);
			return result;
		}

		// Properties.
		//
		constexpr bool     is_phys() const { return cl == regclass::phys; }
		constexpr bool     is_virt() const { return cl == regclass::virt; }
		constexpr bool     is_null() const { return cl == regclass::null; }
		constexpr bool     is_flag() const { return cl == regclass::flag; }
		constexpr bool     is_gp() const { return cl >= regclass::virt && id > 0; }
		constexpr bool     is_fp() const { return cl >= regclass::virt && id < 0; }
		explicit constexpr operator bool() const { return !is_null(); }

		// Comparison.
		//
		constexpr bool operator==(const mreg& o) const { return li::bit_cast<int32_t>(*this) == li::bit_cast<int32_t>(o); }
		constexpr bool operator!=(const mreg& o) const { return li::bit_cast<int32_t>(*this) != li::bit_cast<int32_t>(o); }
		constexpr bool operator<(const mreg& o) const { return li::bit_cast<int32_t>(*this) < li::bit_cast<int32_t>(o); }

		// String conversion.
		//
		std::string to_string() const {
			if (is_null()) {
				return LI_RED "null" LI_DEF;
			} else if (is_flag()) {
				return util::fmt(LI_BLU "%%f%x" LI_DEF, id);
			} else if (is_virt()) {
				if (is_fp()) {
					return util::fmt(LI_CYN "%%vf%u" LI_DEF, -id);
				} else {
					if (id < vreg_first) {
						return util::fmt(LI_RED "%s" LI_DEF, vreg_names[id]);
					} else {
						return util::fmt(LI_YLW "%%v%u" LI_DEF, id - vreg_first);
					}
				}
			} else {
				return arch::name_reg(arch::to_native(phys()));
			}
		}
	};

	// Machine memory.
	//
	struct mmem {
		mreg base = {};
		mreg index = {};
		int8_t scale = 0;
		int32_t disp  = 0;

		// String conversion.
		//
		std::string to_string() const {
			std::string result;
			if (scale) {
				result = util::fmt("[%s+%s*%u", base.to_string().c_str(), index.to_string().c_str(), uint32_t(scale));
			} else {
				result = "[" + base.to_string();
			}

			if (disp > 0) {
				result += util::fmt(LI_BRG "+0x%x" LI_DEF, disp);
			} else if (disp < 0) {
				result += util::fmt(LI_BRG "-0x%x" LI_DEF, -disp);
			}
			result += "]";
			return result;
		}
	};

	// Machine operand.
	//
	struct mop {
		union {
			int64_t i64;
			mmem mem;
			mreg reg;
		};

		// Not constexpr since we abuse the union.
		//
		mop() { mem.scale = -3; }
		mop(std::nullopt_t) : mop() {}
		mop(int64_t i) : i64(i) { mem.scale = -2; }
		mop(mreg r) : reg(r) { mem.scale = -1; }
		mop(mmem m) : mem(m) { LI_ASSERT(mem.scale >= 0); }
		mop(flag_id f) : mop(mreg(f)) {}
		mop(any x) : mop(int64_t(x.value)) {}

		// Copy.
		//
		mop(const mop& o) : mem(o.mem) {}
		mop& operator=(const mop& o) {
			mem = o.mem;
			return *this;
		}

		// Observers.
		//
		bool     is_null() const { return mem.scale <= -3; }
		bool     is_const() const { return mem.scale == -2; }
		bool     is_reg() const { return mem.scale == -1; }
		bool     is_mem() const { return mem.scale >= 0; }
		explicit operator bool() const { return !is_null(); }

		// String conversion.
		//
		std::string to_string() const {
			if (is_reg())
				return reg.to_string();
			else if (is_mem())
				return mem.to_string();
			else if (is_const())
				return util::fmt(LI_GRN "0x%llx" LI_DEF, i64);
			else
				return LI_RED "null" LI_DEF;
		}
	};

	// Target specific information.
	//
	struct ins_target_info {
		uint64_t side_effects : 1  = false;
		uint64_t trashes_flags : 1 = true;
		int64_t  rsvd : 62         = 0;
	};

	// Machine instruction.
	//
	enum class vop {
		null,

		movf,   // fpreg = fpreg/gpreg/const
		movi,   // gpreg = fpreg/gpreg/const

		izx8,   // gpreg = u8(gpreg)
		izx16,  // gpreg = u16(gpreg)
		izx32,  // gpreg = u32(gpreg)
		isx8,   // gpreg = i8(gpreg)
		isx16,  // gpreg = i16(gpreg)
		isx32,  // gpreg = i32(gpreg)
		fx32,   // fpreg = f64->f32(fpreg)
		fx64,   // fpreg = f32->f64(fpreg)
		icvt,   // gpreg = f64(fpreg)
		fcvt,   // fpreg = f64(gpreg)

		loadi8,   // gpreg = i8[mem]
		loadi16,  // gpreg = i16[mem]
		loadi32,  // gpreg = i32[mem]
		loadi64,  // gpreg = i64[mem]
		loadf32,  // fpreg = fp32[mem]
		loadf64,  // fpreg = fp64[mem]
		storei8,   // i8[mem] = gpreg
		storei16,  // i16[mem] = gpreg
		storei32,  // i32[mem] = gpreg
		storei64,  // i64[mem] = gpreg
		storef32,  // fp32[mem] = fpreg
		storef64,  // fp64[mem] = fpreg

		setcc,   // reg = flag
		select,  // reg = flag ? reg1 : reg2

		// side-effect group.
		call,           // call i64, [implicit args & nonvol clobber]
		js,             // cnd true block, false block
		jmp,            // block
		ret,            // i1
		unreachable
		// TODO: Profile instruction for PGO.
	};
	static constexpr const char* vop_names[] = {
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

		 "setcc",
		 "select",
		 "call",
		 "js",
		 "jmp",
		 "ret",
		 "unreachable",
	};
	using pop = arch::native_mnemonic;
	struct minsn {
		// Virtual or physical mnemonic.
		//
		int32_t mnemonic : 31  = 0;
		int32_t is_virtual : 1 = 0;

		// Up to 4 operands and 1 register result.
		//
		mop  arg[4] = {std::nullopt};
		mreg out    = {};

		// Architecture specific information.
		//
		ins_target_info target_info = {};

		// No spill mark.
		//
		bool no_spill = false;

		// Default construction and copy.
		//
		minsn()                        = default;
		minsn(const minsn&)            = default;
		minsn& operator=(const minsn&) = default;

		// Construction with mnemonic + operands.
		//
		template<typename... Tx>
		minsn(vop v, mreg out, Tx... arg) : mnemonic(int32_t(v)), is_virtual(true), out(out), arg{arg...} {}
		template<typename... Tx>
		minsn(pop v, ins_target_info target_info, mreg out, Tx... arg) : mnemonic(int32_t(v)), is_virtual(false), out(out), arg{arg...}, target_info(target_info) {}

		// Observers.
		//
		bool is(pop p) const { return !is_virtual && getp() == p; }
		bool is(vop p) const { return is_virtual && getv() == p; }
		bool is_null() const { return is_virtual && !mnemonic; }

		vop getv() const {
			LI_ASSERT(is_virtual && mnemonic != 0);
			return vop(mnemonic);
		}
		pop getp() const {
			LI_ASSERT(!is_virtual);
			return pop(mnemonic);
		}
		size_t num_args() const {
			for (size_t i = 0;; i++) {
				if (i == std::size(arg) || !arg[i])
					return i;
			}
			assume_unreachable();
		}
		explicit operator bool() const { return !is_null(); }

		// Register enumerator.
		// - F(Reg, IsRead)
		//
		template<typename F>
		void for_each_reg(F&& fn) const {
			for (auto& a : arg) {
				if (!a) {
					break;
				}
				if (a.is_reg()) {
					fn(a.reg, true);
				}
				if (a.is_mem()) {
					if (a.mem.base)
						fn(a.mem.base, true);
					if (a.mem.scale)
						fn(a.mem.index, true);
				}
			}
			if (out)
				fn(out, false);
		}
		template<typename F>
		void for_each_reg_w_implicit(F&& fn) const {
			if (is(vop::call)) {
				for (auto r : arch::gp_argument)
					fn(mreg(arch::from_native(r)), true);
				for (auto r : arch::fp_argument)
					fn(mreg(arch::from_native(r)), true);
				for_each_reg(fn);
				for (auto r : arch::gp_volatile)
					fn(mreg(arch::from_native(r)), false);
				for (auto r : arch::fp_volatile)
					fn(mreg(arch::from_native(r)), false);
			} else {
				for_each_reg(std::forward<F>(fn));
			}
		}

		// Simple use checks.
		//
		bool uses_register(mreg r) const {
			bool x = false;
			for_each_reg([&](const mreg& a, bool) { x = x || a == r; });
			return x;
		}
		bool writes_to_register(mreg r) const {
			bool x = false;
			for_each_reg([&](const mreg& a, bool w) { x = x || (w && a == r); });
			return x;
		}
		bool reads_from_register(mreg r) const {
			bool x = false;
			for_each_reg([&](const mreg& a, bool w) { x = x || (!w && a == r); });
			return x;
		}
		bool trashes_flags() const {
			if (out.is_flag() || is(vop::call))
				return true;
			else if (!is_virtual && target_info.trashes_flags)
				return true;
			return false;
		}
		bool writes_to_memory() const {
			if (is_virtual) {
				return vop::storei8 <= getv() && getv() <= vop::storef64;
			} else {
				return !out;
			}
		}
		bool has_side_effects() const {
			if (is_virtual) {
				return getv() >= vop::call;
			} else {
				return target_info.side_effects;
			}
		}
		bool is_move_between_same_class() const {
			if (is_virtual && (vop::movf <= getv() && getv() <= vop::fx64)) {
				return arg[0].is_reg() && out.is_fp() == arg[0].reg.is_fp();
			}
			return false;
		}

		// String conversion.
		//
		std::string to_string() const {
			if (is_null())
				return "INVALID";

			std::string result;
			if (no_spill) {
				result = LI_BLU "nospill " LI_DEF;
			}

			if (out) {
				result += out.to_string() + LI_DEF " = ";
			}
			if (is_virtual) {
				auto v = getv();
				result += LI_PRP;
				result += vop_names[size_t(v)];
				result += LI_DEF;
			} else {
				result += LI_RED;
				result += arch::name_mnemonic(getp());
				result += LI_DEF;
			}
			for (auto& a : arg) {
				if (!a)
					break;
				result += " ";
				result += a.to_string();
			}
			result += LI_DEF;
			return result;
		}
	};

	// Machine block and procedure.
	//
	struct mprocedure;
	struct mblock {
		mprocedure*        parent       = nullptr;  // Owning procedure.
		msize_t            uid          = 0;        // Unique identifier of the block.
		int32_t            hot          = 0;        // Hotness of the block.
		std::vector<minsn> instructions = {};       // List of instructions.

		// Predecessor and successors.
		//
		std::vector<mblock*> predecessors;
		std::vector<mblock*> successors;

		// Visitor temporaries.
		//
		uint64_t visited = 0;

		// Data-flow analysis results.
		//
		util::bitset df_def, df_ref;
		util::bitset df_in_live, df_out_live;

		// Offset to beginning in assembly.
		//
		size_t asm_loc = 0;

		// Appends an instruction at the end of the block and returns the IP.
		//
		template<typename O, typename... Tx>
		size_t append(O v, mreg out, Tx... arg) {
			size_t n = instructions.size();
			instructions.push_back(minsn{v, out, arg...});
			return n;
		}

		// Cute shortcut to access current procedure.
		//
		mprocedure* operator->() const { return parent; }

		// Printer.
		//
		void print() const {
			for (auto& i : instructions) {
				printf("\t%s\n", i.to_string().c_str());
			}
		}
	};
	struct mprocedure {
		procedure*           source       = nullptr;  // Source procedure.
		std::list<mblock>    basic_blocks = {};       // List of basic blocks.
		int32_t              next_reg_i   = 0;        // Name of the next virtual register.
		int32_t              next_reg_f   = 0;        //
		uint32_t             next_block   = 0;        // Name of the next block.
		std::vector<uint8_t> code         = {};       // Generated code.
		std::vector<any>     const_pool   = {};       // Pool of constants.

		// Used register and spill slot info
		//
		size_t  used_gp_mask      = 0;
		size_t  used_fp_mask      = 0;
		int32_t used_stack_length = arch::home_size;

		// Target-specific assembly and relocation info.
		//
		std::vector<uint8_t>                   epilogue;
		std::vector<uint8_t>                   assembly;
		std::vector<std::pair<size_t, size_t>> reloc_info;

		// Maximum local index for VCALL.
		//
		msize_t max_stack_slot = 0;

		// Visitor temporaries.
		//
		uint64_t next_visited_mark = 0x50eaeb7446b52b12;

		// Gets the next register.
		//
		mreg next_gp() { return vreg(vreg_first + next_reg_i++); }
		mreg next_fp() { return vreg(-++next_reg_f); }

		// Adds a constant and returns it as a memory operand.
		//
		mmem add_const(any c) {
			auto it = range::find(const_pool, c);
			if (it == const_pool.end()) {
				const_pool.emplace_back(c);
				it = const_pool.end() - 1;
			}
			size_t idx = it - const_pool.begin();
			return mmem{.base = vreg_cpool, .disp = int32_t(idx * sizeof(any))};
		}
		mmem add_const(uint64_t c) { return add_const(any(std::in_place, c)); }

		// Adds or deletes a jump.
		//
		void add_jump(mblock* from, mblock* to) {
			auto sit = range::find(from->successors, to);
			auto pit = range::find(to->predecessors, from);
			LI_ASSERT(sit == from->successors.end());
			LI_ASSERT(pit == to->predecessors.end());
			from->successors.emplace_back(to);
			to->predecessors.emplace_back(from);
		}
		void del_jump(mblock* from, mblock* to) {
			auto sit = range::find(from->successors, to);
			auto pit = range::find(to->predecessors, from);
			LI_ASSERT(sit != from->successors.end());
			LI_ASSERT(pit != to->predecessors.end());
			from->successors.erase(sit);
			to->predecessors.erase(pit);
		}

		// Adds a new block.
		//
		mblock* add_block() {
			auto* bb   = &basic_blocks.emplace_back();
			bb->parent = this;
			bb->uid    = next_block++;
			return bb;
		}

		// Printer.
		//
		void print() const {
			for (auto& b : basic_blocks) {
				printf("-- Block $%x", b.uid);
				if (b.hot < 0)
					printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) -b.hot);
				if (b.hot > 0)
					printf(LI_RED " [HOT  %u]" LI_DEF, (uint32_t) b.hot);
				putchar('\n');
				b.print();
			}
		}
	};
};

