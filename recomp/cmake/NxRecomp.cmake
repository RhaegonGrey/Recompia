# SPDX-License-Identifier: GPL-3.0-or-later
#
# nxrecomp_add_module(<target>
#     INPUT <module file>          NSO or static ELF (see --kind)
#     NAME <module name>           default: main        (the generated descriptor is `nxmod_<NAME>`)
#     [BASE <0x...>]               guest address the module is mapped at (default 0x7100000000)
#     [INDEX <n>]                  NxModule.index / import-id module number (default: position in the game config's module list)
#     [GAME <name>]                recomp/games/<name>/config.py (default: generic)
#     [KIND nso|nx_elf|static_elf] default: guessed from the file's magic
#     [SHARDS <n>]                 exactly n translation units c_000.cpp ... (default 16)
#     [JOBS <n>]                   lifter processes (default: all cores)
#     [PATCHES <file>]             instruction patch list
#     [SITES <file>]               site list: patches, inline callouts, function hooks (nxrecomp/sites.py load_sites)
#     [OUTDIR <dir>])              where the generated sources go (default: <build>/gen/<target>)
#
# Lifts the module with `python -m nxrecomp lift` (a build step, re-run when the input or the lifter changes) and builds
# the result as the static library <target>. Link it into the runtime: it provides `nxmod_<NAME>` (ctx.h: NxModule) and
# expects the runtime to provide the functions ctx.h declares as runtime-provided. Public include directories: recomp/include
# (nxrecomp/ctx.h) and the generated folder (funcs.h).
#
# Flags: C++20 plus the table in nxrecomp/compile_flags.py (shared with the player build; read through `python -m nxrecomp
# build-info` at configure time): no exceptions/RTTI, -fno-strict-aliasing and -ffp-contract=off (lifted FP must not be fused
# or reordered), -mcx16 (128-bit exclusives are cmpxchg16b). Functions longer than the game config's optnone_insns are
# emitted with NX_OPTNONE by the lifter itself (bounded compile time); cmake/nx_cc_launcher.py adds the time-limit fallback.
#
# nxrecomp_build_info(<game> <prefix>) reads the game config's module table (recomp/games/<game>/config.py, the same table
# the player build uses): <prefix>_MODULES = module names in NxModule.index order, and for each module m
# <prefix>_<m>_FILE (file name in the exefs), _BASE, _INDEX, _SHARDS, _SITES (absolute path of its site list, or empty).

set(NXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "recomp/ folder")
# The repository's venv (tools/.venv, recomp/requirements.txt) when it is there; else any Python 3 on the PATH, which is enough for
# build-info (standard library only), so a runtime-only build (pkp_rt.dll on CI) needs no venv. Lifting still needs the requirements.
if(WIN32)
  set(_nx_venv_python "${NXRECOMP_ROOT}/../tools/.venv/Scripts/python.exe")
else()
  set(_nx_venv_python "${NXRECOMP_ROOT}/../tools/.venv/bin/python")
endif()
if(EXISTS "${_nx_venv_python}")
  set(_nx_default_python "${_nx_venv_python}")
else()
  find_package(Python3 3.11 COMPONENTS Interpreter)
  set(_nx_default_python "${Python3_EXECUTABLE}")
endif()
set(NX_PYTHON "${_nx_default_python}" CACHE FILEPATH "Python with the nxrecomp requirements")
set(NX_OPT "" CACHE STRING "optimisation flag of the lifted code (clang-cl syntax); empty: nxrecomp/compile_flags.py CLANG_CL_OPT")
option(NX_SPIN_YIELD "spin-loop heads of the lifted code call nx_spin_hint(2) (nxrecomp/spin.py; lift --spin-yield)" OFF)
set(NX_LOCALS "$ENV{NX_LOCALS}" CACHE STRING "guest registers and flags in C locals: off / exact / aapcs / lean (nxrecomp/locals.py; lift --locals); empty: the game config's reg_locals")
option(NX_USE_LAUNCHER "wrap lifted-code compiles in cmake/nx_cc_launcher.py (time-limit fallback to optnone / -O0)" OFF)

