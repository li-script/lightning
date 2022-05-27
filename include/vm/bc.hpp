#pragma once
#include <optional>
#include <util/common.hpp>
#include <util/format.hpp>
#include <lang/types.hpp>

namespace li::bc {
	// Bytecode definitions.
	//
#define LIGHTNING_ENUM_BC(_)                                                           \
	/* Unary operators */                                                               \
	_(LNOT, reg, reg, ___) /* A=!B */                                                   \
	_(ANEG, reg, reg, ___) /* A=-B */                                                   \
	_(MOV,  reg, reg, ___) /* A=B */                                                    \
	_(TYPE, reg, reg, ___) /* A=TYPE(B) */                                              \
                                                                                       \
	/* Binary operators.  */                                                            \
	_(AADD, reg, reg, reg) /* A=B+C */                                                  \
	_(ASUB, reg, reg, reg) /* A=B-C */                                                  \
	_(AMUL, reg, reg, reg) /* A=B*C */                                                  \
	_(ADIV, reg, reg, reg) /* A=B/C */                                                  \
	_(AMOD, reg, reg, reg) /* A=B%C */                                                  \
	_(APOW, reg, reg, reg) /* A=B^C */                                                  \
	_(LAND, reg, reg, reg) /* A=B&&C */                                                 \
	_(LOR,  reg, reg, reg) /* A=B||C */                                                 \
	_(CEQ,  reg, reg, reg) /* A=B==C */                                                 \
	_(CNE,  reg, reg, reg) /* A=B!=C */                                                 \
	_(CLT,  reg, reg, reg) /* A=B<C */                                                  \
	_(CGT,  reg, reg, reg) /* A=B>C */                                                  \
	_(CLE,  reg, reg, reg) /* A=B<=C */                                                 \
	_(CGE,  reg, reg, reg) /* A=B>=C */                                                 \
	_(CMOV, reg, reg, reg) /* A=B?C:None */                                             \
                                                                                       \
	/* Constant operators. */                                                           \
	_(KIMM, reg, xmm, ___) /* A=Bitcast(BC) */                                          \
                                                                                       \
	/* Upvalue operators. */                                                            \
	_(UGET, reg, uvl, ___) /* A=UVAL[B] */                                              \
	_(USET, uvl, reg, ___) /* UVAL[A]=B */                                              \
                                                                                       \
	/* Parameter operators. */                                                          \
	_(PGET, reg, pvl, ___) /* A=ARGS[B] */                                              \
	_(PSET, pvl, reg, ___) /* ARGS[A]=B */                                              \
                                                                                       \
	/* Global operators. */                                                             \
	_(GGET, reg, reg, ___) /* A=G[B] */                                                 \
	_(GSET, reg, reg, ___) /* G[A]=B */                                                 \
                                                                                       \
	/* Table/Array operators. */                                                        \
	_(ANEW, reg, imm, ___) /* A=ARRAY{Reserved=B} */                                    \
	_(TNEW, reg, imm, ___) /* A=TABLE{Reserved=B} */                                    \
	_(ADUP, reg, kvl, ___) /* A=Duplicate(KVAL[B]) */                                   \
	_(TDUP, reg, kvl, ___) /* A=Duplicate(KVAL[B]) */                                   \
	_(TGET, reg, reg, reg) /* A=C[B] */                                                 \
	_(TSET, reg, reg, reg) /* C[A]=B */                                                 \
                                                                                       \
	/* Closure operators. */                                                            \
	_(FDUP, reg, kvl, reg) /* A=Duplicate(KVAL[B]), A.UVAL[0]=C, A.UVAL[1]=C+1... */    \
                                                                                       \
	/* Control flow. */                                                                 \
	_(CALL, reg, imm, ___) /* A = Callsite, B = Argcount */                             \
	_(INVK, rel, reg, imm) /* A = Catchpad, B = Callsite, C = Argcount */               \
	_(RET,  reg, ___, ___) /* RETURN A */                                               \
	_(THRW, reg, ___, ___) /* THROW A (if A != None) */                                 \
	_(JMP,  rel, ___, ___) /* JMP A */                                                  \
	_(JS,   rel, reg, ___) /* JMP A if B */                                             \
	_(JNS,  rel, reg, ___) /* JMP A if !B */                                            \
	_(ITER, rel, reg, reg) /* B[1,2] = C[B].kv, JMP A if end */                         \
	/* Misc. */                                                                         \
	_(NOP,  ___, ___, ___) /* No-op */ \
	_(BP,   ___, ___, ___) /* Breakpoint */

