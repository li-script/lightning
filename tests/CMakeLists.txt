if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)

    add_executable(fastlock_test
        fastlock.cpp
        fastlock_recursive.cpp
    )
    target_compile_features(fastlock_test PRIVATE cxx_std_20)
    target_include_directories(fastlock_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(fastlock_test PRIVATE Threads::Threads)

    add_test(
        NAME util.fastlock
        COMMAND "$<TARGET_FILE:fastlock_test>"
    )
    set_tests_properties(
        util.fastlock
        PROPERTIES
            LABELS util
            TIMEOUT 30
    )

    function(add_lightning_probe TARGET SOURCE TEST_NAME LABEL)
        add_executable("${TARGET}" "${SOURCE}")
        target_compile_features("${TARGET}" PRIVATE cxx_std_20)
        target_link_libraries("${TARGET}" PRIVATE libli)

        add_test(
            NAME "${TEST_NAME}"
            COMMAND "$<TARGET_FILE:${TARGET}>"
        )
        set_tests_properties(
            "${TEST_NAME}"
            PROPERTIES
                LABELS "${LABEL}"
                TIMEOUT 60
        )
    endfunction()

    add_lightning_probe(
        code_memory_test
        code-memory.cpp
        util.code-memory
        util
    )
    add_lightning_probe(
        native_stack_test
        native-stack.cpp
        util.native-stack
        util
    )
    if(LI_CONTEXT_X86_64 OR LI_CONTEXT_A64)
        add_lightning_probe(
            context_switch_test
            context-switch.cpp
            util.context-switch
            util
        )
        if(LI_CONTEXT_X86_64_MASM)
            target_sources(context_switch_test PRIVATE context-switch-x86-64.asm)
        endif()
    endif()
    add_lightning_probe(
        a64_encoder_test
        a64-encoder.cpp
        util.a64-encoder
        util
    )
    add_lightning_probe(
        a64_backend_test
        a64-backend.cpp
        backend.a64
        backend
    )
    add_lightning_probe(
        ownership_runtime_test
        ownership-runtime.cpp
        ownership.runtime
        ownership
    )
    add_lightning_probe(
        lexer_lifetime_test
        lexer-lifetime.cpp
        ownership.lexer-lifetime
        ownership
    )
    add_lightning_probe(
        typespec_test
        typespec.cpp
        strict.typespec
        strict
    )

    add_lightning_probe(
        parser_fuzz_test
        parser-fuzz.cpp
        hardening.parser
        hardening
    )
    set_tests_properties(
        hardening.parser
        PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_lightning_probe(
        allocation_failure_test
        allocation-failure.cpp
        hardening.allocation-failure
        hardening
    )

    if(LI_JIT)
        add_lightning_probe(
            dominance_test
            dominance.cpp
            ir.dominance
            ir
        )
        add_lightning_probe(
            ir_verify_test
            ir-verify.cpp
            ir.verify
            ir
        )
    endif()

    add_lightning_probe(
        shared_strings_test
        shared-strings.cpp
        shared.strings
        shared
    )
    add_lightning_probe(
        shared_runtime_test
        shared-runtime.cpp
        shared.runtime
        shared
    )
    add_lightning_probe(
        struct_runtime_test
        struct-runtime.cpp
        runtime.struct
        runtime
    )
    add_executable(shared_embedding_test shared-embedding.c)
    target_compile_features(shared_embedding_test PRIVATE c_std_11)
    target_link_libraries(shared_embedding_test PRIVATE libli)
    set_target_properties(shared_embedding_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME shared.c-api COMMAND "$<TARGET_FILE:shared_embedding_test>")
    set_tests_properties(shared.c-api PROPERTIES LABELS "shared;embed" TIMEOUT 60)

    add_executable(embed_smoke_test embed-smoke.c)
    target_compile_features(embed_smoke_test PRIVATE c_std_11)
    target_link_libraries(embed_smoke_test PRIVATE libli)
    set_target_properties(embed_smoke_test PROPERTIES LINKER_LANGUAGE CXX)

    add_test(
        NAME embed.c-api
        COMMAND "$<TARGET_FILE:embed_smoke_test>"
    )
    set_tests_properties(
        embed.c-api
        PROPERTIES
            LABELS embed
            TIMEOUT 60
    )
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.li")
foreach(SOURCE IN LISTS SOURCES)
    get_filename_component(NAME "${SOURCE}" NAME_WE)

    add_test(
        NAME "interpreter.${NAME}"
        COMMAND "$<TARGET_FILE:li>" --jit=off "${SOURCE}"
    )
    set_tests_properties(
        "interpreter.${NAME}"
        PROPERTIES
            LABELS interpreter
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(LI_JIT)
        add_test(
            NAME "jit.${NAME}"
            COMMAND "$<TARGET_FILE:li>" --jit "${SOURCE}"
        )
        set_tests_properties(
            "jit.${NAME}"
            PROPERTIES
                LABELS jit
                TIMEOUT 60
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    endif()
endforeach()

if(LI_JIT)
    add_test(
        NAME tiering.osr
        COMMAND "$<TARGET_FILE:li>" --jit=auto "${CMAKE_CURRENT_SOURCE_DIR}/deopt.li"
    )
    set_tests_properties(
        tiering.osr
        PROPERTIES
            LABELS "jit;tiering"
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            PASS_REGULAR_EXPRESSION "in-invocation osr transfer observed"
    )
    add_test(
        NAME tiering.auto
        COMMAND "$<TARGET_FILE:li>" --jit=auto "${CMAKE_CURRENT_SOURCE_DIR}/tiering.li"
    )
    set_tests_properties(
        tiering.auto
        PROPERTIES
            LABELS "jit;tiering"
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            PASS_REGULAR_EXPRESSION "automatic tier transitions observed"
    )
    add_test(
        NAME tiering.required-runtime
        COMMAND "$<TARGET_FILE:li>" --jit "${CMAKE_CURRENT_SOURCE_DIR}/tiering.li"
    )
    set_tests_properties(
        tiering.required-runtime
        PROPERTIES
            LABELS "jit;tiering"
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            PASS_REGULAR_EXPRESSION "required runtime prototype compilation observed"
    )

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(LI_NATIVE_FUZZ_CASES 100 CACHE STRING "Number of bounded native differential fuzz cases")
    if(NOT LI_NATIVE_FUZZ_CASES MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "LI_NATIVE_FUZZ_CASES must be a positive integer")
    endif()

    add_test(
        NAME differential
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential.py" "$<TARGET_FILE:li>"
    )
    set_tests_properties(
        differential
        PROPERTIES
            LABELS differential
            TIMEOUT 7200
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_test(
        NAME differential.fuzz
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/fuzz-differential.py"
            "$<TARGET_FILE:li>"
            --cases "${LI_NATIVE_FUZZ_CASES}"
            --output-dir "${CMAKE_CURRENT_BINARY_DIR}/fuzz-failures"
    )
    set_tests_properties(
        differential.fuzz
        PROPERTIES
            LABELS "differential;fuzz"
            TIMEOUT 1800
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