file(GLOB NX_LIFTER_SOURCES CONFIGURE_DEPENDS
     "${NXRECOMP_ROOT}/nxrecomp/*.py" "${NXRECOMP_ROOT}/nxrecomp/lift/*.py" "${NXRECOMP_ROOT}/nxrecomp/lift/tier2/*.py"
     "${NXRECOMP_ROOT}/include/nxrecomp/*.h")

# one `build-info` run per game and configure: its JSON text, kept in the global property NXRECOMP_INFO_<game>
function(_nxrecomp_info game out)
  get_property(json GLOBAL PROPERTY NXRECOMP_INFO_${game})
  if(NOT json)
    execute_process(COMMAND "${NX_PYTHON}" -m nxrecomp build-info --game ${game} WORKING_DIRECTORY "${NXRECOMP_ROOT}"
                    OUTPUT_VARIABLE json RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "nxrecomp build-info --game ${game} failed (${NX_PYTHON}): ${err}")
    endif()
    set_property(GLOBAL PROPERTY NXRECOMP_INFO_${game} "${json}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRECOMP_ROOT}/nxrecomp/compile_flags.py"
                 "${NXRECOMP_ROOT}/nxrecomp/config.py" "${NXRECOMP_ROOT}/nxrecomp/__main__.py")
    if(NOT game STREQUAL "generic")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRECOMP_ROOT}/games/${game}/config.py")
    endif()
  endif()
  set(${out} "${json}" PARENT_SCOPE)
endfunction()

# the JSON array <key> of `build-info` as a CMake list
function(_nxrecomp_info_list json key out)
  set(r "")
  string(JSON n LENGTH "${json}" ${key})
  if(n GREATER 0)
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
      string(JSON v GET "${json}" ${key} ${i})
      list(APPEND r "${v}")
    endforeach()
  endif()
  set(${out} "${r}" PARENT_SCOPE)
endfunction()

function(nxrecomp_build_info game prefix)
  _nxrecomp_info(${game} json)
  set(names "")
  string(JSON n LENGTH "${json}" modules)
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON m GET "${json}" modules ${i} name)
    list(APPEND names ${m})
    foreach(k FILE BASE INDEX SHARDS SITES)
      string(TOLOWER ${k} key)
      string(JSON v GET "${json}" modules ${i} ${key})
      if(k STREQUAL "SITES" AND NOT v STREQUAL "")
        get_filename_component(v "${NXRECOMP_ROOT}/games/${game}/${v}" ABSOLUTE)
      endif()
      set(${prefix}_${m}_${k} "${v}" PARENT_SCOPE)
    endforeach()
  endforeach()
  set(${prefix}_MODULES "${names}" PARENT_SCOPE)
endfunction()

# nxrecomp_module_header(<game> <file>) writes the C++ header of the module list (nxrecomp/modheader.py: the NX_MODULES
# X-macro) at configure time; the file only changes when the module table does. The player build writes the same text.
function(nxrecomp_module_header game file)
  _nxrecomp_info(${game} json)      # registers the configure dependencies on the config
  execute_process(COMMAND "${NX_PYTHON}" -m nxrecomp module-header --game ${game} --out "${file}" WORKING_DIRECTORY "${NXRECOMP_ROOT}"
                  RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxrecomp module-header --game ${game} failed (${NX_PYTHON}): ${err}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRECOMP_ROOT}/nxrecomp/modheader.py")
endfunction()

