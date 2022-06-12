#pragma once
#include <lang/types.hpp>
#include <optional>
#include <util/common.hpp>
#include <util/format.hpp>
#include <vm/state.hpp>
#include <vm/traits.hpp>

namespace li::bc {
	// Bytecode definitions.
	//
#define LIGHTNING_ENUM_BC(_)                                                             \
	/* Unary operators */                                                                 \
	_(LNOT, reg, reg, ___, none) /* A=!B */                                               \
	_(ANEG, reg, reg, ___, neg)  /* A=-B */                                               \
	_(MOV, reg, reg, ___, none)  /* A=B */                                                \
	_(VLEN, reg, reg, ___, len)  /* A=LEN(B) */                                           \
                                                                                         \
	/* Binary operators.  */                                                              \
	_(AADD, reg, reg, reg, add)  /* A=B+C */                                              \
	_(ASUB, reg, reg, reg, sub)  /* A=B-C */                                              \
	_(AMUL, reg, reg, reg, mul)  /* A=B*C */                                              \
	_(ADIV, reg, reg, reg, div)  /* A=B/C */                                              \
	_(AMOD, reg, reg, reg, mod)  /* A=B%C */                                              \
	_(APOW, reg, reg, reg, pow)  /* A=B^C */                                              \
	_(LAND, reg, reg, reg, none) /* A=B&&C */                                             \
	_(LOR, reg, reg, reg, none)  /* A=B||C */                                             \
	_(NCS, reg, reg, reg, none)  /* A=B==null?C:B */                                      \
	_(CTY, reg, reg, imm, none)  /* A=TYPE(B)==C */                                       \
	_(CEQ, reg, reg, reg, eq)    /* A=B==C */                                             \
	_(CNE, reg, reg, reg, none)  /* A=B!=C */                                             \
	_(CLT, reg, reg, reg, lt)    /* A=B<C */                                              \
	_(CGE, reg, reg, reg, none)  /* A=B>=C */                                             \
	_(CGT, reg, reg, reg, none)  /* A=B>C */                                              \
	_(CLE, reg, reg, reg, le)    /* A=B<=C */                                             \
	_(VIN, reg, reg, reg, none)  /* A=C Includes B */                                     \
                                                                                         \
	/* Helpers */                                                                         \
	_(VJOIN, reg, reg, reg, none) /* A=JOIN(B<-C) */                                      \
	_(VDUP, reg, reg, ___, none)  /* A=DUP(B) */                                          \
	_(CCAT, reg, imm, ___, none)  /* A=CONCAT(A..A+B) */                                  \
	_(SETEH, rel, ___, ___, none) /* Exception Handler = A */                             \
	_(SETEX, reg, ___, ___, none) /* Last exception = A */                                \
	_(GETEX, reg, ___, ___, none) /* A = Last exception */                                \
                                                                                         \
	/* Trait operators. */                                                                \
	_(TRSET, reg, reg, imm, none) /* A[Trait C] = B */                                    \
	_(TRGET, reg, reg, imm, none) /* A = B[Trait C] */                                    \
                                                                                         \
	/* Constant operators. */                                                             \
	_(KIMM, reg, xmm, ___, none) /* A=Bitcast(BC) */                                      \
                                                                                         \
	/* Upvalue operators. */                                                              \
	_(UGET, reg, uvl, ___, none) /* A=UVAL[B] */                                          \
	_(USET, uvl, reg, ___, none) /* UVAL[A]=B */                                          \
                                                                                         \
	/* Table/Array operators. */                                                          \
	_(ANEW, reg, imm, ___, none)  /* A=ARRAY{Size=B} */                                   \
	_(TNEW, reg, imm, ___, none)  /* A=TABLE{Reserved=B} */                               \
	_(ADUP, reg, kvl, ___, none)  /* A=Duplicate(KVAL[B]) */                              \
	_(TDUP, reg, kvl, ___, none)  /* A=Duplicate(KVAL[B]) */                              \
	_(TGET, reg, reg, reg, none)  /* A=C[B] */                                            \
	_(TSET, reg, reg, reg, none)  /* C[A]=B */                                            \
	_(TGETR, reg, reg, reg, none) /* A=C[B] | Raw */                                      \
	_(TSETR, reg, reg, reg, none) /* C[A]=B | Raw */                                      \
                                                                                         \
	/* Closure operators. */                                                              \
	_(FDUP, reg, kvl, reg, none) /* A=Duplicate(KVAL[B]), A.UVAL[0]=C, A.UVAL[1]=C+1.. */ \
                                                                                         \
	/* Stack operators. */                                                                \
	_(PUSHR, reg, ___, ___, none) /* PUSH(A) */                                           \
	_(PUSHI, ___, xmm, ___, none) /* PUSH(A) */                                           \
                                                                                         \
	/* Type coercion. */                                                                  \
	_(TOSTR, reg, reg, ___, none) /* A = str(B) */                                        \
	_(TONUM, reg, reg, ___, none) /* A = num(B) */                                        \
	_(TOINT, reg, reg, ___, none) /* A = int(B) */                                        \
	_(TOBOOL, reg, reg, ___, none) /* A = bool(B) */                                      \
                                                                                         \
	/* Control flow. */                                                                   \
	_(CALL, reg, imm, ___, none)   /* A = Call(w/ B Args) */                              \
	_(RET, reg, ___, ___, none)  /* RETURN A */                                           \
	_(JMP, rel, ___, ___, none)  /* JMP A */                                              \
	_(JS, rel, reg, ___, none)   /* JMP A if B */                                         \
	_(JNS, rel, reg, ___, none)  /* JMP A if !B */                                        \
	_(ITER, rel, reg, reg, none) /* B[1,2] = C[B++].kv, JMP A if end */                   \
	/* Misc. */                                                                           \
	_(NOP, ___, ___, ___, none)  /* No-op */                                                                                      

