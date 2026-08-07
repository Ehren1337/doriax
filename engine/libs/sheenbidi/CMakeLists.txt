# SheenBidi 3.0.0 - https://github.com/Tehreer/SheenBidi/
#
# Unicode Bidirectional Algorithm, used to split a paragraph into runs in visual
# order before HarfBuzz shapes them. Built from the SheenBidi.c unity source.

add_library(

    sheenbidi

    STATIC

    Source/SheenBidi.c

)

target_include_directories(sheenbidi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Headers")
target_include_directories(sheenbidi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(sheenbidi PRIVATE -pthread)
endif()
