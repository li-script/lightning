#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>
#include <vm/typed_array.hpp>

namespace li::shared {
	namespace {
		struct process_heap {
			std::recursive_mutex mutex;
			vm*                  owner        = nullptr;
			string*              empty_string = nullptr;
		};

		process_heap& heap() {
			static process_heap value;
			return value;
		}

		std::atomic_ref<uint64_t> object_lockword(gc::header* value) {
			LI_ASSERT(value && value->shared && value->total_bytes() >= sizeof(gc::header) + sizeof(uint64_t));
			auto* address = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(value) + value->total_bytes() - sizeof(uint64_t));
			return std::atomic_ref<uint64_t>(*address);
		}

		struct pending_value {
			vm*         caller;
			gc::header* value;
		};
		thread_local std::vector<pending_value> pending_destruction;
		thread_local bool                       draining_destruction = false;

		void drain_pending_destruction() {
			if (draining_destruction)
				return;
			draining_destruction = true;
			while (true) {
				auto ready = std::find_if(
					 pending_destruction.rbegin(), pending_destruction.rend(), [](const pending_value& item) { return item.caller->active_locks == 0; });
				if (ready == pending_destruction.rend())
					break;
				pending_value item = *ready;
				pending_destruction.erase(std::next(ready).base());
				detail::run_finalizer(item.caller, item.value);
				std::lock_guard guard(heap().mutex);
				detail::destroy_object(item.caller, item.value);
				LI_ASSERT(object_lockword(item.value).load(std::memory_order_relaxed) == 0);
				allocator_vm()->gc.free_storage(item.value);
			}
			draining_destruction = false;
		}

		uint32_t current_thread_id() {
			static std::atomic<uint32_t> next{1};
			thread_local uint32_t        id = [] {
				uint32_t value = next.fetch_add(1, std::memory_order_relaxed);
				if (!value)
					util::abort("shared lock thread id space exhausted");
				return value;
			}();
			return id;
		}

		[[noreturn]] void fail(const char* message) { util::abort("li shared heap: %s", message); }

		bool field_holds_reference(type value) { return value == type::any || is_gc_data(value); }

		struct clone_context {
			vm*                                                caller;
			std::unordered_map<const gc::header*, gc::header*> memo;
			bool                                               failed = false;

			void reject(const char* message) {
				if (!failed)
					caller->error("%s", message);
				failed = true;
			}
		};

		struct validation_context {
			vm*                                            caller;
			std::unordered_map<const gc::header*, uint8_t> state;
			bool                                           failed = false;

			bool reject(const char* message) {
				if (!failed)
					caller->error("%s", message);
				failed = true;
				return false;
			}
		};

		bool validate_edge(validation_context& context, any_t value);
		bool validate_class(validation_context& context, vclass* source);
		bool validate_function(validation_context& context, function* source);
		bool validate_table(validation_context& context, table* source);
		bool validate_literal_array(validation_context& context, array* source);

		bool begin_validation(validation_context& context, const gc::header* value) {
			auto [it, inserted] = context.state.emplace(value, 1);
			return inserted;
		}

		void end_validation(validation_context& context, const gc::header* value) { context.state[value] = 2; }

		bool validate_traits(validation_context& context, const trait_set* traits) {
			if (!traits)
				return true;
			for (function* method : traits->methods) {
				if (method && !validate_function(context, method))
					return false;
			}
			return true;
		}

		bool validate_proto(validation_context& context, function_proto* source) {
			if (!source || source->shared)
				return true;
			if (!begin_validation(context, source))
				return true;
			if (source->src_chunk && !validate_edge(context, any(source->src_chunk)))
				return false;
			if (source->attributes && !validate_edge(context, any(source->attributes)))
				return false;
			for (any value : source->kvals()) {
				if (!validate_edge(context, value))
					return false;
			}
			end_validation(context, source);
			return true;
		}

		bool validate_function(validation_context& context, function* source) {
			if (!source || source->is_static || source->shared)
				return true;
			if (!begin_validation(context, source))
				return true;
			if (source->is_native())
				return context.reject("cannot share a native function without a sharing hook");
			if (!validate_proto(context, source->proto))
				return false;
			for (any value : source->uvals()) {
				if (!validate_edge(context, value))
					return false;
			}
			end_validation(context, source);
			return true;
		}

		bool validate_class(validation_context& context, vclass* source) {
			if (!source || source->is_static || source->shared)
				return true;
			if (!begin_validation(context, source))
				return true;
			if (source->cxx_tid != util::type_id_v<void>)
				return context.reject("cannot share native userdata metadata without a sharing hook");
			if (source->name && !validate_edge(context, any(source->name)))
				return false;
			if (source->attributes && !validate_edge(context, any(source->attributes)))
				return false;
			if (!validate_class(context, source->super) || !validate_function(context, source->ctor) || !validate_function(context, source->initializer) ||
				 !validate_function(context, source->constructor_body) || !validate_traits(context, source->traits))
				return false;
			for (const property_definition& property : source->properties) {
				if (!property.name || !property.method)
					return context.reject("cannot share invalid class property metadata");
				if (!validate_edge(context, any(property.name)) || !validate_function(context, property.method))
					return false;
			}
			for (const field_pair& field : source->fields()) {
				if (field.key && !validate_edge(context, any(field.key)))
					return false;
				if (field.value.ty < type::obj && !field.value.class_identity)
					return context.reject("cannot share class metadata with an unresolved typed field identity");
				if (field_holds_reference(field.value.ty)) {
					const uint8_t* bytes = field.value.is_static ? source->static_space() : source->default_space();
					if (!validate_edge(context, any::load_from(bytes + field.value.offset, field.value.ty)))
						return false;
				}
			}
			end_validation(context, source);
			return true;
		}

		bool validate_literal_array(validation_context& context, array* source) {
			if (!source || source->shared)
				return true;
			if (!begin_validation(context, source))
				return true;
			for (msize_t index = 0; index != source->length; ++index) {
				any value = source->begin()[index];
				if (value.is_gc() && !value.is_str())
					return context.reject("attribute arguments must be literals");
				if (value.is_str() && !validate_edge(context, value))
					return false;
			}
			end_validation(context, source);
			return true;
		}

