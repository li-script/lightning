#include <lightning.h>

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>

struct native_binding {
	li_native_fn   callback = nullptr;
	void*          userdata = nullptr;
	std::string    name;
	li::nfunc_info info;
};

struct li_vm {
	std::atomic_size_t    references{1};
	std::atomic_bool      open{true};
	std::atomic_bool      allocator_threw{false};
	std::atomic<uint64_t> error_generation{0};
	li::vm*               state = nullptr;

	li_allocator_fn allocator          = nullptr;
	void*           allocator_userdata = nullptr;
	li_import_fn    import             = nullptr;
	void*           import_userdata    = nullptr;
	li_error_fn     error              = nullptr;
	void*           error_userdata     = nullptr;

	std::mutex                                                         error_mutex;
	std::string                                                        last_error;
	std::unordered_map<li::function*, std::unique_ptr<native_binding>> native_bindings;
	std::unordered_set<std::string>                                    native_names;
	std::unordered_map<li_type_id, li::vclass*>                        userdata_classes;
};

struct li_value {
	li_vm*  owner  = nullptr;
	li::any value  = li::nil;
	bool    owning = false;
};

namespace {
	struct userdata_payload {
		li_vm*                 owner;
		void*                  pointer;
		li_userdata_destroy_fn destroy;
		void*                  destroy_userdata;
		li_type_id             type;
		uint32_t               reserved;
		uint64_t               magic;
	};

	constexpr uint64_t userdata_magic = 0x4c49554441544131ull;

	std::mutex                                  controller_mutex;
	std::unordered_map<li::vm*, li_vm*>         controllers;
	std::mutex                                  userdata_type_mutex;
	std::unordered_map<std::string, li_type_id> userdata_type_by_name;
	std::unordered_map<li_type_id, std::string> userdata_name_by_type;
	li_type_id                                  next_userdata_type = 2;

	void retain_control(li_vm* vm) noexcept { vm->references.fetch_add(1, std::memory_order_relaxed); }

	void publish_error(li_vm* vm, li_status status, std::string_view message) noexcept {
		if (!vm)
			return;
		try {
			std::lock_guard guard(vm->error_mutex);
			vm->last_error.assign(message);
		} catch (...) {
		}

		vm->error_generation.fetch_add(1, std::memory_order_release);
		if (vm->error) {
			try {
				vm->error(vm->error_userdata, status, message.data(), message.size());
			} catch (...) {
			}
		}
	}

	std::string error_message(li_vm* vm) {
		std::lock_guard guard(vm->error_mutex);
		return vm->last_error;
	}

	void set_script_error(li_vm* vm, std::string_view message) {
		li::string* text = li::string::create(vm->state, message);
		vm->state->error(li::any(text));
		li::rc::release(vm->state, text);
	}

	std::string script_error_message(li_vm* vm) {
		if (vm->state->last_ex == li::nil)
			return "script failed without an exception value";
		return vm->state->last_ex.to_string();
	}

	void* allocator_bridge(void* context, void* pointer, size_t page_count, bool executable) noexcept {
		auto* vm = static_cast<li_vm*>(context);
		if (pointer == context && page_count == 0)
			return nullptr;
		try {
			return vm->allocator(vm->allocator_userdata, pointer, page_count, executable ? 1 : 0);
		} catch (...) {
			vm->allocator_threw.store(true, std::memory_order_relaxed);
			return nullptr;
		}
	}

	void userdata_destroy(li::object* object) noexcept {
		auto* payload  = reinterpret_cast<userdata_payload*>(object->context);
		payload->magic = 0;
		if (!payload->destroy)
			return;
		try {
			payload->destroy(payload->pointer, payload->destroy_userdata);
		} catch (...) {
			publish_error(payload->owner, LI_STATUS_CXX_EXCEPTION, "userdata destructor threw a C++ exception");
		}
	}

	bool is_userdata(li::any_t value) noexcept {
		if (!value.is_obj())
			return false;
		li::object* object = value.as_obj();
		if (object->gc_hook != &userdata_destroy || !object->cl || object->cl->object_length < sizeof(userdata_payload))
			return false;
		auto* payload = reinterpret_cast<const userdata_payload*>(object->context);
		return payload->magic == userdata_magic && payload->type == object->cl->cxx_tid;
	}

