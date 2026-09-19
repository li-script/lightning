#include <lightning.h>

#include <stdio.h>
#include <string.h>

struct observer_context {
	const li_value* reader;
	li_value*       retained;
};

static int check_status(li_vm* vm, li_status actual, li_status expected, const char* operation) {
	const char* message = NULL;
	size_t      size    = 0;
	if (actual == expected)
		return 1;
	fprintf(stderr, "%s: expected %s, got %s", operation, li_status_name(expected), li_status_name(actual));
	if (vm && li_vm_last_error(vm, &message, &size) == LI_STATUS_OK && size)
		fprintf(stderr, ": %.*s", (int) size, message);
	fputc('\n', stderr);
	return 0;
}

static int check_number(const li_value* value, double expected, const char* operation) {
	double actual = 0;
	if (li_value_get_number(value, &actual) == LI_STATUS_OK && actual == expected)
		return 1;
	fprintf(stderr, "%s: expected %.17g\n", operation, expected);
	return 0;
}

static li_status observe_shared(li_vm* vm, const li_value* const* args, size_t argument_count, void* userdata, li_value** result) {
	struct observer_context* context = (struct observer_context*) userdata;
	li_status                status;

	if (argument_count != 1 || li_value_get_kind(args[0]) != LI_KIND_TABLE)
		return li_vm_set_error(vm, "observe_shared expects one table", 32);
	if (context->retained)
		return li_vm_set_error(vm, "observe_shared called twice", 27);
	status = li_value_copy_to(vm, args[0], &context->retained);
	if (status != LI_STATUS_OK)
		return status;
	return li_vm_call(vm, context->reader, args, 1, result);
}