		bool validate_table(validation_context& context, table* source) {
			if (!source || source->shared)
				return true;
			if (!source->is_frozen)
				return context.reject("shared values cannot contain private mutable child values");
			if (!begin_validation(context, source))
				return true;
			if (!validate_traits(context, source->traits))
				return false;
			for (const table_entry& entry : *source) {
				if (entry.key != nil && (!validate_edge(context, entry.key) || !validate_edge(context, entry.value)))
					return false;
			}
			end_validation(context, source);
			return true;
		}

		bool validate_edge(validation_context& context, any_t value) {
			if (!value.is_gc() || !value.as_gc() || value.as_gc()->is_static || value.as_gc()->shared)
				return true;
			if (value.is_str())
				return true;
			if (value.is_vcl())
				return validate_class(context, value.as_vcl());
			if (value.is_fn())
				return validate_function(context, value.as_fn());
			if (value.is_tbl())
				return validate_table(context, value.as_tbl());
			if (value.is_arr())
				return validate_literal_array(context, value.as_arr());
			if (value.is_obj() && value.as_obj()->cl && value.as_obj()->cl->value_semantics) {
				object* source = value.as_obj();
				if (!begin_validation(context, source))
					return true;
				if (!validate_class(context, source->cl))
					return false;
				for (const field_pair& field : source->cl->fields()) {
					if (!field.value.is_static && field_holds_reference(field.value.ty) &&
						 !validate_edge(context, any::load_from(source->data + field.value.offset, field.value.ty)))
						return false;
				}
				end_validation(context, source);
				return true;
			}
			return context.reject("shared values cannot contain private mutable child values");
		}

		bool validate_root(validation_context& context, any_t value) {
			if (!value.is_gc() || !value.as_gc())
				return context.reject("shared.make requires a heap value");
			gc::header* header = value.as_gc();
			if (header->is_static || header->shared || value.is_str())
				return true;
			if (value.is_vcl())
				return validate_class(context, value.as_vcl());
			if (value.is_fn())
				return validate_function(context, value.as_fn());
			if (value.is_tbl()) {
				table* source = value.as_tbl();
				if (!validate_traits(context, source->traits))
					return false;
				for (const table_entry& entry : *source) {
					if (entry.key != nil && (!validate_edge(context, entry.key) || !validate_edge(context, entry.value)))
						return false;
				}
				return true;
			}
			if (value.is_arr()) {
				array* source = value.as_arr();
				for (msize_t i = 0; i != source->length; ++i) {
					if (!validate_edge(context, source->begin()[i]))
						return false;
				}
				return true;
			}
			if (value.is_tarr()) {
				typed_array* source = value.as_tarr();
				if (!source->is_struct_array())
					return true;
				if (!validate_class(context, source->element_class))
					return false;
				for (msize_t index = 0; index != source->length; ++index) {
					const std::byte* slot = source->element_data(index);
					for (const field_pair& field : source->element_class->fields()) {
						if (!field.value.is_static && field_holds_reference(field.value.ty) &&
							 !validate_edge(context, any::load_from(slot + field.value.offset, field.value.ty)))
							return false;
					}
				}
				return true;
			}
			if (value.is_obj()) {
				object* source = value.as_obj();
				if (source->gc_hook || source->data != source->context)
					return context.reject("cannot share native userdata without a sharing hook");
				if (!validate_class(context, source->cl))
					return false;
				for (const field_pair& field : source->cl->fields()) {
					if (!field.value.is_static && field_holds_reference(field.value.ty) &&
						 !validate_edge(context, any::load_from(source->data + field.value.offset, field.value.ty)))
						return false;
				}
				return true;
			}
			return context.reject("this heap value type cannot be shared");
		}

		any          clone_edge(clone_context& context, any_t value);
		vclass*      clone_class(clone_context& context, vclass* source);
		function*    clone_function(clone_context& context, function* source);
		table*       clone_table_root(clone_context& context, table* source);
		object*      clone_object_root(clone_context& context, object* source);
		array*       clone_array_root(clone_context& context, array* source);
		typed_array* clone_typed_array_root(clone_context& context, typed_array* source);

		template<typename T>
		T* memoized(clone_context& context, const gc::header* source) {
			auto it = context.memo.find(source);
			if (it == context.memo.end())
				return nullptr;
			auto* result = static_cast<T*>(it->second);
			rc::retain(result);
			return result;
		}

		trait_set* clone_traits(clone_context& context, const trait_set* source) {
			if (!source)
				return nullptr;
			trait_set* result = allocate<trait_set>(context.caller);
			result->seal      = source->seal;
			result->freeze    = source->freeze;
			result->hide      = source->hide;
			for (size_t i = 0; i != num_trait_methods; ++i) {
				function* method = source->methods[i];
				if (!method)
					continue;
				result->methods[i] = clone_function(context, method);
				if (context.failed) {
					destroy_trait_set(context.caller, result);
					return nullptr;
				}
			}
			return result;
		}

