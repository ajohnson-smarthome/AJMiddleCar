file(GLOB U8G2_SRCS "csrc/*.c")
idf_component_register(SRCS ${U8G2_SRCS} INCLUDE_DIRS "csrc")
