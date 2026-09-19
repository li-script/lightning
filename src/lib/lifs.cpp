#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <lang/parser.hpp>
#include <lib/fs.hpp>
#include <system_error>
#include <util/user.hpp>
#include <vector>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/rc.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li::lib::fs {
	namespace {
		enum class module_status : uint8_t {
			loading,
			initialized,
			failed,
		};
		enum class record_field : int8_t {
			status,
			exports,
			error,
			previous,
			name,
		};

		static any record_key(record_field field) { return any(number(static_cast<int8_t>(field))); }
		static any initialized_head_key() { return any(number(-1)); }

		static void clear_table(vm* L, table* target) {
			// Transfer the entries out before releasing them: finalizers may
			// re-enter and observe or mutate the already-empty table. Reusing the
			// backing store also keeps shutdown clearing GC-allocation-free after
			// gc::state::close has disabled new allocations.
			std::vector<table_entry> retired;
			retired.reserve(target->active_count);
			for (table_entry& entry : *target) {
				if (entry.key == nil)
					continue;
				retired.push_back(entry);
				entry = {nil, nil};
			}
			target->active_count = 0;

			for (const table_entry& entry : retired) {
				rc::release(L, entry.key);
				rc::release(L, entry.value);
			}
		}

		static void clear_record(vm* L, table* record) { clear_table(L, record); }

		static any_t runtime_import(vm* L, any* args, slot_t n) {
			vm_stack_guard stack_guard{L, args};
			if (L->gc.shutting_down)
				return L->error("cannot import while VM is shutting down");
			if (n != 3 || !args[0].is_str() || !args[-1].is_str() || !args[-2].is_bool())
				return L->error("invalid runtime import call");

			string* importer = args[0].as_str();
			string* name     = args[-1].as_str();
			bool    optional = args[-2].as_bool();

			if (!L->module_records)
				L->module_records = table::create(L, 8);
			table* records = L->module_records;

			any cached = records->get(L, any(name));
			if (cached.is_tbl()) {
				table*        record       = cached.as_tbl();
				any           status_value = record->get(L, record_key(record_field::status));
				module_status status = status_value.is_num() ? static_cast<module_status>(static_cast<uint8_t>(status_value.as_num())) : module_status::failed;

				if (status == module_status::loading || status == module_status::initialized)
					return L->ok(record->get(L, record_key(record_field::exports)));

				if (optional) {
					L->clear_exception();
					return L->ok();
				}

				any error = record->get(L, record_key(record_field::error));
				if (error == nil)
					return L->error("module '%s' failed to load", name->c_str());
				return L->error(error);
			}

			// Publishing the language-visible module name must use the checked
			// retain boundary. Holding this guard makes every subsequent table
			// insertion safe until all record/cache owners are installed.
			//
			if (!rc::try_retain(L, name))
				return exception_marker;

			// Publish the record and export table before invoking the loader so a
			// recursive import observes this exact table and never starts a
			// second loader.
			//
			table* record         = table::create(L, 5);
			table* exports        = table::create(L);
			auto   abandon_record = [&] {
				clear_table(L, exports);
				L->modules->erase(L, any(name));
				records->erase(L, any(name));
				clear_record(L, record);
				rc::release(L, exports);
				rc::release(L, record);
				rc::release(L, name);
			};
			if (!record->set(L, record_key(record_field::status), any(number(static_cast<uint8_t>(module_status::loading)))) ||
				 !record->set(L, record_key(record_field::exports), any(exports)) || !record->set(L, record_key(record_field::name), any(name)) ||
				 !records->set(L, any(name), any(record)) || !L->modules->set(L, any(name), any(exports))) {
				abandon_record();
				return exception_marker;
			}

			any loaded = L->import_fn ? L->import_fn(L, importer->view(), name->view()) : nil;
			if (loaded == nil || loaded.is_exc()) {
				if (loaded == nil)
					L->error("module '%s' not found", name->c_str());
				else if (L->last_ex == nil)
					L->error("module '%s' failed to load", name->c_str());

				any failure = L->last_ex;
				if (!rc::try_retain(L, failure)) {
					failure = L->last_ex;
					if (!rc::try_retain(L, failure)) {
						rc::release(L, loaded);
						abandon_record();
						return exception_marker;
					}
				}
				if (!record->set(L, record_key(record_field::error), failure) ||
					 !record->set(L, record_key(record_field::status), any(number(static_cast<uint8_t>(module_status::failed))))) {
					rc::release(L, failure);
					rc::release(L, loaded);
					abandon_record();
					return exception_marker;
				}

				clear_table(L, exports);
				L->modules->erase(L, any(name));
				rc::release(L, loaded);
				rc::release(L, exports);
				rc::release(L, record);
				rc::release(L, name);

				if (optional) {
					rc::release(L, failure);
					L->clear_exception();
					return L->ok();
				}
				L->error(failure);
				rc::release(L, failure);
				return exception_marker;
			}

			// Replace the placeholder only for host-provided virtual modules.
			// Script loaders return the already-published export table.
			//
			if (!record->set(L, record_key(record_field::exports), loaded) || !L->modules->set(L, any(name), loaded) ||
				 !record->set(L, record_key(record_field::previous), records->get(L, initialized_head_key()))) {
				rc::release(L, loaded);
				abandon_record();
				return exception_marker;
			}
			if (!records->set(L, initialized_head_key(), any(record)) ||
				 !record->set(L, record_key(record_field::status), any(number(static_cast<uint8_t>(module_status::initialized))))) {
				rc::release(L, loaded);
				abandon_record();
				return exception_marker;
			}

			rc::release(L, exports);
			rc::release(L, record);
			rc::release(L, name);
			return L->take(loaded);
		}
	}

	util::native_function detail::module_import = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 nullptr,
		 &runtime_import,
	};

	any default_import(vm* L, std::string_view importer, std::string_view name) {
		// Get the file name.
		//
		std::string file_name{name};
		if (!name.ends_with(".li") && !name.ends_with(".LI") && !name.ends_with(".Li") && !name.ends_with(".lI")) {
			file_name += ".li";
		}

		// Read the file.
		//
		auto file = read_string(file_name.c_str());
		if (!file && !importer.empty()) {
			auto pos = importer.find_last_of("/\\");
			file_name.insert(file_name.begin(), importer.begin(), importer.begin() + (pos + 1));
			file = read_string(file_name.c_str());
		}
		if (!file)
			return L->error("failed reading file '%s'", file_name.c_str());

		// Compile and execute while the runtime record keeps the pre-published
		// export table alive. Both load_script and call return owned values.
		//
		any script = load_script(L, *file, file_name, name);
		if (script.is_exc())
			return script;
		any value = L->call(0, script);
		rc::release(L, script);
		if (value.is_exc())
			return value;
		rc::release(L, value);

		auto* key    = string::create(L, name);
		any   result = L->modules->get(L, any(key));
		rc::release(L, key);
		if (result == nil)
			return L->error("module '%.*s' did not publish exports", static_cast<uint32_t>(name.size()), name.data());
		return L->ok(result);
	}
}