	// Opcodes.
	//
	enum opcode : uint8_t {
#define BC_WRITE(name, a, b, c) name,
		LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};
	using imm = int32_t;
	using reg = int32_t;
	using rel = int32_t;
	using pos = uint32_t;
	static constexpr pos no_pos = UINT32_MAX;

	// Magic upvalues.
	//
	static constexpr reg uval_fun = -1;
	static constexpr reg uval_env = -2;
	static constexpr reg uval_glb = -3;
	
	// Write all descriptors.
	//
	enum class op_t : uint8_t { none, reg, uvl, pvl, kvl, imm, xmm, rel, ___ = none };
	struct desc {
		const char* name;
		op_t        a, b, c;
	};
	static constexpr desc opcode_descs[] = {
#define BC_WRITE(name, a, b, c) {LI_STRINGIFY(name), op_t::a, op_t::b, op_t::c},
		 LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};
	static const desc& opcode_details(opcode o) { return opcode_descs[uint8_t(o)]; }

	// Define the instruction type.
	//
	struct insn {
		opcode o;
		reg    a = 0;
		reg    b = 0;
		reg    c = 0;

		// Extended immediate, always at B:C.
		//
		uint64_t&       xmm() { return *(uint64_t*) &b; }
		const uint64_t& xmm() const { return *(const uint64_t*) &b; }

		// Prints an instruction.
		//
		void print(uint32_t ip) const {
			auto& d = opcode_details(o);

			std::optional<rel> rel_pr;

			auto print_op = [&](op_t o, reg value) LI_INLINE {
				char op[16];
				const char* col = "";
				switch (o) {
					case op_t::none:
						op[0] = 0;
						break;
					case op_t::pvl:
						col = LI_YLW;
						sprintf_s(op, "a%u", (uint32_t) value);
						break;
					case op_t::reg:
						col = LI_RED;
						sprintf_s(op, "r%u", (uint32_t) value);
						break;
					case op_t::rel:
						if (value >= 0) {
							col = LI_GRN; 
							sprintf_s(op, "@%x", ip + 1 + value);
						} else {
							col = LI_YLW; 
							sprintf_s(op, "@%x", ip + 1 + value);
						}
						rel_pr = (rel) value;
						break;
					case op_t::uvl:
						if (value == bc::uval_fun) {
							col = LI_GRN;
							strcpy_s(op, "$F");
						} else if (value == bc::uval_env) {
							col = LI_GRN;
							strcpy_s(op, "$E");
						} else if (value == bc::uval_glb) {
							col = LI_GRN;
							strcpy_s(op, "$G");
						} else {
							col = LI_CYN;
							sprintf_s(op, "u%u", (uint32_t) value);
						}
						break;
					case op_t::kvl:
						col    = LI_BLU; 
						sprintf_s(op, "k%u", (uint32_t) value);
						break;
					case op_t::imm:
						col = LI_BLU;
						sprintf_s(op, "$%d", (int32_t) value);
						break;
				}
				printf("%s%-12s " LI_DEF, col, op);
			};

			// IP: OP
			printf(LI_PRP "%05x:" LI_BRG " %-6s", ip, d.name);
			// ... Operands.
			print_op(d.a, a);

			if (d.b == op_t::xmm) {
				std::string op = any{std::in_place, xmm()}.to_string();
				if (op.size() > 25) {
					op[22] = '.';
					op[23] = '.';
					op[24] = '.';
					op.resize(25);
				}
				printf(LI_BLU "%-25s " LI_DEF, op.c_str());
			} else {
				print_op(d.b, b);
				print_op(d.c, c);
			}
			printf("|");

			// Details for relative.
			//
			if (rel_pr) {
				if (*rel_pr >= 0) {
					printf(LI_GRN " v" LI_DEF);
				} else {
					printf(LI_RED " ^" LI_DEF);
				}
			}

			printf("\n");
		}
	};
};