	void release_control(li_vm* vm) noexcept {
		if (vm->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
			return;

		try {
			{
				std::lock_guard guard(controller_mutex);
				if (vm->state)
					controllers.erase(vm->state);
			}
			if (vm->state) {
				for (auto& entry : vm->userdata_classes)
					li::rc::release(vm->state, entry.second);
				vm->userdata_classes.clear();
				vm->state->close();
				vm->state = nullptr;
			}
		} catch (...) {
			publish_error(vm, LI_STATUS_CXX_EXCEPTION, "VM shutdown threw a C++ exception");
		}
		delete vm;
	}

	struct control_reference {
		li_vm* value = nullptr;
		explicit control_reference(li_vm* vm) noexcept : value(vm) {
			if (value)
				retain_control(value);
		}
		control_reference(const control_reference&)            = delete;
		control_reference& operator=(const control_reference&) = delete;
		~control_reference() {
			if (value)
				release_control(value);
		}
	};

	struct vm_pair_guard {
		std::unique_lock<li::util::fastlock> first;
		std::unique_lock<li::util::fastlock> second;

		vm_pair_guard(li::vm* left, li::vm* right) {
			if (left == right) {
				first = std::unique_lock(left->lock);
				return;
			}
			li::vm* lower  = std::less<li::vm*>{}(left, right) ? left : right;
			li::vm* higher = lower == left ? right : left;
			first          = std::unique_lock(lower->lock);
			second         = std::unique_lock(higher->lock);
		}
	};

	li_vm* find_control(li::vm* state) {
		std::lock_guard guard(controller_mutex);
		auto            found = controllers.find(state);
		if (found == controllers.end())
			return nullptr;
		retain_control(found->second);
		return found->second;
	}

	void release_handle_locked(li_value* value) noexcept {
		if (!value)
			return;
		li_vm* owner = value->owner;
		if (value->owning && owner && owner->state)
			li::rc::release(owner->state, value->value);
		delete value;
		if (owner)
			release_control(owner);
	}

	li::any consume_handle(li_value* value) noexcept {
		li_vm*  owner  = value->owner;
		li::any result = value->value;
		value->owning  = false;
		delete value;
		release_control(owner);
		return result;
	}

	li_status make_handle(li_vm* vm, li::any owned, li_value** result) noexcept {
		auto* handle = new (std::nothrow) li_value;
		if (!handle) {
			li::rc::release(vm->state, owned);
			publish_error(vm, LI_STATUS_OUT_OF_MEMORY, "failed allocating a value handle");
			return LI_STATUS_OUT_OF_MEMORY;
		}
		retain_control(vm);
		handle->owner  = vm;
		handle->value  = owned;
		handle->owning = true;
		*result        = handle;
		return LI_STATUS_OK;
	}

	li_status copy_handle_value(li_vm* destination, li::any_t value, li_value** result) {
		if (!li::rc::try_retain(destination->state, value)) {
			publish_error(destination, LI_STATUS_LIFETIME_ERROR, "cannot retain a value that is being destroyed");
			return LI_STATUS_LIFETIME_ERROR;
		}
		return make_handle(destination, value, result);
	}

	li_status validate_open(li_vm* vm) noexcept {
		if (!vm)
			return LI_STATUS_INVALID_ARGUMENT;
		if (!vm->open.load(std::memory_order_acquire)) {
			publish_error(vm, LI_STATUS_CLOSED, "VM is closed");
			return LI_STATUS_CLOSED;
		}
		return LI_STATUS_OK;
	}

	li_status validate_bytes(const char* data, size_t size) noexcept { return data || size == 0 ? LI_STATUS_OK : LI_STATUS_INVALID_ARGUMENT; }

	li_status exception_status(li_vm* vm) noexcept {
		try {
			throw;
		} catch (const std::bad_alloc&) {
			publish_error(vm, LI_STATUS_OUT_OF_MEMORY, "out of memory");
			return LI_STATUS_OUT_OF_MEMORY;
		} catch (...) {
			publish_error(vm, LI_STATUS_CXX_EXCEPTION, "C++ exception contained at the C API boundary");
			return LI_STATUS_CXX_EXCEPTION;
		}
	}

	li::any_t LI_CC native_trampoline_impl(li::vm* state, li::any* args, li::slot_t argument_count) {
		li_vm* vm = find_control(state);
		if (!vm)
			return state->error("embedding VM is unavailable");
		control_reference callback_reference(vm);
		release_control(vm);

		if (argument_count < 0 || argument_count > li::MAX_ARGS)
			return state->error("invalid native argument count");
		li::any target = args[2];
		if (!target.is_fn())
			return state->error("invalid native callback target");
		auto found = vm->native_bindings.find(target.as_fn());
		if (found == vm->native_bindings.end())
			return state->error("native callback binding is unavailable");

		li::vm_stack_guard                        stack_guard{state, args};
		std::array<li_value, li::MAX_ARGS>        borrowed;
		std::array<const li_value*, li::MAX_ARGS> pointers;
		for (li::slot_t index = 0; index != argument_count; ++index) {
			borrowed[size_t(index)] = li_value{vm, args[-index], false};
			pointers[size_t(index)] = &borrowed[size_t(index)];
		}

		li_value* output           = nullptr;
		li_status status           = LI_STATUS_CALLBACK_ERROR;
		uint64_t  error_generation = vm->error_generation.load(std::memory_order_acquire);
		try {
			status = found->second->callback(vm, pointers.data(), size_t(argument_count), found->second->userdata, &output);
		} catch (...) {
			publish_error(vm, LI_STATUS_CXX_EXCEPTION, "native callback threw a C++ exception");
			set_script_error(vm, "native callback threw a C++ exception");
			if (output && output->owning)
				release_handle_locked(output);
			return li::exception_marker;
		}

		if (!vm->open.load(std::memory_order_acquire)) {
			if (output && output->owning)
				release_handle_locked(output);
			set_script_error(vm, "VM was closed by a native callback");
			return li::exception_marker;
		}
		if (status != LI_STATUS_OK) {
			if (output && output->owning)
				release_handle_locked(output);
			std::string message;
			if (vm->error_generation.load(std::memory_order_acquire) != error_generation) {
				try {
					message = error_message(vm);
				} catch (...) {
				}
			}
			if (message.empty()) {
				message = "native callback failed";
				publish_error(vm, status, message);
			}
			set_script_error(vm, message);
			return li::exception_marker;
		}
		if (!output)
			return state->ok();
		if (!output->owning || output->owner != vm) {
			if (output->owning)
				release_handle_locked(output);
			publish_error(vm, LI_STATUS_CALLBACK_ERROR, "native callback returned an invalid value handle");
			set_script_error(vm, "native callback returned an invalid value handle");
			return li::exception_marker;
		}
		return state->take(consume_handle(output));
	}

	li::any_t LI_CC native_trampoline(li::vm* state, li::any* args, li::slot_t argument_count) {
		try {
			return native_trampoline_impl(state, args, argument_count);
		} catch (...) {
			return state->error("C++ exception contained in native bridge");
		}
	}

	li::any import_trampoline_impl(li::vm* state, std::string_view importer, std::string_view name) {
		li_vm* vm = find_control(state);
		if (!vm)
			return state->error("embedding VM is unavailable");
		control_reference callback_reference(vm);
		release_control(vm);

		li_value* output           = nullptr;
		li_status status           = LI_STATUS_CALLBACK_ERROR;
		uint64_t  error_generation = vm->error_generation.load(std::memory_order_acquire);
		try {
			status = vm->import(vm, importer.data(), importer.size(), name.data(), name.size(), vm->import_userdata, &output);
		} catch (...) {
			publish_error(vm, LI_STATUS_CXX_EXCEPTION, "import callback threw a C++ exception");
			set_script_error(vm, "import callback threw a C++ exception");
			if (output && output->owning)
				release_handle_locked(output);
			return li::exception_marker;
		}

		if (!vm->open.load(std::memory_order_acquire)) {
			if (output && output->owning)
				release_handle_locked(output);
			set_script_error(vm, "VM was closed by an import callback");
			return li::exception_marker;
		}
		bool valid_handle    = output && output->owning && output->owner == vm;
		bool invalid_exports = status == LI_STATUS_OK && valid_handle && !output->value.is_tbl();
		if (status != LI_STATUS_OK || !valid_handle || invalid_exports) {
			if (output && output->owning)
				release_handle_locked(output);
			std::string message;
			if (vm->error_generation.load(std::memory_order_acquire) != error_generation) {
				try {
					message = error_message(vm);
				} catch (...) {
				}
			}
			if (message.empty()) {
				message = invalid_exports ? "import callback must return an exports table" : "import callback failed";
				publish_error(vm, status == LI_STATUS_OK ? LI_STATUS_CALLBACK_ERROR : status, message);
			}
			set_script_error(vm, message);
			return li::exception_marker;
		}
		return consume_handle(output);
	}

	li::any import_trampoline(li::vm* state, std::string_view importer, std::string_view name) {
		try {
			return import_trampoline_impl(state, importer, name);
		} catch (...) {
			return state->error("C++ exception contained in import bridge");
		}
	}

	bool userdata_type_name(li_type_id type, std::string& result) {
		std::lock_guard guard(userdata_type_mutex);
		auto            found = userdata_name_by_type.find(type);
		if (found == userdata_name_by_type.end())
			return false;
		result = found->second;
		return true;
	}

	li::vclass* userdata_class(li_vm* vm, li_type_id type) {
		if (auto found = vm->userdata_classes.find(type); found != vm->userdata_classes.end())
			return found->second;

		std::string type_name;
		if (!userdata_type_name(type, type_name))
			return nullptr;
		li::string*    name         = li::string::create(vm->state, type_name);
		li::string*    storage_name = li::string::create(vm->state, "@userdata-storage");
		li::field_pair field{
			 .key = storage_name,
			 .value =
				  li::field_info{
						.ty     = li::type::ptr,
						.offset = uint32_t(sizeof(userdata_payload) - sizeof(uint64_t)),
				  },
		};
		std::array<uint8_t, sizeof(userdata_payload)> defaults{};
		li::vclass*                                   result = li::vclass::create(
			 vm->state, name, std::span<const li::field_pair>(&field, 1), defaults, std::span<const uint8_t>{}, nullptr, li::reserve_class_identity(vm->state));
		li::rc::release(vm->state, storage_name);
		li::rc::release(vm->state, name);
		result->cxx_tid = type;
		try {
			vm->userdata_classes.emplace(type, result);
		} catch (...) {
			li::rc::release(vm->state, result);
			throw;
		}
		return result;
	}

	li_kind value_kind(li::any_t value) noexcept {
		switch (value.type()) {
			case li::type_nil:
				return LI_KIND_NIL;
			case li::type_bool:
				return LI_KIND_BOOL;
			case li::type_number:
				return LI_KIND_NUMBER;
			case li::type_string:
				return LI_KIND_STRING;
			case li::type_array:
				return LI_KIND_ARRAY;
			case li::type_table:
				return LI_KIND_TABLE;
			case li::type_function:
				return LI_KIND_FUNCTION;
			case li::type_class:
				return LI_KIND_CLASS;
			case li::type_weak:
				return LI_KIND_WEAK;
			case li::type_typed_array:
				return LI_KIND_TYPED_ARRAY;
			case li::type_object:
				return is_userdata(value) ? LI_KIND_USERDATA : LI_KIND_OBJECT;
			default:
				return LI_KIND_NIL;
		}
	}
}

extern "C" {
const char* li_status_name(li_status status) {
	switch (status) {
		case LI_STATUS_OK:
			return "ok";
		case LI_STATUS_INVALID_ARGUMENT:
			return "invalid argument";
		case LI_STATUS_CLOSED:
			return "VM closed";
		case LI_STATUS_TYPE_ERROR:
			return "type error";
		case LI_STATUS_SCRIPT_ERROR:
			return "script error";
		case LI_STATUS_CALLBACK_ERROR:
			return "callback error";
		case LI_STATUS_OUT_OF_MEMORY:
			return "out of memory";
		case LI_STATUS_CXX_EXCEPTION:
			return "C++ exception";
		case LI_STATUS_LIFETIME_ERROR:
			return "value lifetime error";
		default:
			return "unknown status";
	}
}

void li_vm_options_init(li_vm_options* options) {
	if (!options)
		return;
	*options             = li_vm_options{};
	options->struct_size = sizeof(*options);
}

li_status li_vm_create(const li_vm_options* options, li_vm** result) {
	if (!result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	if (options && options->struct_size != sizeof(li_vm_options))
		return LI_STATUS_INVALID_ARGUMENT;

	auto* vm = new (std::nothrow) li_vm;
	if (!vm)
		return LI_STATUS_OUT_OF_MEMORY;
	if (options) {
		vm->allocator          = options->allocator;
		vm->allocator_userdata = options->allocator_userdata;
		vm->import             = options->import;
		vm->import_userdata    = options->import_userdata;
		vm->error              = options->error;
		vm->error_userdata     = options->error_userdata;
	}

	try {
		vm->state = vm->allocator ? li::vm::create(&allocator_bridge, vm) : li::vm::create();
		if (!vm->state) {
			li_status status = vm->allocator_threw.load(std::memory_order_relaxed) ? LI_STATUS_CXX_EXCEPTION : LI_STATUS_OUT_OF_MEMORY;
			publish_error(vm, status, status == LI_STATUS_CXX_EXCEPTION ? "allocator callback threw a C++ exception" : "failed creating VM");
			delete vm;
			return status;
		}
		li::lib::register_std(vm->state);
		if (vm->import)
			vm->state->import_fn = &import_trampoline;
		{
			std::lock_guard guard(controller_mutex);
			controllers.emplace(vm->state, vm);
		}
		*result = vm;
		return LI_STATUS_OK;
	} catch (...) {
		li_status status = exception_status(vm);
		if (vm->state)
			vm->state->close();
		delete vm;
		return status;
	}
}

li_status li_vm_close(li_vm* vm) {
	if (!vm)
		return LI_STATUS_INVALID_ARGUMENT;
	if (!vm->open.exchange(false, std::memory_order_acq_rel))
		return LI_STATUS_CLOSED;
	release_control(vm);
	return LI_STATUS_OK;
}

li_status li_vm_last_error(const li_vm* vm, const char** data, size_t* size) {
	if (!vm || !data || !size)
		return LI_STATUS_INVALID_ARGUMENT;
	auto*             mutable_vm = const_cast<li_vm*>(vm);
	control_reference reference(mutable_vm);
	try {
		std::lock_guard guard(mutable_vm->error_mutex);
		*data = mutable_vm->last_error.data();
		*size = mutable_vm->last_error.size();
		return LI_STATUS_OK;
	} catch (...) {
		return exception_status(mutable_vm);
	}
}

li_status li_vm_set_error(li_vm* vm, const char* message, size_t message_size) {
	if (!vm || validate_bytes(message, message_size) != LI_STATUS_OK)
		return LI_STATUS_INVALID_ARGUMENT;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		std::string_view    text(message ? message : "", message_size);
		set_script_error(vm, text);
		publish_error(vm, LI_STATUS_SCRIPT_ERROR, text);
		return LI_STATUS_SCRIPT_ERROR;
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_vm_load(li_vm* vm, const char* source, size_t source_size, const char* source_name, size_t source_name_size, li_value** result) {
	if (!vm || !result || validate_bytes(source, source_size) != LI_STATUS_OK || validate_bytes(source_name, source_name_size) != LI_STATUS_OK)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		li::any             loaded =
			 li::load_script(vm->state, std::string_view(source ? source : "", source_size), std::string_view(source_name ? source_name : "", source_name_size));
		if (!vm->open.load(std::memory_order_acquire)) {
			li::rc::release(vm->state, loaded);
			publish_error(vm, LI_STATUS_CLOSED, "VM was closed while loading a script");
			return LI_STATUS_CLOSED;
		}
		if (loaded.is_exc()) {
			std::string message = script_error_message(vm);
			publish_error(vm, LI_STATUS_SCRIPT_ERROR, message);
			return LI_STATUS_SCRIPT_ERROR;
		}
		return make_handle(vm, loaded, result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_vm_execute(li_vm* vm, const char* source, size_t source_size, const char* source_name, size_t source_name_size, li_value** result) {
	if (!vm || !result || validate_bytes(source, source_size) != LI_STATUS_OK || validate_bytes(source_name, source_name_size) != LI_STATUS_OK)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		li::any             function =
			 li::load_script(vm->state, std::string_view(source ? source : "", source_size), std::string_view(source_name ? source_name : "", source_name_size));
		if (!vm->open.load(std::memory_order_acquire)) {
			li::rc::release(vm->state, function);
			publish_error(vm, LI_STATUS_CLOSED, "VM was closed while loading a script");
			return LI_STATUS_CLOSED;
		}
		if (function.is_exc()) {
			std::string message = script_error_message(vm);
			publish_error(vm, LI_STATUS_SCRIPT_ERROR, message);
			return LI_STATUS_SCRIPT_ERROR;
		}
		li::any value = vm->state->call(0, function);
		li::rc::release(vm->state, function);
		if (!vm->open.load(std::memory_order_acquire)) {
			li::rc::release(vm->state, value);
			publish_error(vm, LI_STATUS_CLOSED, "VM was closed while executing a script");
			return LI_STATUS_CLOSED;
		}
		if (value.is_exc()) {
			std::string message = script_error_message(vm);
			publish_error(vm, LI_STATUS_SCRIPT_ERROR, message);
			return LI_STATUS_SCRIPT_ERROR;
		}
		return make_handle(vm, value, result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_vm_call(li_vm* vm, const li_value* function, const li_value* const* arguments, size_t argument_count, li_value** result) {
	if (!vm || !function || !result || (argument_count && !arguments) || argument_count > size_t(li::MAX_ARGS))
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	if (!function->value.is_fn() || (function->owner != vm && !li::shared::is_shared(function->value))) {
		publish_error(vm, LI_STATUS_TYPE_ERROR, "call target must be a function owned by this VM or a shared function");
		return LI_STATUS_TYPE_ERROR;
	}
	for (size_t index = 0; index != argument_count; ++index) {
		if (!arguments[index] || (arguments[index]->owner != vm && !li::shared::is_shared(arguments[index]->value))) {
			publish_error(vm, LI_STATUS_INVALID_ARGUMENT, "cross-VM call arguments must be shared values");
			return LI_STATUS_INVALID_ARGUMENT;
		}
	}
	try {
		li::vm_thread_guard lock(vm->state);
		if (!vm->state->vm_stack_headroom_available(argument_count + li::FRAME_SIZE)) {
			vm->state->vm_stack_exhausted();
			std::string message = script_error_message(vm);
			publish_error(vm, LI_STATUS_SCRIPT_ERROR, message);
			return LI_STATUS_SCRIPT_ERROR;
		}
		for (size_t index = argument_count; index != 0; --index)
			vm->state->push_stack(arguments[index - 1]->value);
		li::any value = vm->state->call(li::slot_t(argument_count), function->value);
		if (!vm->open.load(std::memory_order_acquire)) {
			li::rc::release(vm->state, value);
			publish_error(vm, LI_STATUS_CLOSED, "VM was closed while calling a function");
			return LI_STATUS_CLOSED;
		}
		if (value.is_exc()) {
			std::string message = script_error_message(vm);
			publish_error(vm, LI_STATUS_SCRIPT_ERROR, message);
			return LI_STATUS_SCRIPT_ERROR;
		}
		return make_handle(vm, value, result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_vm_register_native(li_vm* vm, const char* name, size_t name_size, li_native_fn callback, void* userdata) {
	if (!vm || !callback || validate_bytes(name, name_size) != LI_STATUS_OK || name_size == 0)
		return LI_STATUS_INVALID_ARGUMENT;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		std::string         owned_name(name, name_size);
		if (owned_name.find('.') == std::string::npos)
			owned_name.insert(0, "builtin.");
		if (vm->native_names.contains(owned_name)) {
			publish_error(vm, LI_STATUS_INVALID_ARGUMENT, "native name is already registered");
			return LI_STATUS_INVALID_ARGUMENT;
		}
		auto binding       = std::make_unique<native_binding>();
		binding->callback  = callback;
		binding->userdata  = userdata;
		binding->name      = owned_name;
		binding->info.attr = li::func_attr_sideeffect;
		binding->info.name = binding->name.c_str();

		li::function* function = li::function::create(vm->state, &native_trampoline);
		function->ninfo        = &binding->info;
		try {
			vm->native_bindings.emplace(function, std::move(binding));
			vm->native_names.emplace(owned_name);
			if (!li::util::export_as(vm->state, owned_name, li::any(function))) {
				std::string message = script_error_message(vm);
				li::rc::release(vm->state, function);
				vm->native_bindings.erase(function);
				vm->native_names.erase(owned_name);
				publish_error(vm, LI_STATUS_LIFETIME_ERROR, message);
				return LI_STATUS_LIFETIME_ERROR;
			}
		} catch (...) {
			vm->native_bindings.erase(function);
			vm->native_names.erase(owned_name);
			li::rc::release(vm->state, function);
			throw;
		}
		li::rc::release(vm->state, function);
		return LI_STATUS_OK;
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_create_nil(li_vm* vm, li_value** result) {
	if (!vm || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		return make_handle(vm, li::nil, result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_create_bool(li_vm* vm, int value, li_value** result) {
	if (!vm || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		return make_handle(vm, li::any(value != 0), result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_create_number(li_vm* vm, double value, li_value** result) {
	if (!vm || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		return make_handle(vm, li::any(value), result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_create_string(li_vm* vm, const char* data, size_t size, li_value** result) {
	if (!vm || !result || validate_bytes(data, size) != LI_STATUS_OK)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		li::string*         value = li::string::create(vm->state, std::string_view(data ? data : "", size));
		return make_handle(vm, li::any(value), result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_copy(const li_value* value, li_value** result) {
	if (!value || !value->owner || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result              = nullptr;
	li_vm*            vm = value->owner;
	control_reference reference(vm);
	try {
		li::vm_thread_guard lock(vm->state);
		if (!li::rc::try_retain(vm->state, value->value)) {
			std::string message = script_error_message(vm);
			publish_error(vm, LI_STATUS_LIFETIME_ERROR, message);
			return LI_STATUS_LIFETIME_ERROR;
		}
		return make_handle(vm, value->value, result);
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_share(li_vm* destination, const li_value* source, li_value** result) {
	if (!destination || !source || !source->owner || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference destination_reference(destination);
	control_reference source_reference(source->owner);
	if (li_status status = validate_open(destination); status != LI_STATUS_OK)
		return status;
	try {
		vm_pair_guard lock(destination->state, source->owner->state);
		li::any       shared = li::shared::make(destination->state, source->value);
		if (shared.is_exc()) {
			std::string message = script_error_message(destination);
			publish_error(destination, LI_STATUS_SCRIPT_ERROR, message);
			return LI_STATUS_SCRIPT_ERROR;
		}
		return make_handle(destination, shared, result);
	} catch (...) {
		return exception_status(destination);
	}
}

li_status li_value_copy_to(li_vm* destination, const li_value* source, li_value** result) {
	if (!destination || !source || !source->owner || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference destination_reference(destination);
	control_reference source_reference(source->owner);
	if (li_status status = validate_open(destination); status != LI_STATUS_OK)
		return status;
	try {
		if (source->owner == destination) {
			li::vm_thread_guard lock(destination->state);
			return copy_handle_value(destination, source->value, result);
		}
		if (!source->value.is_gc() || li::shared::is_shared(source->value)) {
			li::vm_thread_guard lock(destination->state);
			return copy_handle_value(destination, source->value, result);
		}
		if (!source->value.is_str()) {
			publish_error(destination, LI_STATUS_TYPE_ERROR, "private mutable values must be explicitly shared before cross-VM transfer");
			return LI_STATUS_TYPE_ERROR;
		}

		vm_pair_guard lock(destination->state, source->owner->state);
		li::string*   copied = li::string::create(destination->state, source->value.as_str()->view());
		return make_handle(destination, li::any(copied), result);
	} catch (...) {
		return exception_status(destination);
	}
}

void li_value_release(li_value* value) {
	if (!value || !value->owner)
		return;
	li_vm*            vm = value->owner;
	control_reference reference(vm);
	try {
		li::vm_thread_guard lock(vm->state);
		release_handle_locked(value);
	} catch (...) {
		publish_error(vm, LI_STATUS_CXX_EXCEPTION, "C++ exception while releasing a value handle");
	}
}

li_kind li_value_get_kind(const li_value* value) {
	if (!value)
		return LI_KIND_NIL;
	return value_kind(value->value);
}

li_status li_value_get_bool(const li_value* value, int* result) {
	if (!value || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	if (!value->value.is_bool())
		return LI_STATUS_TYPE_ERROR;
	*result = value->value.as_bool() ? 1 : 0;
	return LI_STATUS_OK;
}

li_status li_value_get_number(const li_value* value, double* result) {
	if (!value || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	if (!value->value.is_num())
		return LI_STATUS_TYPE_ERROR;
	*result = value->value.as_num();
	return LI_STATUS_OK;
}

li_status li_value_get_string(const li_value* value, const char** data, size_t* size) {
	if (!value || !data || !size)
		return LI_STATUS_INVALID_ARGUMENT;
	if (!value->value.is_str())
		return LI_STATUS_TYPE_ERROR;
	li::string* string = value->value.as_str();
	*data              = string->data;
	*size              = string->length;
	return LI_STATUS_OK;
}

li_status li_userdata_type_register(const char* name, size_t name_size, li_type_id* result) {
	if (!result || validate_bytes(name, name_size) != LI_STATUS_OK || name_size == 0)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = 0;
	try {
		std::string     key(name, name_size);
		std::lock_guard guard(userdata_type_mutex);
		if (auto found = userdata_type_by_name.find(key); found != userdata_type_by_name.end()) {
			*result = found->second;
			return LI_STATUS_OK;
		}
		if (next_userdata_type > std::numeric_limits<li_type_id>::max() - 2)
			return LI_STATUS_OUT_OF_MEMORY;
		li_type_id type = next_userdata_type;
		next_userdata_type += 2;
		userdata_type_by_name.emplace(key, type);
		try {
			userdata_name_by_type.emplace(type, key);
		} catch (...) {
			userdata_type_by_name.erase(key);
			next_userdata_type -= 2;
			throw;
		}
		*result = type;
		return LI_STATUS_OK;
	} catch (const std::bad_alloc&) {
		return LI_STATUS_OUT_OF_MEMORY;
	} catch (...) {
		return LI_STATUS_CXX_EXCEPTION;
	}
}

li_status li_value_create_userdata(li_vm* vm, li_type_id type, void* pointer, li_userdata_destroy_fn destroy, void* destroy_userdata, li_value** result) {
	if (!vm || !result || type == 0)
		return LI_STATUS_INVALID_ARGUMENT;
	*result = nullptr;
	control_reference reference(vm);
	if (li_status status = validate_open(vm); status != LI_STATUS_OK)
		return status;
	try {
		li::vm_thread_guard lock(vm->state);
		li::vclass*         type_class = userdata_class(vm, type);
		if (!type_class) {
			publish_error(vm, LI_STATUS_INVALID_ARGUMENT, "userdata type is not registered");
			return LI_STATUS_INVALID_ARGUMENT;
		}
		auto* handle = new (std::nothrow) li_value;
		if (!handle) {
			publish_error(vm, LI_STATUS_OUT_OF_MEMORY, "failed allocating a value handle");
			return LI_STATUS_OUT_OF_MEMORY;
		}

		li::object* object = li::object::create(vm->state, type_class);
		new (object->context) userdata_payload{
			 .owner            = vm,
			 .pointer          = pointer,
			 .destroy          = destroy,
			 .destroy_userdata = destroy_userdata,
			 .type             = type,
			 .reserved         = 0,
			 .magic            = userdata_magic,
		};
		object->gc_hook = &userdata_destroy;
		retain_control(vm);
		handle->owner  = vm;
		handle->value  = li::any(object);
		handle->owning = true;
		*result        = handle;
		return LI_STATUS_OK;
	} catch (...) {
		return exception_status(vm);
	}
}

li_status li_value_get_userdata_type(const li_value* value, li_type_id* result) {
	if (!value || !result)
		return LI_STATUS_INVALID_ARGUMENT;
	if (!is_userdata(value->value))
		return LI_STATUS_TYPE_ERROR;
	*result = reinterpret_cast<const userdata_payload*>(value->value.as_obj()->context)->type;
	return LI_STATUS_OK;
}

li_status li_value_get_userdata(const li_value* value, li_type_id type, void** result) {
	if (!value || !result || type == 0)
		return LI_STATUS_INVALID_ARGUMENT;
	if (!is_userdata(value->value))
		return LI_STATUS_TYPE_ERROR;
	auto* payload = reinterpret_cast<const userdata_payload*>(value->value.as_obj()->context);
	if (payload->type != type)
		return LI_STATUS_TYPE_ERROR;
	*result = payload->pointer;
	return LI_STATUS_OK;
}
}