function(nxrecomp_add_module target)
  cmake_parse_arguments(A "" "INPUT;NAME;BASE;INDEX;GAME;KIND;SHARDS;JOBS;OUTDIR;PATCHES;SITES" "" ${ARGN})
  if(NOT A_NAME)
    set(A_NAME main)
  endif()
  if(NOT A_BASE)
    set(A_BASE 0x7100000000)
  endif()
  if(NOT A_SHARDS)
    set(A_SHARDS 16)
  endif()
  if(NOT A_JOBS)
    cmake_host_system_information(RESULT A_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
  endif()
  if(NOT A_OUTDIR)
    set(A_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  endif()
  set(extra "")
  set(game_deps "")
  if(A_GAME)
    list(APPEND extra --game ${A_GAME})
    file(GLOB game_deps CONFIGURE_DEPENDS "${NXRECOMP_ROOT}/games/${A_GAME}/*.py")
  endif()
  if(A_KIND)
    list(APPEND extra --kind ${A_KIND})
  endif()
  if(NOT "${A_INDEX}" STREQUAL "")
    list(APPEND extra --index ${A_INDEX})
  endif()
  if(A_PATCHES)
    list(APPEND extra --patches ${A_PATCHES})
  endif()
  if(A_SITES)
    list(APPEND extra --sites ${A_SITES})
  endif()
  if(NX_SPIN_YIELD)
    list(APPEND extra --spin-yield)
  endif()
  if(NX_LOCALS)
    list(APPEND extra --locals ${NX_LOCALS})
  endif()
  set(outs "${A_OUTDIR}/funcs.h" "${A_OUTDIR}/module_${A_NAME}.cpp")
  set(shard_srcs "")
  math(EXPR last "${A_SHARDS} - 1")
  foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
      set(n "00${i}")
    elseif(len EQUAL 2)
      set(n "0${i}")
    else()
      set(n "${i}")
    endif()
    list(APPEND shard_srcs "${A_OUTDIR}/c_${n}.cpp")
  endforeach()
  # <target>.sites next to the library: the site list it was lifted with (nx_sites_stamp.cmake; checked by nxrecomp_check_lift)
  set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}.sites")
  add_custom_command(
    OUTPUT ${outs} ${shard_srcs} "${stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${A_OUTDIR}"
    COMMAND "${NX_PYTHON}" -m nxrecomp lift "${A_INPUT}" "${A_OUTDIR}" --name ${A_NAME} --base ${A_BASE}
            --shards ${A_SHARDS} --jobs ${A_JOBS} ${extra}
    COMMAND ${CMAKE_COMMAND} -DMODE=write "-DSITES=${A_SITES}" "-DOUT=${stamp}" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nx_sites_stamp.cmake"
    WORKING_DIRECTORY "${NXRECOMP_ROOT}"
    DEPENDS "${A_INPUT}" ${A_SITES} ${NX_LIFTER_SOURCES} ${game_deps}
    COMMENT "nxrecomp: lifting ${A_NAME} (${A_INPUT})"
    VERBATIM)
  add_library(${target} STATIC ${shard_srcs} "${A_OUTDIR}/module_${A_NAME}.cpp")
  target_include_directories(${target} PUBLIC "${NXRECOMP_ROOT}/include" "${A_OUTDIR}")
  target_compile_features(${target} PUBLIC cxx_std_20)
  _nxrecomp_info(generic info)
  if(MSVC)   # clang-cl
    string(JSON opt GET "${info}" clang_cl_opt)
    if(NX_OPT)
      set(opt "${NX_OPT}")
    endif()
    _nxrecomp_info_list("${info}" clang_cl flags)
    target_compile_options(${target} PRIVATE ${opt} ${flags})
  else()
    _nxrecomp_info_list("${info}" clang flags)
    target_compile_options(${target} PRIVATE ${flags})
  endif()
  if(NX_USE_LAUNCHER)
    set_target_properties(${target} PROPERTIES CXX_COMPILER_LAUNCHER "${NX_PYTHON};${NXRECOMP_ROOT}/cmake/nx_cc_launcher.py")
  endif()
endfunction()

# nxrecomp_check_lift(<check target> <prefix> <library folder> <library prefix>): a target, run on every build, that stops the build when
# an imported module library (<library folder>/<library prefix><m>.lib, modules from nxrecomp_build_info <prefix>) was lifted with
# other hook sites than this tree's site lists. Make the executable that links them depend on it.
function(nxrecomp_check_lift target prefix libdir libprefix)
  set(cmds "")
  foreach(m IN LISTS ${prefix}_MODULES)
    list(APPEND cmds COMMAND ${CMAKE_COMMAND} -DMODE=check "-DSITES=${${prefix}_${m}_SITES}" "-DSTAMP=${libdir}/${libprefix}${m}.sites"
                     -DMODULE=${m} -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nx_sites_stamp.cmake")
  endforeach()
  add_custom_target(${target} ${cmds} COMMENT "nxrecomp: lifted libraries match this tree's hook sites" VERBATIM)
endfunction()