	// Opcodes.
	//
	enum opcode : uint8_t {
#define BC_WRITE(name, a, b, c, trait) name,
		LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};
	using imm                   = int32_t;
	using reg                   = int32_t;
	using rel                   = int32_t;
	using pos                   = uint32_t;
	static constexpr pos no_pos = UINT32_MAX;

	// Write all descriptors.
	//
	enum class op_t : uint8_t { none, reg, uvl, kvl, imm, xmm, sp, rel, ___ = none };
	struct desc {
		// Opcode name and operand types.
		//
		const char* name;
		op_t        a, b, c;

		// Triggered operator trait.
		// - Does not include get, set, call, str or gc.
		// - Also does not have certain comparisons, should be inverted with ^1 in that case to find it out.
		//
		trait       operator_trait; 
	};
	static constexpr desc opcode_descs[] = {
#define BC_WRITE(name, a, b, c, t) {LI_STRINGIFY(name), op_t::a, op_t::b, op_t::c, trait::t},
		 LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};
	static const desc& opcode_details(opcode o) { return opcode_descs[uint8_t(o)]; }

	// Define the instruction type.
	//
#pragma pack(push, 1)
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
		void print(msize_t ip) const {
			auto& d = opcode_details(o);

			std::optional<rel> rel_pr;

			auto print_op = [&](op_t o, reg value) LI_INLINE {
				char        op[16];
				const char* col = "";
				switch (o) {
					case op_t::none:
						op[0] = 0;
						break;
					case op_t::reg:
						if (value < 0) {
							if (value == FRAME_SELF) {
								col = LI_GRN;
								strcpy(op, "self");
							} else if (value == FRAME_TARGET) {
								col = LI_GRN;
								strcpy(op, "$F");
							} else {
								col = LI_YLW;
								snprintf(op, std::size(op), "a%u", (uint32_t) - (value + 3));
							}
						} else {
							col = LI_RED;
							snprintf(op, std::size(op), "r%u", (uint32_t) value);
						}
						break;
					case op_t::sp:
						if (value > FRAME_SIZE) {
							col = LI_YLW;
							snprintf(op, std::size(op), "@a%u", (uint32_t) (value - FRAME_SIZE));
						} else {
							col = LI_RED;
							snprintf(op, std::size(op), "@undef");
						}
						break;
					case op_t::rel:
						if (value >= 0) {
							col = LI_GRN;
							snprintf(op, std::size(op), "@%x", ip + 1 + value);
						} else {
							col = LI_YLW;
							snprintf(op, std::size(op), "@%x", ip + 1 + value);
						}
						rel_pr = (rel) value;
						break;
					case op_t::uvl:
						col = LI_CYN;
						snprintf(op, std::size(op), "u%u", (uint32_t) value);
						break;
					case op_t::kvl:
						col = LI_BLU;
						snprintf(op, std::size(op), "k%u", (uint32_t) value);
						break;
					case op_t::imm:
						col = LI_BLU;
						snprintf(op, std::size(op), "$%d", (int32_t) value);
						break;
				}
				printf("%s%-12s " LI_DEF, col, op);
			};

			// IP: OP
			printf(LI_PRP "%05x:" LI_BRG " %-8s", ip, d.name);
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
#pragma pack(pop)
};
