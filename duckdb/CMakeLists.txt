cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME tessera)
find_package(OpenMP REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

set(T ${CMAKE_CURRENT_SOURCE_DIR}/../core)
set(TESSERA_CORE ${T}/tessera_amx.cpp ${T}/tessera_vnni.cpp ${T}/tessera_avx512.cpp ${T}/tessera_avx2.cpp
    ${T}/dispatch.cpp ${T}/common.cpp ${T}/query.cpp)
include_directories(src/include ${T})
set_source_files_properties(${T}/tessera_amx.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=sapphirerapids;-fopenmp")
set_source_files_properties(${T}/tessera_vnni.cpp PROPERTIES COMPILE_OPTIONS
    "-O3;-march=x86-64-v4;-mavx512vnni;-mavx512vbmi;-mgfni;-mbmi2;-fopenmp")
set_source_files_properties(${T}/tessera_avx512.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=x86-64-v4;-fopenmp")
set_source_files_properties(${T}/tessera_avx2.cpp ${T}/dispatch.cpp ${T}/common.cpp ${T}/query.cpp
    src/tessera_operator.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=x86-64-v3;-fopenmp")

set(EXTENSION_SOURCES src/tessera_extension.cpp src/tessera_operator.cpp ${TESSERA_CORE})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
target_link_libraries(${EXTENSION_NAME} OpenMP::OpenMP_CXX)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenMP::OpenMP_CXX)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
