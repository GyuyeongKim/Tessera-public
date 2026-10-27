cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME tessera)
find_package(OpenMP REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

set(T ${CMAKE_CURRENT_SOURCE_DIR}/../core)
include_directories(src/include ${T})
if(CMAKE_OSX_ARCHITECTURES)
  set(TESSERA_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
  set(TESSERA_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(TESSERA_ARCH MATCHES "^(x86_64|AMD64|amd64)$")
  set_source_files_properties(${T}/tessera_amx.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=sapphirerapids")
  set_source_files_properties(${T}/tessera_vnni.cpp PROPERTIES COMPILE_OPTIONS
      "-O3;-march=x86-64-v4;-mavx512vnni;-mavx512vbmi;-mgfni;-mbmi2")
  set_source_files_properties(${T}/tessera_avx512.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=x86-64-v4")
  set_source_files_properties(${T}/tessera_avx2.cpp ${T}/common.cpp ${T}/query.cpp src/tessera_operator.cpp
      PROPERTIES COMPILE_OPTIONS "-O3;-march=x86-64-v3")
  set_source_files_properties(${T}/dispatch.cpp PROPERTIES COMPILE_OPTIONS "-O3")
  set(EXTENSION_SOURCES src/tessera_extension.cpp ${T}/dispatch.cpp ${T}/common.cpp ${T}/query.cpp
      src/tessera_operator.cpp ${T}/tessera_avx2.cpp ${T}/tessera_avx512.cpp ${T}/tessera_vnni.cpp ${T}/tessera_amx.cpp)
else()
  set_source_files_properties(${T}/tessera_portable.cpp ${T}/dispatch.cpp ${T}/common.cpp ${T}/query.cpp
      src/tessera_operator.cpp PROPERTIES COMPILE_OPTIONS "-O3")
  set(EXTENSION_SOURCES src/tessera_extension.cpp ${T}/dispatch.cpp ${T}/common.cpp ${T}/query.cpp
      src/tessera_operator.cpp ${T}/tessera_portable.cpp)
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
target_link_libraries(${EXTENSION_NAME} OpenMP::OpenMP_CXX ${CMAKE_DL_LIBS})
set(TESSERA_OMP_STATIC "")
if(OpenMP_libomp_LIBRARY)
  get_filename_component(TESSERA_OMP_DIR "${OpenMP_libomp_LIBRARY}" DIRECTORY)
  if(EXISTS "${TESSERA_OMP_DIR}/libomp.a")
    set(TESSERA_OMP_STATIC "${TESSERA_OMP_DIR}/libomp.a")
  endif()
endif()
if(TESSERA_OMP_STATIC)
  target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE "SHELL:${OpenMP_CXX_FLAGS}")
  target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${OpenMP_CXX_INCLUDE_DIRS})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} "${TESSERA_OMP_STATIC}" ${CMAKE_DL_LIBS})
  message(STATUS "tessera: the loadable extension links ${TESSERA_OMP_STATIC}")
else()
  target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenMP::OpenMP_CXX ${CMAKE_DL_LIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