namespace li {
	void shutdown_module_records(vm* L) {
		table* records = L->module_records;
		if (!records)
			return;

		// Successful records form a private newest-to-oldest chain. Detach and
		// clear them in that order before the generic owning table is released.
		//
		any head_key = lib::fs::initialized_head_key();
		any current  = records->get(L, head_key);
		rc::retain(current);
		records->erase(L, head_key);
		while (current.is_tbl()) {
			table* record = current.as_tbl();
			any    next   = record->get(L, lib::fs::record_key(lib::fs::record_field::previous));
			any    name   = record->get(L, lib::fs::record_key(lib::fs::record_field::name));
			rc::retain(next);
			rc::retain(name);

			if (name != nil) {
				if (L->modules)
					L->modules->erase(L, name);
				records->erase(L, name);
			}
			lib::fs::clear_record(L, record);
			rc::release(L, name);
			rc::release(L, current);
			current = next;
		}
		rc::release(L, current);

		// Failed records are not part of the successful initialization chain,
		// but their cached errors and cleared placeholder tables are owning too.
		//
		std::vector<std::pair<any, any>> pending;
		for (table_entry& entry : *records) {
			if (!entry.key.is_str() || !entry.value.is_tbl())
				continue;
			rc::retain(entry.key);
			rc::retain(entry.value);
			pending.emplace_back(entry.key, entry.value);
		}
		for (auto [key, record_value] : pending) {
			records->erase(L, key);
			if (L->modules)
				L->modules->erase(L, key);
			lib::fs::clear_record(L, record_value.as_tbl());
			rc::release(L, record_value);
			rc::release(L, key);
		}
	}
}

#if !LI_NO_STD_FS
	#include <cstdio>

