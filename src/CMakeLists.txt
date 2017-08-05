set(LINK_DIRS
        ${PROJECT_BINARY_DIR}/src/common
        ${PROJECT_BINARY_DIR}/src/cehc
        )
link_directories(${LINK_DIRS})

add_subdirectory(./common)
add_subdirectory(./cehc)
add_subdirectory(./examples)
