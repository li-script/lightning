#pragma once
#include <lang/types.hpp>
#include <vm/bc.hpp>
#include <tuple>

namespace lightning::core {
	struct vm;

	// Applies the unary/binary operator the values given. On failure (e.g. type mismatch),
	// returns the exception as the result and false for the second return.
	//
	std::pair<any, bool> apply_unary(vm* L, any a, bc::opcode op);
	std::pair<any, bool> apply_binary(vm* L, any a, any b, bc::opcode op);
};