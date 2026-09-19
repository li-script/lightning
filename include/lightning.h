#ifndef LIGHTNING_H
#define LIGHTNING_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(LIGHTNING_SHARED)
	#if defined(LIGHTNING_BUILDING_LIBRARY)
		#define LI_API __declspec(dllexport)
	#else
		#define LI_API __declspec(dllimport)
	#endif
#elif defined(__GNUC__) && defined(LIGHTNING_SHARED)
	#define LI_API __attribute__((visibility("default")))
#else
	#define LI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct li_vm    li_vm;
typedef struct li_value li_value;
typedef uint32_t        li_type_id;

typedef enum li_status {
	LI_STATUS_OK               = 0,
	LI_STATUS_INVALID_ARGUMENT = 1,
	LI_STATUS_CLOSED           = 2,
	LI_STATUS_TYPE_ERROR       = 3,
	LI_STATUS_SCRIPT_ERROR     = 4,
	LI_STATUS_CALLBACK_ERROR   = 5,
	LI_STATUS_OUT_OF_MEMORY    = 6,
	LI_STATUS_CXX_EXCEPTION    = 7,
	LI_STATUS_LIFETIME_ERROR   = 8
} li_status;

typedef enum li_kind {
	LI_KIND_NIL         = 0,
	LI_KIND_BOOL        = 1,
	LI_KIND_NUMBER      = 2,
	LI_KIND_STRING      = 3,
	LI_KIND_ARRAY       = 4,
	LI_KIND_TABLE       = 5,
	LI_KIND_FUNCTION    = 6,
	LI_KIND_CLASS       = 7,
	LI_KIND_OBJECT      = 8,
	LI_KIND_USERDATA    = 9,
	LI_KIND_WEAK        = 10,
	LI_KIND_TYPED_ARRAY = 11
} li_kind;

/*
 * The allocator works in 4096-byte pages, matching Lightning's page allocator.
 * pointer == NULL requests page_count pages. A non-NULL pointer releases the
 * same page count and must return NULL. executable is non-zero for code pages.
 */
typedef void* (*li_allocator_fn)(void* userdata, void* pointer, size_t page_count, int executable);

/* result is initially NULL. A successful hook transfers one owning result. */
typedef li_status (*li_import_fn)(li_vm* vm, const char* importer, size_t importer_size, const char* name, size_t name_size, void* userdata, li_value** result);

typedef void (*li_error_fn)(void* userdata, li_status status, const char* message, size_t message_size);

/*
 * Arguments are borrowed for the duration of the callback. On success, the
 * callback may transfer one owning value through result. Exceptions thrown by
 * a C++ implementation of this callback are contained by the C ABI.
 */
typedef li_status (*li_native_fn)(li_vm* vm, const li_value* const* args, size_t argument_count, void* userdata, li_value** result);

typedef void (*li_userdata_destroy_fn)(void* pointer, void* userdata);

typedef struct li_vm_options {
	size_t          struct_size;
	li_allocator_fn allocator;
	void*           allocator_userdata;
	li_import_fn    import;
	void*           import_userdata;
	li_error_fn     error;
	void*           error_userdata;
} li_vm_options;

LI_API const char* li_status_name(li_status status);
LI_API void        li_vm_options_init(li_vm_options* options);
LI_API li_status   li_vm_create(const li_vm_options* options, li_vm** result);

/*
 * VM entry is serialized across threads. The caller must synchronize close
 * against new entries and must not release a value while another thread uses
 * that same handle.
 *
 * Close consumes the host VM pointer and prevents further script operations.
 * The pointer must not be used again. Outstanding li_value handles keep the
 * underlying heap alive until the last handle is released.
 */
LI_API li_status li_vm_close(li_vm* vm);

/* The returned message is valid until the next error reported by this VM. */
LI_API li_status li_vm_last_error(const li_vm* vm, const char** data, size_t* size);

/* Sets the current script exception; intended for native/import callbacks. */
LI_API li_status li_vm_set_error(li_vm* vm, const char* message, size_t message_size);

LI_API li_status li_vm_load(li_vm* vm, const char* source, size_t source_size, const char* source_name, size_t source_name_size, li_value** result);
LI_API li_status li_vm_execute(li_vm* vm, const char* source, size_t source_size, const char* source_name, size_t source_name_size, li_value** result);
/*
 * Cross-VM call targets and arguments are accepted only when their values are
 * explicitly shared. Values owned by vm retain the existing unrestricted
 * same-VM behavior.
 */
LI_API li_status li_vm_call(li_vm* vm, const li_value* function, const li_value* const* arguments, size_t argument_count, li_value** result);
/*
 * Registration borrows userdata; the caller controls its lifetime. An
 * unqualified name is a script global; dotted names use explicit modules.
 */
LI_API li_status li_vm_register_native(li_vm* vm, const char* name, size_t name_size, li_native_fn callback, void* userdata);

LI_API li_status li_value_create_nil(li_vm* vm, li_value** result);
LI_API li_status li_value_create_bool(li_vm* vm, int value, li_value** result);
LI_API li_status li_value_create_number(li_vm* vm, double value, li_value** result);
LI_API li_status li_value_create_string(li_vm* vm, const char* data, size_t size, li_value** result);

/* copy creates a new owning persistent handle for the same VM. */
LI_API li_status li_value_copy(const li_value* value, li_value** result);

/*
 * share creates an explicitly shared copy of a supported heap value in
 * destination and returns one destination-owned handle. copy_to safely
 * transfers immediates and shared
 * values, copies immutable strings, and rejects private mutable cross-VM
 * values. A same-VM copy_to is an ordinary handle copy.
 *
 * destination must be open. source is borrowed and may belong to a logically
 * closed VM while its handle keeps that VM physically alive. On success the
 * caller must release *result exactly once; both functions set it to NULL on
 * failure.
 */
LI_API li_status li_value_share(li_vm* destination, const li_value* source, li_value** result);
LI_API li_status li_value_copy_to(li_vm* destination, const li_value* source, li_value** result);

LI_API void      li_value_release(li_value* value);
LI_API li_kind   li_value_get_kind(const li_value* value);
LI_API li_status li_value_get_bool(const li_value* value, int* result);
LI_API li_status li_value_get_number(const li_value* value, double* result);
LI_API li_status li_value_get_string(const li_value* value, const char** data, size_t* size);

/*
 * Type names are process-global: registering the same bytes returns the same id.
 * A non-NULL userdata destructor runs exactly once when the last script/host
 * reference is released, which may be after logical VM close.
 */
LI_API li_status li_userdata_type_register(const char* name, size_t name_size, li_type_id* result);
LI_API li_status li_value_create_userdata(li_vm* vm, li_type_id type, void* pointer, li_userdata_destroy_fn destroy, void* destroy_userdata, li_value** result);
LI_API li_status li_value_get_userdata_type(const li_value* value, li_type_id* result);
LI_API li_status li_value_get_userdata(const li_value* value, li_type_id type, void** result);

#ifdef __cplusplus
}
#endif

#endif
