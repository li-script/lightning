#include <cstring>
#include <limits>
#include <vm/function.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

#if LI_VTUNE
	#include <jitprofiling.h>
#endif

namespace li {
	jfunction* jfunction::create(vm* L, function_proto* owner, platform::code_memory&& memory, layout code_layout, std::span<const any> constants,
		 std::unique_ptr<inline_cache_array> inline_caches) {
		LI_ASSERT(memory.published());
		LI_ASSERT(code_layout.code_size != 0 && code_layout.code_size <= memory.capacity());
		LI_ASSERT(code_layout.native_stack_bytes != 0);
		LI_ASSERT(code_layout.vm_stack_slots <= size_t(std::numeric_limits<slot_t>::max()));

		const bool shared_code     = owner && owner->shared;
		size_t     reference_count = 0;
		for (any value : constants) {
			reference_count += value.is_gc();
			if (shared_code && value.is_gc() && value.as_gc() && !value.as_gc()->is_static && !value.as_gc()->shared)
				L->panic("shared JIT code references a private constant");
		}
		if (reference_count > std::numeric_limits<msize_t>::max() || reference_count > std::numeric_limits<size_t>::max() / sizeof(any)) {
			L->panic("JIT constant reference table is too large");
		}

		jfunction* result =
			 shared_code ? shared::allocate<jfunction>(
									 L, reference_count * sizeof(any), std::move(memory), code_layout, std::move(inline_caches), msize_t(reference_count))
							 : L->alloc<jfunction>(reference_count * sizeof(any), std::move(memory), code_layout, std::move(inline_caches), msize_t(reference_count));
		auto   references = result->references();
		size_t index      = 0;
		for (any value : constants) {
			if (value.is_gc()) {
				references[index++] = value;
				rc::retain(value);
			}
		}
		return result;
	}

	jfunction::~jfunction() noexcept { LI_ASSERT(active_invocations == 0); }

	const void* jfunction::begin_invoke() noexcept {
		std::lock_guard guard(publication_lock);
		const void*     entry = entry_address();
		if (!entry)
			return nullptr;
		LI_ASSERT(active_invocations != std::numeric_limits<uint32_t>::max());
		++active_invocations;
		return entry;
	}

	void jfunction::end_invoke() noexcept {
		std::lock_guard guard(publication_lock);
		LI_ASSERT(active_invocations != 0);
		--active_invocations;
	}

	std::error_code jfunction::publish_breakpoint() noexcept {
		if (shared)
			return std::make_error_code(std::errc::operation_not_permitted);
		std::lock_guard guard(publication_lock);
		if (active_invocations != 0)
			return std::make_error_code(std::errc::device_or_resource_busy);
#if LI_ARCH_X86 || LI_ARCH_ARM
		if (code_length < platform::breakpoint_bytes.size())
			return std::make_error_code(std::errc::result_out_of_range);

		if (auto error = memory.reopen_for_write(platform::inactive_code))
			return error;
		if (auto error = memory.write(0, platform::breakpoint_bytes))
			return error;
		return memory.publish();
#else
		return std::make_error_code(std::errc::not_supported);
#endif
	}

	any_t LI_CC jit_dispatch(vm* L, any* args, slot_t n_args) {
		any target = args[2];
		if (!target.is_fn() || !target.as_fn()->proto)
			return L->error("JIT entry has no published code");

		jfunction* code = target.as_fn()->proto->load_jfunc();
		if (!code)
			return L->error("JIT entry has no published code");

		// The published FRAME_TARGET owns the function, which owns its prototype,
		// which owns this jfunction for the entire dispatch. begin/end_invoke
		// separately prevents executable-code replacement while native code runs.
		const void* entry = code->begin_invoke();
		if (!entry)
			return L->error("JIT code is not published");

		struct invocation_guard {
			jfunction* code;

			~invocation_guard() { code->end_invoke(); }
		} guard{code};

		if (!L->vm_stack_headroom_available(code->vm_stack_slots())) [[unlikely]]
			return L->vm_stack_exhausted();
		if (!L->native_stack_headroom_available(code->native_stack_bytes())) [[unlikely]]
			return L->error("native stack exhausted while entering script frame");

		return platform::invoke_generated_code<nfunc_t>(entry, L, args, n_args);
	}

	function_proto* function_proto::create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, std::span<const line_info> lines) {
		msize_t routine_length = (msize_t) opcodes.size();
		LI_ASSERT(routine_length != 0);

		msize_t kval_n = (msize_t) kval.size();

		// Set function details.
		//
		function_proto* result = L->alloc<function_proto>(function_proto::payload_size(routine_length, kval_n, (msize_t) lines.size()));
		result->num_kval       = kval_n;
		result->length         = routine_length;
		result->src_chunk      = string::create(L);
		result->num_lines      = (msize_t) lines.size();

		// Copy the information, initialize all upvalues to nil.
		//
		std::copy_n(opcodes.data(), opcodes.size(), result->opcode_array);
		std::copy_n(kval.data(), kval.size(), result->kvals().begin());
		for (any value : result->kvals()) {
			rc::retain(value);
		}
		std::copy_n(lines.data(), lines.size(), result->lines().begin());
		return result;
	}

	function* function::create(vm* L, function_proto* proto) {
		function* f = L->alloc<function>(sizeof(any) * proto->num_uval);
		f->num_uval = proto->num_uval;
		f->invoke   = &vm_invoke;
		f->proto    = proto;
		rc::retain(proto);
		fill_nil(f->uvals().data(), f->num_uval);
		return f;
	}
	function* function::create(vm* L, nfunc_t cb) {
		function* f = L->alloc<function>();
		f->num_uval = 0;
		f->invoke   = cb;
		f->proto    = nullptr;
		return f;
	}

	function* function::duplicate(vm* L, bool force) const {
		if (!num_uval && !force) {
			rc::retain((gc::header*) this);
			return (function*) this;
		}

		shared::recursive_guard guard(L, const_cast<function*>(this));
		function*               result = L->duplicate(this);
		result->execution_flags        = execution_flags;
		rc::retain(result->proto);
		for (any value : result->uvals()) {
			rc::retain(value);
		}
		return result;
	}

	void gc::destroy(vm* L, function_proto* o) {
		rc::release(L, o->src_chunk);
		rc::release(L, o->jit_rejection);
		rc::release(L, o->attributes);
		o->signature.reset();
		o->generic.reset();
		rc::release(L, o->load_jfunc());
		for (any value : o->kvals()) {
			rc::release(L, value);
		}
	}

	void gc::destroy(vm* L, jfunction* o) {
#if LI_VTUNE
		iJIT_Method_Load mload;
		memset(&mload, 0, sizeof(iJIT_Method_Load));
		mload.method_id = o->uid;
		iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_UNLOAD_START, &mload);
#endif
		for (any value : o->references()) {
			rc::release(L, value);
		}
		o->~jfunction();
	}

	void gc::destroy(vm* L, function* o) {
		rc::release(L, o->proto);
		for (any value : o->uvals()) {
			rc::release(L, value);
		}
	}
};