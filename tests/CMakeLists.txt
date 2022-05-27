include(CTest)
enable_testing()

file(GLOB SOURCES *.li)
foreach(SOURCE ${SOURCES})
    get_filename_component(NAME "${SOURCE}" NAME_WE)
    add_test(
        NAME
            "${NAME}"
        COMMAND
            "$<TARGET_FILE:li>" "${SOURCE}"
)
endforeach(SOURCE ${SOURCES})