		function_proto* clone_proto(clone_context& context, function_proto* source) {
			if (!source)
				return nullptr;
			if (source->shared) {
				rc::retain(source);
				return source;
			}
			if (auto* existing = memoized<function_proto>(context, source))
				return existing;

			auto* result = allocate<function_proto>(context.caller, function_proto::payload_size(source->length, source->num_kval, source->num_lines));
			result->attr = source->attr;
			result->return_class_identity = source->return_class_identity;
			result->length                = source->length;
			result->num_locals            = source->num_locals;
			result->num_kval              = source->num_kval;
			result->num_lines             = source->num_lines;
			result->num_uval              = source->num_uval;
			result->src_line              = source->src_line;
			result->src_chunk             = nullptr;
			result->jit_rejection         = nullptr;
			result->attributes            = nullptr;
			result->signature             = nullptr;
			result->jfunc                 = nullptr;
			std::copy(source->opcodes().begin(), source->opcodes().end(), result->opcodes().begin());
			std::copy(source->lines().begin(), source->lines().end(), result->lines().begin());
			fill_nil(result->kvals().data(), result->num_kval);
			context.memo.emplace(source, result);

			if (source->signature)
				result->signature = std::make_unique<strict_signature>(*source->signature);
			if (source->attributes) {
				any attributes = clone_edge(context, any(source->attributes));
				if (context.failed) {
					context.memo.erase(source);
					rc::release(context.caller, result);
					return nullptr;
				}
				result->attributes = attributes.as_tbl();
			}
			if (source->src_chunk) {
				result->src_chunk = copy_string(context.caller, source->src_chunk);
			}
			if (source->jit_rejection) {
				result->jit_rejection = copy_string(context.caller, source->jit_rejection);
			}
			for (msize_t i = 0; i != result->num_kval; ++i) {
				any value = clone_edge(context, source->kvals()[i]);
				if (context.failed) {
					context.memo.erase(source);
					rc::release(context.caller, result);
					return nullptr;
				}
				result->kvals()[i] = value;
			}

			// KIMM, PUSHI, and VACHK embed boxed values directly in the instruction.
			// Their constant-pool entries own those values but the bytecode does not
			// address the pool, so copied instructions must be relocated to the cloned
			// constants before the source VM can release them.
			for (msize_t pc = 0; pc != result->length; ++pc) {
				bc::insn& instruction = result->opcodes()[pc];
				if (instruction.o != bc::KIMM && instruction.o != bc::PUSHI && instruction.o != bc::VACHK)
					continue;

				any immediate(std::in_place, source->opcodes()[pc].xmm());
				if (!immediate.is_gc())
					continue;

				msize_t index = 0;
				while (index != source->num_kval && source->kvals()[index].value != immediate.value)
					++index;
				if (index == source->num_kval) {
					context.reject("cannot share bytecode with an untracked heap immediate");
					context.memo.erase(source);
					rc::release(context.caller, result);
					return nullptr;
				}
				instruction.set_xmm(result->kvals()[index].value);
			}

			// Native code can contain immediate GC pointer bits. Without relocation
			// records, copying its bytes would preserve private-heap addresses. The
			// shared immutable prototype is therefore published interpreted and may be
			// compiled again through the owner-aware jfunction factory.
			return result;
		}

		function* clone_function(clone_context& context, function* source) {
			if (!source)
				return nullptr;
			if (source->is_static || source->shared) {
				rc::retain(source);
				return source;
			}
			if (auto* existing = memoized<function>(context, source))
				return existing;
			if (source->is_native()) {
				context.reject("cannot share a native function without a sharing hook");
				return nullptr;
			}

			function* result = allocate<function>(context.caller, sizeof(any) * source->num_uval);
			result->invoke   = &vm_invoke;
			result->num_uval = source->num_uval;
			result->proto    = nullptr;
			result->ninfo    = nullptr;
			fill_nil(result->uvals().data(), result->num_uval);
			context.memo.emplace(source, result);

			result->proto = clone_proto(context, source->proto);
			if (context.failed) {
				context.memo.erase(source);
				rc::release(context.caller, result);
				return nullptr;
			}
			for (msize_t i = 0; i != result->num_uval; ++i) {
				any value = clone_edge(context, source->uvals()[i]);
				if (context.failed) {
					context.memo.erase(source);
					rc::release(context.caller, result);
					return nullptr;
				}
				result->uvals()[i] = value;
			}
			return result;
		}

		vclass* clone_class(clone_context& context, vclass* source) {
			if (!source)
				return nullptr;
			if (source->is_static || source->shared) {
				rc::retain(source);
				return source;
			}
			if (auto* existing = memoized<vclass>(context, source))
				return existing;
			if (source->cxx_tid != util::type_id_v<void>) {
				context.reject("cannot share native userdata metadata without a sharing hook");
				return nullptr;
			}

			std::vector<field_pair> fields(source->fields().begin(), source->fields().end());
			std::vector<uint8_t>    defaults(source->default_space(), source->default_space() + source->object_length);
			std::vector<uint8_t>    statics(source->static_space(), source->static_space() + source->static_length);
			std::vector<string*>    keys(fields.size(), nullptr);
			for (size_t i = 0; i != fields.size(); ++i) {
				field_pair& field = fields[i];
				keys[i]           = copy_string(context.caller, field.key);
				field.key         = keys[i];
				if (field.value.ty < type::obj)
					field.value.ty = type::obj;
				if (field_holds_reference(source->fields()[i].value.ty)) {
					uint8_t* bytes = source->fields()[i].value.is_static ? statics.data() : defaults.data();
					if (source->fields()[i].value.ty == type::any)
						any(nil).store_at(bytes + source->fields()[i].value.offset, type::any);
					else
						std::memset(bytes + source->fields()[i].value.offset, 0, sizeof(gc::header*));
				}
			}

			string* name  = copy_string(context.caller, source->name);
			vclass* super = clone_class(context, source->super);
			if (context.failed) {
				for (string* key : keys)
					rc::release(context.caller, key);
				rc::release(context.caller, name);
				rc::release(context.caller, super);
				return nullptr;
			}

			vm*     owner = allocator_vm();
			vclass* result;
			{
				std::lock_guard guard(heap().mutex);
				result = vclass::create(owner, name, fields, defaults, statics, super, source->identity, source->value_semantics);
			}
			context.memo.emplace(source, result);
			result->dynamic_traits = source->dynamic_traits;
			if (source->attributes) {
				any attributes = clone_edge(context, any(source->attributes));
				if (!context.failed)
					result->attributes = attributes.as_tbl();
			}
			for (string* key : keys)
				rc::release(context.caller, key);
			rc::release(context.caller, name);
			rc::release(context.caller, super);

			for (size_t i = 0; i != fields.size(); ++i) {
				const field_pair& source_field = source->fields()[i];
				field_pair&       result_field = result->fields()[i];
				if (source_field.value.ty < type::obj) {
					// Negative type ids are VM-local storage tags. Cross-VM runtime
					// checks use the process-stable class_identity copied with the
					// descriptor, so no source registry pointer or strong class edge
					// survives publication.
					result_field.value.ty = source_field.value.ty;
				}
				if (!field_holds_reference(source_field.value.ty))
					continue;
				const uint8_t* source_bytes = source_field.value.is_static ? source->static_space() : source->default_space();
				uint8_t*       result_bytes = result_field.value.is_static ? result->static_space() : result->default_space();
				any            value        = clone_edge(context, any::load_from(source_bytes + source_field.value.offset, source_field.value.ty));
				if (context.failed)
					break;
				if (result_field.value.ty == type::any)
					value.store_at(result_bytes + result_field.value.offset, type::any);
				else {
					gc::header* pointer = value == nil ? nullptr : value.as_gc();
					std::memcpy(result_bytes + result_field.value.offset, &pointer, sizeof(pointer));
				}
			}
			if (!context.failed) {
				function* ctor = clone_function(context, source->ctor);
				if (!context.failed) {
					result->set_ctor(owner, ctor);
					rc::release(context.caller, ctor);
				}
				function* initializer = clone_function(context, source->initializer);
				if (!context.failed) {
					result->set_initializer(owner, initializer);
					rc::release(context.caller, initializer);
				}
				function* constructor_body = clone_function(context, source->constructor_body);
				if (!context.failed) {
					result->set_constructor_body(owner, constructor_body);
					rc::release(context.caller, constructor_body);
				}
			}
			if (!context.failed) {
				for (const property_definition& source_property : source->properties) {
					string*   property_name   = copy_string(context.caller, source_property.name);
					function* property_method = clone_function(context, source_property.method);
					if (context.failed) {
						rc::release(context.caller, property_name);
						rc::release(context.caller, property_method);
						break;
					}
					const property_definition property{
						 .name    = property_name,
						 .method  = property_method,
						 .access  = source_property.access,
						 .dynamic = source_property.dynamic,
					};
					if (!result->define_property(context.caller, property))
						context.reject("failed to clone class property metadata");
					rc::release(context.caller, property_name);
					rc::release(context.caller, property_method);
					if (context.failed)
						break;
				}
			}
			if (!context.failed)
				result->traits = clone_traits(context, source->traits);
			if (context.failed) {
				context.memo.erase(source);
				rc::release(context.caller, result);
				return nullptr;
			}
			return result;
		}

