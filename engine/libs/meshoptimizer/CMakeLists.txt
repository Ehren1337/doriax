file(GLOB MESHOPTIMIZER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_library(
    meshoptimizer

    STATIC

    ${MESHOPTIMIZER_SRCS}
)
