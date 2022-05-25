#pragma once
#include <optional>
#include <util/common.hpp>
#include <util/format.hpp>
#include <lang/types.hpp>

namespace lightning::bc {
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
	_(SCAT, reg, reg, reg) /* A=B..C */                                                 \
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
	_(KGET, reg, kvl, ___) /* A=KVAL[B] */                                              \
                                                                                       \
	/* Upvalue operators. */                                                            \
	_(UGET, reg, uvl, ___) /* A=UVAL[B] */                                              \
	_(USET, uvl, reg, ___) /* UVAL[A]=B */                                              \
                                                                                       \
	/* Table operators. */                                                              \
	_(TNEW, reg, imm, ___) /* A=TABLE{Reserved=B} */                                    \
	_(TDUP, reg, kvl, ___) /* A=Duplicate(KVAL[B]) */                                   \
	_(TGET, reg, reg, reg) /* A=C[B] */                                                 \
	_(TSET, reg, reg, reg) /* C[A]=B */                                                 \
	_(GGET, reg, reg, ___) /* A=G[B] */                                                 \
	_(GSET, reg, reg, ___) /* G[A]=B */                                                 \
                                                                                       \
	/* Closure operators. */                                                            \
	_(FDUP, reg, kvl, reg) /* A=Duplicate(KVAL[B]), A.UVAL[0]=C, A.UVAL[1]=C+1... */    \
                                                                                       \
	/* Control flow. */                                                                 \
	_(CALL, reg, imm, rel) /* CALL A(B x args @(a+1, a+2...)), JMP C if throw */        \
	_(RETN, reg, ___, ___) /* RETURN A */                                               \
	_(THRW, reg, ___, ___) /* THROW A (if A != None) */                                 \
	_(JMP,  rel, ___, ___) /* JMP A */                                                  \
	_(JCC,  rel, reg, ___) /* JMP A if B */                                             \
	/* Misc. */                                                                         \
	_(BP,   ___, ___, ___)   /* Breakpoint */

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
	
	// Write all descriptors.
	//
	enum class op_t : uint8_t { none, reg, uvl, kvl, imm, xmm, rel, ___ = none };
	using  op = uint16_t;
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
					case op_t::reg:
						if (value >= 0) {
							col = LI_RED;
							sprintf_s(op, "r%u", (uint32_t) value);
						} else {
							col = LI_YLW;
							sprintf_s(op, "a%u", (uint32_t) - (value + 1));
						}
						break;
					case op_t::rel:
						if (value >= 0) {
							col = LI_GRN; 
							sprintf_s(op, "@%x", ip + value);
						} else {
							col = LI_YLW; 
							sprintf_s(op, "@%x", ip - value);
						}
						rel_pr = (rel) value;
						break;
					case op_t::uvl:
						col = LI_CYN;
						sprintf_s(op, "u%u", (uint32_t) value);
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
				char        op[32];

				core::any v{std::in_place, xmm()};
				switch (v.type()) {
					case core::type_number:
						sprintf_s(op, "%lf", v.as_num());
						break;
					case core::type_false:
						strcpy_s(op, "False");
						break;
					case core::type_true:
						strcpy_s(op, "True");
						break;
					case core::type_none:
						strcpy_s(op, "None");
						break;
					default:
						sprintf_s(op, "0x%016llx", v.value);
						break;
				}
				printf(LI_BLU "%-25s " LI_DEF, op);
			} else {
				print_op(d.b, b);
				print_op(d.c, c);
			}
			printf("|");

			// Details for relative.
			//
			if (rel_pr) {
				if (*rel_pr < 0) {
					printf(LI_GRN " v" LI_DEF);
				} else {
					printf(LI_RED " ^" LI_DEF);
				}
			}

			printf("\n");
		}
	};
};