		any clone_edge(clone_context& context, any_t value) {
			if (!value.is_gc())
				return any(value);
			gc::header* header = value.as_gc();
			if (!header)
				return any(value);
			if (header->is_static || header->shared) {
				rc::retain(header);
				return any(value);
			}
			if (value.is_str())
				return any(copy_string(context.caller, value.as_str()));
			if (value.is_vcl())
				return any(clone_class(context, value.as_vcl()));
			if (value.is_fn())
				return any(clone_function(context, value.as_fn()));
			if (value.is_tbl() && value.as_tbl()->is_frozen)
				return any(clone_table_root(context, value.as_tbl()));
			if (value.is_arr())
				return any(clone_array_root(context, value.as_arr()));
			if (value.is_obj() && value.as_obj()->cl && value.as_obj()->cl->value_semantics)
				return any(clone_object_root(context, value.as_obj()));
			context.reject("shared values cannot contain private mutable child values");
			return nil;
		}

		table* clone_table_root(clone_context& context, table* source) {
			if (auto* existing = memoized<table>(context, source))
				return existing;
			vm*    owner = allocator_vm();
			table* result;
			{
				std::lock_guard guard(heap().mutex);
				result = table::create(owner, source->active_count);
			}
			context.memo.emplace(source, result);
			result->is_frozen = source->is_frozen;
			result->traits    = clone_traits(context, source->traits);
			if (context.failed) {
				rc::release(context.caller, result);
				return nullptr;
			}
			for (const table_entry& entry : *source) {
				if (entry.key == nil)
					continue;
				any key   = clone_edge(context, entry.key);
				any value = context.failed ? any(nil) : clone_edge(context, entry.value);
				if (context.failed) {
					rc::release(context.caller, key);
					rc::release(context.caller, value);
					rc::release(context.caller, result);
					return nullptr;
				}
				bool stored = result->set(context.caller, key, value);
				rc::release(context.caller, key);
				rc::release(context.caller, value);
				if (!stored) {
					context.failed = true;
					rc::release(context.caller, result);
					return nullptr;
				}
			}
			result->mutation_version = 0;
			return result;
		}

		array* clone_array_root(clone_context& context, array* source) {
			if (auto* existing = memoized<array>(context, source))
				return existing;
			array* result;
			{
				std::lock_guard guard(heap().mutex);
				result = array::create(allocator_vm(), source->length);
			}
			context.memo.emplace(source, result);
			for (msize_t i = 0; i != source->length; ++i) {
				any value = clone_edge(context, source->begin()[i]);
				if (context.failed) {
					rc::release(context.caller, result);
					return nullptr;
				}
				result->begin()[i] = value;
			}
			return result;
		}

		typed_array* clone_typed_array_root(clone_context& context, typed_array* source) {
			if (auto* existing = memoized<typed_array>(context, source))
				return existing;
			vclass* element_class = source->is_struct_array() ? clone_class(context, source->element_class) : nullptr;
			if (context.failed)
				return nullptr;
			const size_t width = source->element_size();
			if (width && size_t(source->capacity) > std::numeric_limits<size_t>::max() / width) {
				context.reject("typed array allocation size overflow");
				rc::release(context.caller, element_class);
				return nullptr;
			}
			const size_t capacity_bytes = size_t(source->capacity) * width;
			const size_t live_bytes     = size_t(source->length) * width;

			typed_array* result      = allocate<typed_array>(context.caller);
			result->length           = source->length;
			result->capacity         = source->capacity;
			result->mutation_version = 0;
			result->element_kind     = source->element_kind;
			result->element_class    = element_class;
			result->element_stride   = source->element_stride;
			if (capacity_bytes) {
				result->storage = allocate<typed_array_store>(context.caller, capacity_bytes);
				std::memset(result->data(), 0, capacity_bytes);
				if (live_bytes)
					std::memcpy(result->data(), source->data(), live_bytes);
			}
			context.memo.emplace(source, result);
			if (!source->is_struct_array())
				return result;

			for (msize_t index = 0; index != source->length; ++index) {
				std::byte* slot = result->element_data(index);
				for (const field_pair& field : source->element_class->fields()) {
					if (field.value.is_static || !field_holds_reference(field.value.ty))
						continue;
					if (field.value.ty == type::any)
						any(nil).store_at(slot + field.value.offset, type::any);
					else
						std::memset(slot + field.value.offset, 0, sizeof(gc::header*));
				}
			}
			for (msize_t index = 0; index != source->length; ++index) {
				const std::byte* source_slot = source->element_data(index);
				std::byte*       result_slot = result->element_data(index);
				for (const field_pair& field : source->element_class->fields()) {
					if (field.value.is_static || !field_holds_reference(field.value.ty))
						continue;
					any value = clone_edge(context, any::load_from(source_slot + field.value.offset, field.value.ty));
					if (context.failed) {
						context.memo.erase(source);
						rc::release(context.caller, result);
						return nullptr;
					}
					if (field.value.ty == type::any)
						value.store_at(result_slot + field.value.offset, type::any);
					else {
						gc::header* pointer = value == nil ? nullptr : value.as_gc();
						std::memcpy(result_slot + field.value.offset, &pointer, sizeof(pointer));
					}
				}
			}
			return result;
		}