// To ensure compatibility across STL versions, script loading continues to use
// checked C stdio. The language-facing operations use std::filesystem's
// error_code overloads for metadata and directory work.
//
namespace li::lib::fs {
	static std::filesystem::path utf8_path(std::string_view path) {
		const auto utf8 = std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size());
		return std::filesystem::path(utf8);
	}

	static FILE* open_read_file(const char* path) {
	#if LI_WINDOWS
		try {
			const std::filesystem::path native_path = utf8_path(path);
			return _wfopen(native_path.c_str(), L"rb");
		} catch (...) {
			return nullptr;
		}
	#else
		return fopen(path, "rb");
	#endif
	}

	std::optional<std::string> read_string(const char* path) {
		FILE* file = open_read_file(path);
		if (!file)
			return std::nullopt;

		if (fseek(file, 0, SEEK_END) != 0) {
			fclose(file);
			return std::nullopt;
		}
		const long length = ftell(file);
		if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
			fclose(file);
			return std::nullopt;
		}

		std::optional<std::string> buffer;
		try {
			buffer.emplace(static_cast<size_t>(length), '\0');
		} catch (...) {
			fclose(file);
			return std::nullopt;
		}

		const size_t expected = buffer->size();
		const size_t actual   = expected == 0 ? 0 : fread(buffer->data(), 1, expected, file);
		bool         failed   = actual != expected || ferror(file) != 0;
		if (fclose(file) != 0)
			failed = true;
		if (failed)
			return std::nullopt;
		return buffer;
	}
}

namespace li::lib {
	namespace {
		namespace stdfs = std::filesystem;

		static bool path_argument(vm* L, any value, const char* operation, std::string_view& result) {
			if (!value.is_str()) {
				L->error("%s expects a path string", operation);
				return false;
			}
			result = value.as_str()->view();
			if (result.find('\0') != std::string_view::npos) {
				L->error("%s path contains a NUL byte", operation);
				return false;
			}
			return true;
		}

		static FILE* open_write_file(const stdfs::path& path) {
	#if LI_WINDOWS
			return _wfopen(path.c_str(), L"wb");
	#else
			return fopen(path.c_str(), "wb");
	#endif
		}

		static any_t filesystem_error(vm* L, const char* operation, std::string_view path, const std::error_code& error) {
			const std::string message = error ? error.message() : "operation failed";
			return L->error("%s failed for '%s': %s", operation, path.data(), message.c_str());
		}

		template<typename Fn>
		static any_t contain_filesystem_exception(vm* L, Fn&& operation) {
			try {
				return operation();
			} catch (const std::exception& exception) {
				return L->error("filesystem operation failed: %s", exception.what());
			} catch (...) {
				return L->error("filesystem operation failed: native exception");
			}
		}

		static any_t fs_read(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 1)
					return L->error("fs.read expects one path");
				if (!path_argument(L, args[0], "fs.read", path))
					return exception_marker;

				const stdfs::path        native_path = fs::utf8_path(path);
				std::error_code          error;
				const stdfs::file_status status = stdfs::status(native_path, error);
				if (error)
					return filesystem_error(L, "fs.read", path, error);
				if (!stdfs::is_regular_file(status))
					return filesystem_error(L, "fs.read", path, std::make_error_code(std::errc::invalid_argument));

