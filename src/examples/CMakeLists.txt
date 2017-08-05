aux_source_directory(. SRCS)

add_executable(cech_examples ${SRCS})

target_link_libraries(cech_examples cehc common curl pthread)