		object* clone_object_root(clone_context& context, object* source) {
			if (auto* existing = memoized<object>(context, source))
				return existing;
			if (source->gc_hook || source->data != source->context) {
				context.reject("cannot share native userdata without a sharing hook");
				return nullptr;
			}
			vclass* cl = clone_class(context, source->cl);
			if (context.failed)
				return nullptr;
			object* result      = allocate<object>(context.caller, cl->object_length);
			result->cl          = cl;
			result->data        = result->context;
			result->gc_hook     = nullptr;
			result->finalizable = source->finalizable;
			result->type_id     = cl->vm_tid;
			std::memcpy(result->data, source->data, cl->object_length);
			context.memo.emplace(source, result);

			for (const field_pair& field : source->cl->fields()) {
				if (field.value.is_static || !field_holds_reference(field.value.ty))
					continue;
				if (field.value.ty == type::any)
					any(nil).store_at(result->data + field.value.offset, type::any);
				else
					std::memset(result->data + field.value.offset, 0, sizeof(gc::header*));
			}
			for (const field_pair& field : source->cl->fields()) {
				if (field.value.is_static || !field_holds_reference(field.value.ty))
					continue;
				any value = clone_edge(context, any::load_from(source->data + field.value.offset, field.value.ty));
				if (context.failed) {
					context.memo.erase(source);
					rc::release(context.caller, result);
					return nullptr;
				}
				if (field.value.ty == type::any)
					value.store_at(result->data + field.value.offset, type::any);
				else {
					gc::header* pointer = value == nil ? nullptr : value.as_gc();
					std::memcpy(result->data + field.value.offset, &pointer, sizeof(pointer));
				}
			}
			return result;
		}

		struct numeric_slot {
			void* address = nullptr;
			type  storage = type::any;
		};

		bool table_key_equals(any_t lhs, any_t rhs) {
			if (lhs.value == rhs.value)
				return true;
			if (lhs.is_str() && rhs.is_str())
				return string_value_equals(lhs.as_str(), rhs.as_str());
			return canonicalize_zero_bits(lhs.value) == canonicalize_zero_bits(rhs.value);
		}

		const char* mutation_version_available(gc::header* target) {
			switch (gc::identify_value_type(target)) {
				case type_array:
					return static_cast<array*>(target)->mutation_version == std::numeric_limits<uint64_t>::max() ? "array mutation version overflow" : nullptr;
				case type_table:
					return static_cast<table*>(target)->mutation_version == std::numeric_limits<uint64_t>::max() ? "table mutation version overflow" : nullptr;
				default:
					return nullptr;
			}
		}

		void record_numeric_mutation(gc::header* target) {
			switch (gc::identify_value_type(target)) {
				case type_array:
					++static_cast<array*>(target)->mutation_version;
					break;
				case type_table:
					++static_cast<table*>(target)->mutation_version;
					break;
				default:
					break;
			}
		}

		const char* resolve_numeric_slot(gc::header* target, any_t key, numeric_slot& result) {
			if (!target || !target->shared)
				return "atomic updates require a shared target";
			switch (gc::identify_value_type(target)) {
				case type_array: {
					auto* value = static_cast<array*>(target);
					if (!key.is_num() || !std::isfinite(key.as_num()) || key.as_num() != std::trunc(key.as_num()) || key.as_num() < 0 ||
						 key.as_num() >= value->length)
						return "atomic array index is out of bounds";
					result = {&value->begin()[msize_t(key.as_num())], type::any};
					return nullptr;
				}
				case type_table: {
					auto* value = static_cast<table*>(target);
					if (key == nil)
						return "atomic table key cannot be nil";
					for (table_entry& entry : value->find(key.hash())) {
						if (table_key_equals(entry.key, key)) {
							result = {&entry.value, type::any};
							return nullptr;
						}
					}
					return "atomic table key does not exist";
				}
				case type_object: {
					auto* value = static_cast<object*>(target);
					if (!key.is_str())
						return "atomic object field key must be a string";
					for (const field_pair& field : value->cl->fields()) {
						if (!any(field.key).equals(key))
							continue;
						if (field.value.is_static)
							return "atomic updates of static class fields require the class target";
						if (field.value.ty != type::any && !is_integer_data(field.value.ty) && !is_floating_point_data(field.value.ty))
							return "atomic object field is not numeric";
						result = {value->data + field.value.offset, field.value.ty};
						return nullptr;
					}
					return "atomic object field does not exist";
				}
				default:
					return "atomic updates require a shared table, array, or object";
			}
		}

		bool numeric_result_fits(type storage, number value) {
			switch (storage) {
				case type::i8:
					return std::isfinite(value) && value == std::trunc(value) && value >= std::numeric_limits<int8_t>::lowest() &&
							 value <= std::numeric_limits<int8_t>::max();
				case type::i16:
					return std::isfinite(value) && value == std::trunc(value) && value >= std::numeric_limits<int16_t>::lowest() &&
							 value <= std::numeric_limits<int16_t>::max();
				case type::i32:
					return std::isfinite(value) && value == std::trunc(value) && value >= std::numeric_limits<int32_t>::lowest() &&
							 value <= std::numeric_limits<int32_t>::max();
				case type::i64:
					return std::isfinite(value) && value == std::trunc(value) && value >= -0x1p63 && value < 0x1p63;
				default:
					return true;
			}
		}

		number apply_numeric(numeric_operation operation, number lhs, number rhs) {
			switch (operation) {
				case numeric_operation::set:
					return rhs;
				case numeric_operation::add:
					return lhs + rhs;
				case numeric_operation::sub:
					return lhs - rhs;
				case numeric_operation::mul:
					return lhs * rhs;
				case numeric_operation::div:
					return lhs / rhs;
				case numeric_operation::mod:
					return std::fmod(lhs, rhs);
			}
			assume_unreachable();
		}