				const std::u8string encoded_path = native_path.u8string();
				const std::string   path_bytes(reinterpret_cast<const char*>(encoded_path.data()), encoded_path.size());
				errno         = 0;
				auto contents = fs::read_string(path_bytes.c_str());
				if (!contents)
					return filesystem_error(L, "fs.read", path, std::error_code(errno, std::generic_category()));
				return L->take(string::create(L, *contents));
			});
		}

		static any_t fs_write(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 2)
					return L->error("fs.write expects a path and byte string");
				if (!path_argument(L, args[0], "fs.write", path))
					return exception_marker;
				if (!args[-1].is_str())
					return L->error("fs.write expects bytes as a string");

				const stdfs::path native_path = fs::utf8_path(path);
				const auto        bytes       = args[-1].as_str()->view();
				errno                         = 0;
				FILE* file                    = open_write_file(native_path);
				if (!file)
					return filesystem_error(L, "fs.write", path, std::error_code(errno, std::generic_category()));

				size_t written = 0;
				while (written != bytes.size()) {
					const size_t amount = fwrite(bytes.data() + written, 1, bytes.size() - written, file);
					if (amount == 0) {
						const int error = errno;
						fclose(file);
						return filesystem_error(L, "fs.write", path, std::error_code(error, std::generic_category()));
					}
					written += amount;
				}
				if (fclose(file) != 0)
					return filesystem_error(L, "fs.write", path, std::error_code(errno, std::generic_category()));
				return L->ok(number(written));
			});
		}

		static any_t fs_exists(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 1)
					return L->error("fs.exists expects one path");
				if (!path_argument(L, args[0], "fs.exists", path))
					return exception_marker;

				std::error_code error;
				const bool      exists = stdfs::exists(fs::utf8_path(path), error);
				if (error)
					return filesystem_error(L, "fs.exists", path, error);
				return L->ok(exists);
			});
		}

		static any_t fs_remove(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 1)
					return L->error("fs.remove expects one path");
				if (!path_argument(L, args[0], "fs.remove", path))
					return exception_marker;

				std::error_code error;
				const bool      removed = stdfs::remove(fs::utf8_path(path), error);
				if (error)
					return filesystem_error(L, "fs.remove", path, error);
				return L->ok(removed);
			});
		}

		static any_t fs_create_directory(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 1)
					return L->error("fs.create_directory expects one path");
				if (!path_argument(L, args[0], "fs.create_directory", path))
					return exception_marker;

				std::error_code error;
				const bool      created = stdfs::create_directory(fs::utf8_path(path), error);
				if (error)
					return filesystem_error(L, "fs.create_directory", path, error);
				return L->ok(created);
			});
		}

		static any_t fs_list(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 1)
					return L->error("fs.list expects one path");
				if (!path_argument(L, args[0], "fs.list", path))
					return exception_marker;

				std::error_code           error;
				stdfs::directory_iterator iterator(fs::utf8_path(path), error);
				if (error)
					return filesystem_error(L, "fs.list", path, error);

				std::vector<std::string> names;
				for (const stdfs::directory_iterator end; iterator != end; iterator.increment(error)) {
					if (error)
						return filesystem_error(L, "fs.list", path, error);
					const std::u8string encoded = iterator->path().filename().u8string();
					names.emplace_back(reinterpret_cast<const char*>(encoded.data()), encoded.size());
				}
				if (error)
					return filesystem_error(L, "fs.list", path, error);
				std::sort(names.begin(), names.end());

				array* result = array::create(L, 0, static_cast<msize_t>(names.size()));
				for (const std::string& name : names) {
					string*    value  = string::create(L, name);
					const bool stored = result->push(L, any(value));
					rc::release(L, value);
					if (!stored) {
						rc::release(L, result);
						return exception_marker;
					}
				}
				return L->take(result);
			});
		}

		static bool set_metadata_field(vm* L, table* result, std::string_view name, any value) {
			string*    key = string::create(L, name);
			const bool set = result->set(L, any(key), value);
			rc::release(L, key);
			return set;
		}

		static any_t fs_metadata(vm* L, any* args, slot_t count) {
			return contain_filesystem_exception(L, [&]() -> any_t {
				std::string_view path;
				if (count != 1)
					return L->error("fs.metadata expects one path");
				if (!path_argument(L, args[0], "fs.metadata", path))
					return exception_marker;

				const stdfs::path        native_path = fs::utf8_path(path);
				std::error_code          error;
				const stdfs::file_status status = stdfs::symlink_status(native_path, error);
				if (error)
					return filesystem_error(L, "fs.metadata", path, error);
				if (status.type() == stdfs::file_type::not_found)
					return filesystem_error(L, "fs.metadata", path, std::make_error_code(std::errc::no_such_file_or_directory));

				std::string_view type = "other";
				if (stdfs::is_regular_file(status))
					type = "file";
				else if (stdfs::is_directory(status))
					type = "directory";
				else if (stdfs::is_symlink(status))
					type = "symlink";

				any size = nil;
				if (stdfs::is_regular_file(status)) {
					const uintmax_t file_size = stdfs::file_size(native_path, error);
					if (error)
						return filesystem_error(L, "fs.metadata", path, error);
					size = any(number(file_size));
				}

				const stdfs::file_time_type modified = stdfs::last_write_time(native_path, error);
				if (error)
					return filesystem_error(L, "fs.metadata", path, error);
				const auto   system_modified  = std::chrono::system_clock::now() + (modified - stdfs::file_time_type::clock::now());
				const number modified_seconds = std::chrono::duration<number>(system_modified.time_since_epoch()).count();

				table*     result     = table::create(L, 3);
				string*    type_value = string::create(L, type);
				const bool stored     = set_metadata_field(L, result, "type", any(type_value)) && set_metadata_field(L, result, "size", size) &&
												set_metadata_field(L, result, "modified", any(modified_seconds));
				rc::release(L, type_value);
				if (!stored) {
					rc::release(L, result);
					return exception_marker;
				}
				return L->take(result);
			});
		}
	}

	void register_fs(vm* L) {
		util::export_as(L, "fs.read", &fs_read);
		util::export_as(L, "fs.write", &fs_write);
		util::export_as(L, "fs.exists", &fs_exists);
		util::export_as(L, "fs.remove", &fs_remove);
		util::export_as(L, "fs.create_directory", &fs_create_directory);
		util::export_as(L, "fs.list", &fs_list);
		util::export_as(L, "fs.metadata", &fs_metadata);
	}
}
#endif