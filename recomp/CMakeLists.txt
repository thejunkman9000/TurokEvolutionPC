cmake_minimum_required(VERSION 3.20)
project(TurokRecomp C)

# Regenerate with: python -I tools/recomp/ppcrecomp.py extracted/main.dol \
#   decomp/config/GTKE51/symbols.txt recomp/hle_overrides.txt recomp/generated
file(GLOB GENERATED_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
file(GLOB RUNTIME_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.c)

# The recompiled game + runtime, loaded by the MonoGame host.
add_library(turok_core SHARED ${RUNTIME_SRC} ${GENERATED_SRC})
target_include_directories(turok_core PUBLIC runtime PRIVATE generated)
target_compile_definitions(turok_core PRIVATE _CRT_SECURE_NO_WARNINGS TUROK_CORE_BUILD)

# Headless runner for debugging.
add_executable(turok_run runner/runner.c)
target_link_libraries(turok_run PRIVATE turok_core)

if(MSVC)
  # Generated code: optimise, but keep compile times sane on 600k-instruction output.
  set_source_files_properties(${GENERATED_SRC} PROPERTIES COMPILE_OPTIONS "/O2;/Ob1;/W1;/bigobj")
  target_compile_options(turok_core PRIVATE /Zi /fp:precise)
  target_link_options(turok_core PRIVATE /DEBUG)
  target_link_options(turok_run PRIVATE /DEBUG)
endif()