		const char* atomic_update_one(const numeric_update& update, number& result) {
			if (static_cast<uint8_t>(update.operation) > static_cast<uint8_t>(numeric_operation::mod))
				return "atomic update contains an invalid numeric operation";
			numeric_slot slot;
			if (const char* error = resolve_numeric_slot(update.target, update.key, slot))
				return error;
			if (const char* error = mutation_version_available(update.target))
				return error;

			if (slot.storage == type::any) {
				auto*                     bits = static_cast<uint64_t*>(slot.address);
				std::atomic_ref<uint64_t> cell(*bits);
				uint64_t                  current = cell.load(std::memory_order_relaxed);
				while (true) {
					any previous{std::in_place, current};
					if (!previous.is_num())
						return "atomic field is not numeric";
					any next(apply_numeric(update.operation, previous.as_num(), update.operand));
					if (cell.compare_exchange_weak(current, next.value, std::memory_order_acq_rel, std::memory_order_relaxed)) {
						result = next.as_num();
						record_numeric_mutation(update.target);
						return nullptr;
					}
				}
			}

			if (slot.storage == type::f64) {
				auto*                   storage = static_cast<number*>(slot.address);
				std::atomic_ref<number> cell(*storage);
				number                  current = cell.load(std::memory_order_relaxed);
				while (true) {
					number next = apply_numeric(update.operation, current, update.operand);
					if (cell.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
						result = next;
						record_numeric_mutation(update.target);
						return nullptr;
					}
				}
			}

			if (slot.storage == type::f32) {
				auto*                  storage = static_cast<float*>(slot.address);
				std::atomic_ref<float> cell(*storage);
				float                  current = cell.load(std::memory_order_relaxed);
				while (true) {
					float next = float(apply_numeric(update.operation, number(current), update.operand));
					if (cell.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
						result = number(next);
						record_numeric_mutation(update.target);
						return nullptr;
					}
				}
			}

			auto update_integer = [&]<typename T>() -> const char* {
				auto*              storage = static_cast<T*>(slot.address);
				std::atomic_ref<T> cell(*storage);
				T                  current = cell.load(std::memory_order_relaxed);
				while (true) {
					number next_number = apply_numeric(update.operation, number(current), update.operand);
					if (!numeric_result_fits(slot.storage, next_number))
						return "atomic result does not fit the typed field";
					T next = T(next_number);
					if (cell.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
						result = number(next);
						record_numeric_mutation(update.target);
						return nullptr;
					}
				}
			};
			switch (slot.storage) {
				case type::i8:
					return update_integer.template operator()<int8_t>();
				case type::i16:
					return update_integer.template operator()<int16_t>();
				case type::i32:
					return update_integer.template operator()<int32_t>();
				case type::i64:
					return update_integer.template operator()<int64_t>();
				default:
					assume_unreachable();
			}
		}

