#include <lightning.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
	#include <windows.h>
#else
	#include <sys/mman.h>
	#if !defined(MAP_ANONYMOUS)
		#define MAP_ANONYMOUS MAP_ANON
	#endif
#endif

static li_type_id box_type;
static int        destroyed_boxes;
static int        imported_modules;
static int        reported_errors;
static size_t     page_allocations;
static size_t     page_frees;

static void* allocate_pages(void* userdata, void* pointer, size_t page_count, int executable) {
	size_t byte_count;
	(void) userdata;
	if (pointer) {
#if defined(_WIN32)
		VirtualFree(pointer, 0, MEM_RELEASE);
#else
		munmap(pointer, page_count * 4096);
#endif
		++page_frees;
		return NULL;
	}
	if (page_count == 0 || page_count > SIZE_MAX / 4096)
		return NULL;
	byte_count = page_count * 4096;
#if defined(_WIN32)
	pointer = VirtualAlloc(NULL, byte_count, MEM_COMMIT | MEM_RESERVE, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#else
	pointer = mmap(NULL, byte_count, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pointer == MAP_FAILED)
		pointer = NULL;
#endif
	if (pointer)
		++page_allocations;
	return pointer;
}

static void destroy_box(void* pointer, void* userdata) {
	(void) userdata;
	free(pointer);
	++destroyed_boxes;
}

static li_status make_box(li_vm* vm, const li_value* const* args, size_t argument_count, void* userdata, li_value** result) {
	double number = 0;
	int*   box;
	(void) userdata;
	if (argument_count != 1 || li_value_get_number(args[0], &number) != LI_STATUS_OK)
		return li_vm_set_error(vm, "make_box expects one number", 27);
	box = (int*) malloc(sizeof(*box));
	if (!box)
		return LI_STATUS_OUT_OF_MEMORY;
	*box = (int) number;
	if (li_value_create_userdata(vm, box_type, box, destroy_box, NULL, result) != LI_STATUS_OK) {
		free(box);
		return LI_STATUS_OUT_OF_MEMORY;
	}
	return LI_STATUS_OK;
}

static li_status fail_native(li_vm* vm, const li_value* const* args, size_t argument_count, void* userdata, li_value** result) {
	(void) args;
	(void) argument_count;
	(void) userdata;
	(void) result;
	return li_vm_set_error(vm, "failure from native", 19);
}

static li_status import_virtual(li_vm* vm, const char* importer, size_t importer_size, const char* name, size_t name_size, void* userdata, li_value** result) {
	static const char module[] = "export const value = 8\n$E";
	(void) importer;
	(void) importer_size;
	(void) userdata;
	if (name_size != 7 || memcmp(name, "virtual", 7) != 0)
		return li_vm_set_error(vm, "unknown virtual module", 22);
	++imported_modules;
	return li_vm_execute(vm, module, sizeof(module) - 1, "virtual", 7, result);
}

static char last_error[256];

static void report_error(void* userdata, li_status status, const char* message, size_t message_size) {
	(void) userdata;
	(void) status;
	if (message_size >= sizeof(last_error))
		message_size = sizeof(last_error) - 1;
	memcpy(last_error, message, message_size);
	last_error[message_size] = 0;
	++reported_errors;
}

static int check(li_status status, li_status expected, const char* operation) {
	if (status == expected)
		return 1;
	fprintf(stderr, "%s: expected %s, got %s (%s)\n", operation, li_status_name(expected), li_status_name(status), last_error);
	return 0;
}

static int execute_unaligned_number(li_vm* vm, const unsigned char* source, size_t source_size, const char* operation) {
	li_value* value  = NULL;
	double    number = 0;
	int       ok     = check(li_vm_execute(vm, (const char*) source + 1, source_size - 1, "unaligned", 9, &value), LI_STATUS_OK, operation);
	if (ok)
		ok = check(li_value_get_number(value, &number), LI_STATUS_OK, operation) && number == 40;
	li_value_release(value);
	return ok;
}

int main(void) {
	li_vm_options options;
	li_vm*        vm            = NULL;
	li_value*     loaded        = NULL;
	li_value*     retained      = NULL;
	li_value*     retained_copy = NULL;
	li_value*     ignored       = NULL;
	void*         pointer       = NULL;
	const char    program[]     = "import virtual as v\nmake_box(v.value + 34)";
	static const union {
		uint32_t      alignment;
		unsigned char bytes[6];
	} utf8_source = {.bytes = {0, 0xEF, 0xBB, 0xBF, '4', '0'}};
	static const union {
		uint32_t      alignment;
		unsigned char bytes[7];
	} utf16le_source = {.bytes = {0, 0xFF, 0xFE, '4', 0, '0', 0}};
	static const union {
		uint32_t      alignment;
		unsigned char bytes[7];
	} utf16be_source = {.bytes = {0, 0xFE, 0xFF, 0, '4', 0, '0'}};
	static const union {
		uint32_t      alignment;
		unsigned char bytes[13];
	} utf32le_source = {.bytes = {0, 0xFF, 0xFE, 0, 0, '4', 0, 0, 0, '0', 0, 0, 0}};
	static const union {
		uint32_t      alignment;
		unsigned char bytes[13];
	} utf32be_source = {.bytes = {0, 0, 0, 0xFE, 0xFF, 0, 0, 0, '4', 0, 0, 0, '0'}};

	if (!check(li_userdata_type_register("embed.box", 9, &box_type), LI_STATUS_OK, "register userdata type"))
		return 1;
	li_vm_options_init(&options);
	options.allocator = allocate_pages;
	options.import    = import_virtual;
	options.error     = report_error;
	if (!check(li_vm_create(&options, &vm), LI_STATUS_OK, "create VM"))
		return 1;
	if (!check(li_vm_register_native(vm, "make_box", 8, make_box, NULL), LI_STATUS_OK, "register make_box") ||
		 !check(li_vm_register_native(vm, "fail", 4, fail_native, NULL), LI_STATUS_OK, "register fail"))
		return 1;

	if (!execute_unaligned_number(vm, utf8_source.bytes, sizeof(utf8_source.bytes), "execute unaligned UTF-8") ||
		 !execute_unaligned_number(vm, utf16le_source.bytes, sizeof(utf16le_source.bytes), "execute unaligned UTF-16LE") ||
		 !execute_unaligned_number(vm, utf16be_source.bytes, sizeof(utf16be_source.bytes), "execute unaligned UTF-16BE") ||
		 !execute_unaligned_number(vm, utf32le_source.bytes, sizeof(utf32le_source.bytes), "execute unaligned UTF-32LE") ||
		 !execute_unaligned_number(vm, utf32be_source.bytes, sizeof(utf32be_source.bytes), "execute unaligned UTF-32BE"))
		return 1;

	if (!check(li_vm_load(vm, program, sizeof(program) - 1, "embed-smoke", 11, &loaded), LI_STATUS_OK, "load") ||
		 !check(li_vm_call(vm, loaded, NULL, 0, &retained), LI_STATUS_OK, "call"))
		return 1;
	li_value_release(loaded);
	if (imported_modules != 1 || li_value_get_kind(retained) != LI_KIND_USERDATA ||
		 !check(li_value_get_userdata(retained, box_type, &pointer), LI_STATUS_OK, "read userdata") || *(int*) pointer != 42)
		return 1;
	if (!check(li_value_copy(retained, &retained_copy), LI_STATUS_OK, "copy retained value"))
		return 1;
	li_value_release(retained);
	retained = retained_copy;

	if (!check(li_vm_execute(vm, "fail()", 6, "embed-smoke", 11, &ignored), LI_STATUS_SCRIPT_ERROR, "native failure") || ignored != NULL || reported_errors == 0)
		return 1;

	if (!check(li_vm_close(vm), LI_STATUS_OK, "close VM"))
		return 1;
	if (!check(li_value_get_userdata(retained, box_type, &pointer), LI_STATUS_OK, "read userdata after close") || *(int*) pointer != 42)
		return 1;
	if (destroyed_boxes != 0)
		return 1;
	li_value_release(retained);
	if (destroyed_boxes != 1 || page_allocations == 0 || page_allocations != page_frees)
		return 1;
	return 0;
}
