#include <vm/function.hpp>
#include <vm/string.hpp>

namespace li {
	function_proto* function_proto::create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, std::span<const line_info> lines) {
		msize_t routine_length = (msize_t) opcodes.size();
		LI_ASSERT(routine_length != 0);

		msize_t kval_n = (msize_t) kval.size();

		// Set function details.
		//
		function_proto* result  = L->alloc<function_proto>(sizeof(bc::insn) * routine_length + sizeof(any) * kval_n + sizeof(line_info) * lines.size());
		result->num_kval        = kval_n;
		result->length          = routine_length;
		result->src_chunk       = string::create(L);
		result->num_lines       = (msize_t) lines.size();

		// Copy the information, initialize all upvalues to nil.
		//
		std::copy_n(opcodes.data(), opcodes.size(), result->opcode_array);
		std::copy_n(kval.data(), kval.size(), result->kvals().begin());
		std::copy_n(lines.data(), lines.size(), result->lines().begin());
		return result;
	}

	function* function::create(vm* L, function_proto* proto) {
		function* f = L->alloc<function>(sizeof(any) * proto->num_uval);
		f->num_uval      = proto->num_uval;
		f->environment   = L->globals;
		f->invoke        = &vm_invoke;
		f->proto         = proto;
		fill_nil(f->upvalue_array, f->num_uval);
		return f;
	}
	function* function::create(vm* L, nfunc_t cb) {
		function* f = L->alloc<function>();
		f->num_uval                       = 0;
		f->invoke                         = cb;
		f->environment                    = nullptr;
		f->proto                          = nullptr;
		return f;
	}

	// GC enumerator.
	//
	void gc::traverse(gc::stage_context s, function_proto* o) {
		o->src_chunk->gc_tick(s);
		if (o->jfunc)
			o->jfunc->gc_tick(s);
		traverse_n(s, o->kvals().data(), o->kvals().size());
	}
	void gc::traverse(gc::stage_context s, function* o) {
		if (o->proto)
			o->proto->gc_tick(s);
		if (o->environment)
			((gc::header*) o->environment)->gc_tick(s);
		traverse_n(s, o->upvalue_array, o->num_uval);
	}
};