		const field_info* find_atomic_field(const object* target, const string* key) {
			if (!target || !target->cl || !key)
				return nullptr;
			for (const field_pair& field : target->cl->fields()) {
				if (!field.value.is_static && field.value.is_atomic && string_value_equals(field.key, key))
					return &field.value;
			}
			return nullptr;
		}
	}

	std::recursive_mutex& heap_mutex() { return heap().mutex; }

	vm* allocator_vm() {
		std::lock_guard guard(heap().mutex);
		if (!heap().owner) {
			heap().owner = vm::create();
			if (!heap().owner)
				fail("failed to initialize process-wide allocator");
			heap().owner->gc.shared_heap = true;
			strset_reset_shared(heap().owner);

			string* private_empty      = heap().owner->empty_string;
			string* shared_empty       = heap().owner->alloc<string>(1);
			shared_empty->hash         = 0;
			shared_empty->length       = 0;
			shared_empty->data[0]      = 0;
			heap().owner->empty_string = shared_empty;
			heap().empty_string        = shared_empty;
			rc::release(heap().owner, private_empty);
			// Shared destruction uses its own queue, not the private bootstrap worklist.
			LI_ASSERT(heap().owner->gc.pending.empty());
			std::vector<gc::header*>{}.swap(heap().owner->gc.pending);
		}
		return heap().owner;
	}

	bool is_shared(const gc::header* value) { return value && value->shared; }
	bool is_shared(any_t value) { return value.is_gc() && is_shared(value.as_gc()); }

	string* copy_string(vm* caller, const string* value) {
		if (!value)
			return nullptr;
		if (value->shared) {
			rc::retain(const_cast<string*>(value));
			return const_cast<string*>(value);
		}
		std::lock_guard guard(heap().mutex);
		vm*             owner = allocator_vm();
		if (value->length != 0)
			return string::create(owner, value->view());
		// allocator_vm owns the initial process-lifetime reference; each caller
		// receives the additional retain below.
		LI_ASSERT(heap().empty_string == owner->empty_string);
		rc::retain(heap().empty_string);
		return heap().empty_string;
	}

	void retain(gc::header* value) {
		if (!value || value->is_static)
			return;
		if (!value->shared)
			fail("shared retain requires a shared value");
		std::atomic_ref<uint32_t> count(value->refcount);
		uint32_t                  current = count.load(std::memory_order_relaxed);
		while (true) {
			if (current == 0 || current == gc::destroying_refcount)
				fail("attempted to resurrect a destroyed shared value");
			if (current == gc::maximum_refcount)
				fail("shared reference count overflow");
			if (count.compare_exchange_weak(current, current + 1, std::memory_order_relaxed, std::memory_order_relaxed))
				break;
		}
		rc::counts().retains++;
	}

	bool try_retain(gc::header* value) {
		if (!value || value->is_static)
			return true;
		if (!value->shared)
			fail("shared try_retain requires a shared value");
		std::atomic_ref<uint32_t> count(value->refcount);
		uint32_t                  current = count.load(std::memory_order_acquire);
		while (current != 0 && current != gc::destroying_refcount) {
			if (current == gc::maximum_refcount)
				fail("shared reference count overflow");
			if (count.compare_exchange_weak(current, current + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
				rc::counts().retains++;
				return true;
			}
		}
		return false;
	}

	void release(vm* caller, gc::header* value) {
		if (!value || value->is_static)
			return;
		if (!value->shared)
			fail("shared release requires a shared value");
		if (!caller)
			fail("shared value released without a caller VM");

		std::atomic_ref<uint32_t> count(value->refcount);
		uint32_t                  current = count.load(std::memory_order_relaxed);
		while (true) {
			if (current == gc::destroying_refcount)
				return;
			if (current == 0)
				fail("shared reference count underflow");
			if (count.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel, std::memory_order_relaxed))
				break;
		}
		rc::counts().releases++;
		if (current != 1)
			return;

		{
			std::lock_guard guard(heap().mutex);
			count.store(gc::destroying_refcount, std::memory_order_release);
			invalidate_observers(caller, value);
		}

		pending_destruction.push_back({caller, value});
		drain_pending_destruction();
	}

	void lock(vm* caller, gc::header* value) {
		if (!value || value->is_static || !value->shared)
			return;
		auto           lockword = object_lockword(value);
		const uint32_t thread   = current_thread_id();
		uint64_t       current  = lockword.load(std::memory_order_relaxed);
		while (true) {
			const uint32_t owner = uint32_t(current >> 32);
			const uint32_t count = uint32_t(current);
			uint64_t       next;
			if (!owner) {
				next = (uint64_t(thread) << 32) | 1;
			} else if (owner == thread) {
				if (count == std::numeric_limits<uint32_t>::max())
					fail("shared recursive lock count overflow");
				next = (uint64_t(thread) << 32) | (count + 1);
			} else {
				std::this_thread::yield();
				current = lockword.load(std::memory_order_relaxed);
				continue;
			}
			if (lockword.compare_exchange_weak(current, next, std::memory_order_acquire, std::memory_order_relaxed))
				break;
		}
		if (caller) {
			if (caller->active_locks == std::numeric_limits<uint32_t>::max())
				caller->panic("shared lock count overflow");
			++caller->active_locks;
		}
	}

	void unlock(vm* caller, gc::header* value) {
		if (!value || value->is_static || !value->shared)
			return;
		auto           lockword = object_lockword(value);
		const uint32_t thread   = current_thread_id();
		uint64_t       current  = lockword.load(std::memory_order_relaxed);
		while (true) {
			const uint32_t owner = uint32_t(current >> 32);
			const uint32_t count = uint32_t(current);
			if (owner != thread || !count)
				fail("shared object unlocked by a thread that does not own it");
			uint64_t next = count == 1 ? 0 : ((uint64_t(thread) << 32) | (count - 1));
			if (lockword.compare_exchange_weak(current, next, std::memory_order_release, std::memory_order_relaxed))
				break;
		}
		if (caller) {
			if (!caller->active_locks)
				caller->panic("shared VM lock count underflow");
			--caller->active_locks;
			if (!caller->active_locks)
				drain_pending_destruction();
		}
	}

	bool held_by_current_thread(const gc::header* value) {
		if (!value || value->is_static || !value->shared)
			return false;
		auto lockword = object_lockword(const_cast<gc::header*>(value));
		return uint32_t(lockword.load(std::memory_order_relaxed) >> 32) == current_thread_id();
	}

	prepared_value prepare_store(vm* caller, gc::header* destination, any_t borrowed) {
		prepared_value result{any(borrowed), false, false};
		if (!destination || !destination->shared) {
			result.ok = rc::check_store(caller, borrowed);
			return result;
		}
		if (!borrowed.is_gc()) {
			result.ok = true;
			return result;
		}
		gc::header* value = borrowed.as_gc();
		if (!value || value->is_static) {
			result.ok = true;
			return result;
		}
		if (value->shared) {
			std::atomic_ref<uint32_t> count(value->refcount);
			uint32_t                  current = count.load(std::memory_order_acquire);
			if (current != 0 && current != gc::destroying_refcount) {
				result.ok = true;
				return result;
			}
			caller->error("cannot store a destroyed value");
			return result;
		}
		if (borrowed.is_str()) {
			result.value = any(copy_string(caller, borrowed.as_str()));
			result.owns  = true;
			result.ok    = true;
			return result;
		}
		if (borrowed.is_obj() && borrowed.as_obj()->cl && borrowed.as_obj()->cl->value_semantics) {
			result.value = make(caller, borrowed);
			result.owns  = !result.value.is_exc();
			result.ok    = result.owns;
			return result;
		}
		caller->error("cannot store a private mutable value in a shared object");
		return result;
	}

	void finish_store(vm* caller, prepared_value& value) {
		if (value.owns)
			rc::release(caller, value.value);
		value = {};
	}

	any_t make(vm* caller, any_t borrowed) {
		validation_context validation{caller};
		if (!validate_root(validation, borrowed))
			return exception_marker;
		gc::header* source = borrowed.as_gc();
		if (source->is_static || source->shared) {
			rc::retain(source);
			return borrowed;
		}

		clone_context context{caller};
		any           result = nil;
		if (borrowed.is_str())
			result = any(copy_string(caller, borrowed.as_str()));
		else if (borrowed.is_tbl())
			result = any(clone_table_root(context, borrowed.as_tbl()));
		else if (borrowed.is_arr())
			result = any(clone_array_root(context, borrowed.as_arr()));
		else if (borrowed.is_tarr())
			result = any(clone_typed_array_root(context, borrowed.as_tarr()));
		else if (borrowed.is_obj())
			result = any(clone_object_root(context, borrowed.as_obj()));
		else if (borrowed.is_vcl())
			result = any(clone_class(context, borrowed.as_vcl()));
		else if (borrowed.is_fn())
			result = any(clone_function(context, borrowed.as_fn()));
		else {
			context.reject("this heap value type cannot be shared");
		}
		return context.failed ? any(exception_marker) : result;
	}

	bool atomic_update(vm* caller, std::span<const numeric_update> updates, std::span<number> results) {
		bool had_preheld_target = false;
		for (const numeric_update& update : updates) {
			if (update.target && held_by_current_thread(update.target)) {
				had_preheld_target = true;
				break;
			}
		}
		if (results.size() < updates.size()) {
			if (!had_preheld_target)
				caller->error("atomic result span is too small");
			return false;
		}
		if (updates.size() == 1) {
			lock(caller, updates[0].target);
			const char* error = atomic_update_one(updates[0], results[0]);
			unlock(caller, updates[0].target);
			if (error) {
				if (!had_preheld_target)
					caller->error("%s", error);
				return false;
			}
			return true;
		}

		// Acquire each distinct target in address order without allocating. This is
		// intentionally O(n^2): atomic blocks are small, and allocation is forbidden
		// even when the parser executor already holds these locks recursively.
		std::less<gc::header*> less;
		gc::header*            previous      = nullptr;
		bool                   have_previous = false;
		while (true) {
			gc::header* next = nullptr;
			for (const numeric_update& update : updates) {
				gc::header* candidate = update.target;
				if (!candidate || (have_previous && !less(previous, candidate)))
					continue;
				if (!next || less(candidate, next))
					next = candidate;
			}
			if (!next)
				break;
			lock(caller, next);
			previous      = next;
			have_previous = true;
		}

		auto unlock_all = [&] {
			gc::header* upper      = nullptr;
			bool        have_upper = false;
			while (true) {
				gc::header* next = nullptr;
				for (const numeric_update& update : updates) {
					gc::header* candidate = update.target;
					if (!candidate || (have_upper && !less(candidate, upper)))
						continue;
					if (!next || less(next, candidate))
						next = candidate;
				}
				if (!next)
					break;
				unlock(caller, next);
				upper      = next;
				have_upper = true;
			}
		};

		const char* error = nullptr;
		previous          = nullptr;
		have_previous     = false;
		while (true) {
			gc::header* next = nullptr;
			for (const numeric_update& update : updates) {
				gc::header* candidate = update.target;
				if (!candidate || (have_previous && !less(previous, candidate)))
					continue;
				if (!next || less(candidate, next))
					next = candidate;
			}
			if (!next)
				break;
			if (!error)
				error = mutation_version_available(next);
			previous      = next;
			have_previous = true;
		}

		for (size_t i = 0; i != updates.size() && !error; ++i) {
			const numeric_update& update = updates[i];
			if (static_cast<uint8_t>(update.operation) > static_cast<uint8_t>(numeric_operation::mod)) {
				error = "atomic update contains an invalid numeric operation";
				break;
			}
			numeric_slot slot;
			error = resolve_numeric_slot(update.target, update.key, slot);
			if (error)
				break;

			number current           = 0;
			bool   previously_staged = false;
			for (size_t prior = i; prior != 0; --prior) {
				numeric_slot          prior_slot;
				const numeric_update& prior_update = updates[prior - 1];
				const char*           prior_error  = resolve_numeric_slot(prior_update.target, prior_update.key, prior_slot);
				LI_ASSERT(prior_error == nullptr);
				if (prior_slot.address == slot.address && prior_slot.storage == slot.storage) {
					current           = results[prior - 1];
					previously_staged = true;
					break;
				}
			}
			if (!previously_staged) {
				any value = any::load_from(slot.address, slot.storage);
				if (!value.is_num()) {
					error = "atomic field is not numeric";
					break;
				}
				current = value.as_num();
			}
			results[i] = apply_numeric(update.operation, current, update.operand);
			if (!numeric_result_fits(slot.storage, results[i]))
				error = "atomic result does not fit the typed field";
		}

		if (!error) {
			for (size_t i = 0; i != updates.size(); ++i) {
				numeric_slot slot;
				const char*  resolve_error = resolve_numeric_slot(updates[i].target, updates[i].key, slot);
				LI_ASSERT(resolve_error == nullptr);
				any(results[i]).store_at(slot.address, slot.storage);
			}

			previous      = nullptr;
			have_previous = false;
			while (true) {
				gc::header* next = nullptr;
				for (const numeric_update& update : updates) {
					gc::header* candidate = update.target;
					if (!candidate || (have_previous && !less(previous, candidate)))
						continue;
					if (!next || less(candidate, next))
						next = candidate;
				}
				if (!next)
					break;
				record_numeric_mutation(next);
				previous      = next;
				have_previous = true;
			}
		}

		unlock_all();
		if (error) {
			// A parser executor may enter with recursive target locks already held.
			// It reports the guard error after releasing those outer locks so error
			// string allocation never occurs inside the restricted transaction.
			if (!had_preheld_target)
				caller->error("%s", error);
			return false;
		}
		return true;
	}

	any_t atomic_add(vm* caller, gc::header* target, any_t key, number delta) {
		if (!target || !target->shared)
			return caller->error("atomic.add requires a shared target");
		numeric_update update{
			 .target    = target,
			 .key       = any(key),
			 .operation = numeric_operation::add,
			 .operand   = delta,
		};
		number result = 0;
		lock(caller, target);
		const char* error = atomic_update_one(update, result);
		unlock(caller, target);
		if (error)
			return caller->error("%s", error);
		return any(result);
	}

	bool is_atomic_field(const object* target, const string* key) { return find_atomic_field(target, key) != nullptr; }

	any_t atomic_field_store(vm* caller, any_t target, any_t key, any_t value) {
		if (!target.is_obj() || !key.is_str() || !find_atomic_field(target.as_obj(), key.as_str()))
			return caller->error("atomic field store requires an atomic object field");
		if (!value.is_num())
			return caller->error("atomic field store expects a numeric value");

		object* instance = target.as_obj();
		if (!instance->shared) {
			if (!instance->set(caller, key.as_str(), value))
				return exception_marker;
			return instance->get(key.as_str());
		}

		std::array<numeric_update, 1> updates{{{
			 .target    = instance,
			 .key       = any(key),
			 .operation = numeric_operation::set,
			 .operand   = value.as_num(),
		}}};
		std::array<number, 1>         results{};
		if (!atomic_update(caller, updates, results))
			return exception_marker;
		return any(results[0]);
	}

	any_t atomic_field_update(vm* caller, any_t target, any_t key, numeric_operation operation, number operand) {
		if (!target.is_obj() || !key.is_str() || !find_atomic_field(target.as_obj(), key.as_str()))
			return caller->error("atomic field update requires an atomic object field");
		if (static_cast<uint8_t>(operation) > static_cast<uint8_t>(numeric_operation::mod))
			return caller->error("atomic field update contains an invalid operation");

		object* instance = target.as_obj();
		if (!instance->shared) {
			any current = instance->get(key.as_str());
			if (!current.is_num())
				return caller->error("atomic field is not numeric");
			any next(apply_numeric(operation, current.as_num(), operand));
			if (!instance->set(caller, key.as_str(), next))
				return exception_marker;
			return instance->get(key.as_str());
		}

		std::array<numeric_update, 1> updates{{{
			 .target    = instance,
			 .key       = any(key),
			 .operation = operation,
			 .operand   = operand,
		}}};
		std::array<number, 1>         results{};
		if (!atomic_update(caller, updates, results))
			return exception_marker;
		return any(results[0]);
	}
}