int main(void) {
	static const char shared_source[] =
		 "import shared\n"
		 "shared.create({counter: 40, label: \"shared graph\"})";
	static const char private_source[] =
		 "const value = {counter: 7}\n"
		 "value";
	static const char reader_source[] =
		 "fn read_counter(value) { value.counter }\n"
		 "read_counter";
	static const char observer_source[] =
		 "fn invoke_observe(value) { observe_shared(value) }\n"
		 "invoke_observe";
	static const char mutator_source[] =
		 "fn add_two(value) {\n"
		 "   value.counter += 2\n"
		 "   value.counter\n"
		 "}\n"
		 "add_two";
	static const char string_source[] = "\"copied after close\"";
	static const char number_source[] = "73";
	static const char function_source[] =
		 "fn shared_answer() { 91 }\n"
		 "shared_answer";
	static const char expected_string[] = "copied after close";

	li_vm*                  origin             = NULL;
	li_vm*                  destination        = NULL;
	li_value*               shared_origin      = NULL;
	li_value*               shared_destination = NULL;
	li_value*               private_origin     = NULL;
	li_value*               promoted           = NULL;
	li_value*               reader             = NULL;
	li_value*               observer_function  = NULL;
	li_value*               mutator            = NULL;
	li_value*               string_origin      = NULL;
	li_value*               string_destination = NULL;
	li_value*               number_origin      = NULL;
	li_value*               number_destination = NULL;
	li_value*               function_origin    = NULL;
	li_value*               shared_function    = NULL;
	li_value*               result             = NULL;
	const li_value*         arguments[1];
	struct observer_context observer  = {NULL, NULL};
	const char*             text      = NULL;
	size_t                  text_size = 0;
	int                     ok        = 0;

	if (!check_status(NULL, li_vm_create(NULL, &origin), LI_STATUS_OK, "create origin") ||
		 !check_status(NULL, li_vm_create(NULL, &destination), LI_STATUS_OK, "create destination"))
		goto cleanup;
	if (!check_status(origin, li_vm_execute(origin, shared_source, sizeof(shared_source) - 1, "shared-origin", 13, &shared_origin), LI_STATUS_OK,
			  "create script shared graph") ||
		 li_value_get_kind(shared_origin) != LI_KIND_TABLE)
		goto cleanup;
	if (!check_status(
			  destination, li_vm_execute(destination, reader_source, sizeof(reader_source) - 1, "shared-reader", 13, &reader), LI_STATUS_OK, "load reader"))
		goto cleanup;

	arguments[0] = shared_origin;
	if (!check_status(destination, li_vm_call(destination, reader, arguments, 1, &result), LI_STATUS_OK, "call with foreign shared argument") ||
		 !check_number(result, 40, "read foreign shared argument"))
		goto cleanup;
	li_value_release(result);
	result = NULL;

	if (!check_status(origin, li_vm_execute(origin, private_source, sizeof(private_source) - 1, "private-origin", 14, &private_origin), LI_STATUS_OK,
			  "create private graph"))
		goto cleanup;
	arguments[0] = private_origin;
	if (!check_status(destination, li_vm_call(destination, reader, arguments, 1, &result), LI_STATUS_INVALID_ARGUMENT, "reject private cross-VM call") ||
		 result != NULL)
		goto cleanup;
	if (!check_status(destination, li_value_copy_to(destination, private_origin, &result), LI_STATUS_TYPE_ERROR, "reject private cross-VM copy") ||
		 result != NULL)
		goto cleanup;

	if (!check_status(destination, li_value_copy_to(destination, shared_origin, &shared_destination), LI_STATUS_OK, "copy shared graph to destination"))
		goto cleanup;
	observer.reader = reader;
	if (!check_status(
			  destination, li_vm_register_native(destination, "observe_shared", 14, observe_shared, &observer), LI_STATUS_OK, "register shared observer") ||
		 !check_status(destination, li_vm_execute(destination, observer_source, sizeof(observer_source) - 1, "shared-observer", 15, &observer_function),
			  LI_STATUS_OK, "load shared observer") ||
		 !check_status(destination, li_vm_execute(destination, mutator_source, sizeof(mutator_source) - 1, "shared-mutator", 14, &mutator), LI_STATUS_OK,
			  "load shared mutator"))
		goto cleanup;

	arguments[0] = shared_origin;
	if (!check_status(destination, li_vm_call(destination, observer_function, arguments, 1, &result), LI_STATUS_OK, "native receives shared graph") ||
		 !check_number(result, 40, "native reads shared graph") || observer.retained == NULL)
		goto cleanup;
	li_value_release(result);
	result = NULL;

	arguments[0] = observer.retained;
	if (!check_status(destination, li_vm_call(destination, mutator, arguments, 1, &result), LI_STATUS_OK, "mutate retained shared graph") ||
		 !check_number(result, 42, "exact shared mutation result"))
		goto cleanup;
	li_value_release(result);
	result       = NULL;
	arguments[0] = shared_destination;
	if (!check_status(destination, li_vm_call(destination, reader, arguments, 1, &result), LI_STATUS_OK, "read same shared graph") ||
		 !check_number(result, 42, "same shared graph observes mutation"))
		goto cleanup;
	li_value_release(result);
	result = NULL;

	if (!check_status(origin, li_vm_execute(origin, string_source, sizeof(string_source) - 1, "private-string", 14, &string_origin), LI_STATUS_OK,
			  "create private string") ||
		 !check_status(
			  origin, li_vm_execute(origin, number_source, sizeof(number_source) - 1, "immediate", 9, &number_origin), LI_STATUS_OK, "create immediate") ||
		 !check_status(origin, li_vm_execute(origin, function_source, sizeof(function_source) - 1, "private-function", 16, &function_origin), LI_STATUS_OK,
			  "create private function"))
		goto cleanup;
	if (!check_status(destination, li_vm_call(destination, function_origin, NULL, 0, &result), LI_STATUS_TYPE_ERROR, "reject private cross-VM function") ||
		 result != NULL)
		goto cleanup;

	li_value_release(shared_origin);
	shared_origin = NULL;
	if (!check_status(origin, li_vm_close(origin), LI_STATUS_OK, "close origin with source and destination values live"))
		goto cleanup;
	origin = NULL;

	if (!check_status(destination, li_value_share(destination, private_origin, &promoted), LI_STATUS_OK, "share private graph after source close"))
		goto cleanup;
	arguments[0] = promoted;
	if (!check_status(destination, li_vm_call(destination, reader, arguments, 1, &result), LI_STATUS_OK, "read explicitly shared graph") ||
		 !check_number(result, 7, "explicitly shared graph value"))
		goto cleanup;
	li_value_release(result);
	result = NULL;
	if (!check_status(destination, li_value_copy_to(destination, string_origin, &string_destination), LI_STATUS_OK, "copy private string after source close") ||
		 !check_status(destination, li_value_copy_to(destination, number_origin, &number_destination), LI_STATUS_OK, "copy immediate after source close") ||
		 !check_number(number_destination, 73, "copied immediate") ||
		 !check_status(destination, li_value_share(destination, function_origin, &shared_function), LI_STATUS_OK, "share function after source close") ||
		 !check_status(destination, li_vm_call(destination, shared_function, NULL, 0, &result), LI_STATUS_OK, "call shared function in destination") ||
		 !check_number(result, 91, "shared function runs in destination"))
		goto cleanup;
	li_value_release(result);
	result = NULL;
	li_value_release(private_origin);
	private_origin = NULL;
	li_value_release(string_origin);
	string_origin = NULL;
	li_value_release(number_origin);
	number_origin = NULL;
	li_value_release(function_origin);
	function_origin = NULL;

	arguments[0] = observer.retained;
	if (!check_status(destination, li_vm_call(destination, reader, arguments, 1, &result), LI_STATUS_OK, "read shared graph after origin close") ||
		 !check_number(result, 42, "shared graph survives origin close"))
		goto cleanup;
	li_value_release(result);
	result = NULL;
	if (!check_status(destination, li_value_get_string(string_destination, &text, &text_size), LI_STATUS_OK, "read copied string after origin close") ||
		 text_size != sizeof(expected_string) - 1 || memcmp(text, expected_string, text_size) != 0)
		goto cleanup;

	if (!check_status(destination, li_vm_close(destination), LI_STATUS_OK, "close destination with handles live"))
		goto cleanup;
	destination = NULL;
	if (li_value_get_kind(shared_destination) != LI_KIND_TABLE ||
		 !check_status(NULL, li_value_get_string(string_destination, &text, &text_size), LI_STATUS_OK, "read copied string after destination close"))
		goto cleanup;
	ok = 1;

cleanup:
	li_value_release(result);
	li_value_release(shared_function);
	li_value_release(function_origin);
	li_value_release(number_destination);
	li_value_release(number_origin);
	li_value_release(string_destination);
	li_value_release(string_origin);
	li_value_release(mutator);
	li_value_release(observer_function);
	li_value_release(observer.retained);
	li_value_release(reader);
	li_value_release(promoted);
	li_value_release(private_origin);
	li_value_release(shared_destination);
	li_value_release(shared_origin);
	if (destination)
		li_vm_close(destination);
	if (origin)
		li_vm_close(origin);
	return ok ? 0 : 1;
}
