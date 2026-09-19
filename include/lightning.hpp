#pragma once

#include <lightning.h>

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lightning {
	class error : public std::runtime_error {
		li_status code_;

	  public:
		error(li_status code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}
		li_status code() const noexcept { return code_; }
	};

	namespace detail {
		[[noreturn]] inline void throw_error(li_vm* vm, li_status status) {
			const char* message = nullptr;
			size_t      size    = 0;
			if (vm)
				li_vm_last_error(vm, &message, &size);
			if (!message || size == 0) {
				message = li_status_name(status);
				size    = std::char_traits<char>::length(message);
			}
			throw error(status, std::string(message, size));
		}

		inline void check(li_vm* vm, li_status status) {
			if (status != LI_STATUS_OK)
				throw_error(vm, status);
		}
	}

	class vm;

	class value {
		li_value* handle_ = nullptr;

		explicit value(li_value* handle) noexcept : handle_(handle) {}
		friend class vm;

	  public:
		value() noexcept = default;
		~value() { li_value_release(handle_); }

		value(const value& other) {
			if (other.handle_)
				detail::check(nullptr, li_value_copy(other.handle_, &handle_));
		}
		value(value&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
		value& operator=(const value& other) {
			if (this == &other)
				return *this;
			li_value* next = nullptr;
			if (other.handle_)
				detail::check(nullptr, li_value_copy(other.handle_, &next));
			li_value_release(handle_);
			handle_ = next;
			return *this;
		}
		value& operator=(value&& other) noexcept {
			if (this != &other) {
				li_value_release(handle_);
				handle_ = std::exchange(other.handle_, nullptr);
			}
			return *this;
		}

		explicit        operator bool() const noexcept { return handle_ != nullptr; }
		li_kind         kind() const noexcept { return li_value_get_kind(handle_); }
		li_value*       native_handle() noexcept { return handle_; }
		const li_value* native_handle() const noexcept { return handle_; }

		bool as_bool() const {
			int result = 0;
			detail::check(nullptr, li_value_get_bool(handle_, &result));
			return result != 0;
		}
		double as_number() const {
			double result = 0;
			detail::check(nullptr, li_value_get_number(handle_, &result));
			return result;
		}
		std::string_view as_string() const {
			const char* data = nullptr;
			size_t      size = 0;
			detail::check(nullptr, li_value_get_string(handle_, &data, &size));
			return {data, size};
		}
		li_type_id userdata_type() const {
			li_type_id result = 0;
			detail::check(nullptr, li_value_get_userdata_type(handle_, &result));
			return result;
		}
		void* userdata(li_type_id type) const {
			void* result = nullptr;
			detail::check(nullptr, li_value_get_userdata(handle_, type, &result));
			return result;
		}
	};

	class vm {
		li_vm* handle_ = nullptr;

	  public:
		explicit vm(const li_vm_options* options = nullptr) { detail::check(nullptr, li_vm_create(options, &handle_)); }
		~vm() { close(); }

		vm(const vm&)            = delete;
		vm& operator=(const vm&) = delete;
		vm(vm&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
		vm& operator=(vm&& other) noexcept {
			if (this != &other) {
				close();
				handle_ = std::exchange(other.handle_, nullptr);
			}
			return *this;
		}

		li_vm*       native_handle() noexcept { return handle_; }
		const li_vm* native_handle() const noexcept { return handle_; }
		explicit     operator bool() const noexcept { return handle_ != nullptr; }

		void close() noexcept {
			if (handle_) {
				li_vm_close(handle_);
				handle_ = nullptr;
			}
		}

		value load(std::string_view source, std::string_view source_name = {}) {
			li_value* result = nullptr;
			detail::check(handle_, li_vm_load(handle_, source.data(), source.size(), source_name.data(), source_name.size(), &result));
			return value(result);
		}
		value execute(std::string_view source, std::string_view source_name = {}) {
			li_value* result = nullptr;
			detail::check(handle_, li_vm_execute(handle_, source.data(), source.size(), source_name.data(), source_name.size(), &result));
			return value(result);
		}
		value call(const value& function, std::span<const value> arguments = {}) {
			std::vector<const li_value*> raw;
			raw.reserve(arguments.size());
			for (const value& argument : arguments)
				raw.push_back(argument.native_handle());
			li_value* result = nullptr;
			detail::check(handle_, li_vm_call(handle_, function.native_handle(), raw.data(), raw.size(), &result));
			return value(result);
		}
		/*
		 * Both operations borrow source and return a new independently owned
		 * destination handle. share makes an explicit shared-heap copy; copy_to
		 * transfers only values that are safe for the destination VM.
		 */
		value share(const value& source) {
			li_value* result = nullptr;
			detail::check(handle_, li_value_share(handle_, source.native_handle(), &result));
			return value(result);
		}
		value copy_to(const value& source) {
			li_value* result = nullptr;
			detail::check(handle_, li_value_copy_to(handle_, source.native_handle(), &result));
			return value(result);
		}
		void register_native(std::string_view name, li_native_fn callback, void* userdata = nullptr) {
			detail::check(handle_, li_vm_register_native(handle_, name.data(), name.size(), callback, userdata));
		}

		value nil() {
			li_value* result = nullptr;
			detail::check(handle_, li_value_create_nil(handle_, &result));
			return value(result);
		}
		value boolean(bool input) {
			li_value* result = nullptr;
			detail::check(handle_, li_value_create_bool(handle_, input, &result));
			return value(result);
		}
		value number(double input) {
			li_value* result = nullptr;
			detail::check(handle_, li_value_create_number(handle_, input, &result));
			return value(result);
		}
		value string(std::string_view input) {
			li_value* result = nullptr;
			detail::check(handle_, li_value_create_string(handle_, input.data(), input.size(), &result));
			return value(result);
		}
		value userdata(li_type_id type, void* pointer, li_userdata_destroy_fn destroy = nullptr, void* destroy_userdata = nullptr) {
			li_value* result = nullptr;
			detail::check(handle_, li_value_create_userdata(handle_, type, pointer, destroy, destroy_userdata, &result));
			return value(result);
		}
	};

	inline li_type_id register_userdata_type(std::string_view name) {
		li_type_id result = 0;
		detail::check(nullptr, li_userdata_type_register(name.data(), name.size(), &result));
		return result;
	}
}
