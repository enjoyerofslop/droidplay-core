# Linux build of the host (aarch64, glibc): the Android port's host (dm/android: the shared sources with the Darwin stand-ins of compat/, the Vulkan replay of NVN)
# with a Linux front end, audio and movie decoding in place of Android's. Products, from one set of objects:
#   dmhost_<GAME>   the game's host with a window (front_linux.c, SDL3: a Wayland window with a Vulkan swapchain, controllers, rumble, keyboard)
#   dmhost_cli      a host without a window, as the Mac's dmhost and Android's adb host: the self test and scripted runs (silent audio unless DM_AUDIO=1)
# Modules are rewritten with DM_TARGET=linux (dm/tools/nso_asm.py: the thread slots at tpidr_el0, plat_linux.c), shaders translated as for Android (dm/android/tools/spv_build.py).
# Configure on an aarch64 Linux with the game profile, as the Mac host is built per profile:
#   cmake -G Ninja -S dm/linux -B <build dir> -DCMAKE_C_COMPILER=clang -DGAME=<base game title ID> [-DDM_GAMES_DIR=<profiles folder outside dm/games>]
# Needs clang, CMake 3.22, glslc (shaderc), Python 3, the Vulkan headers, SDL3 and FFmpeg's libavcodec / libavutil with their development files.
cmake_minimum_required(VERSION 3.22)
project(dmhost_linux C ASM)

if(NOT GAME)
  message(FATAL_ERROR "GAME is not set: -DGAME=<base game title ID> (dm/games/<id>)")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  message(FATAL_ERROR "the host runs the game's arm64 code natively: build it for aarch64 (this is ${CMAKE_SYSTEM_PROCESSOR})")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavutil)
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(AND ${DM}/android)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN}/${GAME} ${GEN}/asm ${GEN}/shaders)

# The profile header (dm/games/game.py header), as for Android
set(GAME_H ${GEN}/${GAME}/game_profile.h)
set(GAME_ENV)
if(DM_GAMES_DIR)
  set(GAME_ENV ${CMAKE_COMMAND} -E env DM_GAMES_DIR=${DM_GAMES_DIR})
endif()
add_custom_target(game_profile ALL
  COMMAND ${GAME_ENV} ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GAME_H}
  BYPRODUCTS ${GAME_H} COMMENT "game profile ${GAME}")
execute_process(COMMAND ${GAME_ENV} ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GAME_H} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "game.py header ${GAME} failed")
endif()

set(STAMP_H ${GEN}/build_stamp.h)
add_custom_target(build_stamp ALL
  COMMAND ${Python3_EXECUTABLE} ${AND}/tools/build_stamp.py ${STAMP_H}
  BYPRODUCTS ${STAMP_H} COMMENT "build stamp")

# Assembly: the Mach-O sources converted to ELF (dm/android/tools/asm_elf.py)
set(ASM_ELF)
foreach(s ${DM}/host/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  add_custom_command(OUTPUT ${GEN}/asm/${n}
    COMMAND ${Python3_EXECUTABLE} ${AND}/tools/asm_elf.py ${s} ${GEN}/asm/${n}
    DEPENDS ${s} ${AND}/tools/asm_elf.py)
  list(APPEND ASM_ELF ${GEN}/asm/${n})
endforeach()

# The Vulkan replay's own shaders, as for Android, with the system's glslc
find_program(GLSLC glslc REQUIRED)
set(SHADER_INC)
foreach(s ds_copy.vert ds_copy_export.frag ds_copy_bits.frag bc_decode.comp gmem_test.comp gmem_test_noptr.comp gmem_probe.comp upscale_square.comp)
  add_custom_command(OUTPUT ${GEN}/shaders/${s}.inc
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=c ${AND}/shaders/${s} -o ${GEN}/shaders/${s}.inc
    DEPENDS ${AND}/shaders/${s})
  list(APPEND SHADER_INC ${GEN}/shaders/${s}.inc)
endforeach()

# Host sources: every dm/host/*.c less the ones Android replaces (its list in dm/android/CMakeLists.txt)
file(GLOB HOST_C ${DM}/host/*.c)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_movie_avc.c ${DM}/host/hle_movie_av1.c ${DM}/host/hle_cheats.c)   # VideoToolbox: media_linux.c; the cheat engine: apple_stubs.c
list(REMOVE_ITEM HOST_C ${DM}/host/main.c)   # built per product below: the windowed host renames its main (front_linux.c runs it on the guest thread)
file(GLOB GFX_C ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
file(GLOB_RECURSE VPX_C ${DM}/third_party/libvpx/*.c)

set(DM_GFX vk CACHE STRING "NVN replay backend: vk or null")
# Android's platform files that are plain Linux, and the Linux ones. Not here: pac_android.c (bionic gives each thread its own pointer authentication key, glibc does
# not), coreaudio_aaudio.c, media_android.c, av1_android.c and haptics_android.c (Android's APIs), loader.c (the app's NativeActivity)
set(LINUX_C
  ${AND}/plat_android.c
  ${AND}/ue_dynres_android.c
  ${AND}/apple_stubs.c
  ${AND}/vk_driver.c
  ${AND}/vk_start.c
  ${AND}/module_pack.c
  ${AND}/build_stamp.c
  ${AND}/gfx_${DM_GFX}.c
  ${CMAKE_CURRENT_SOURCE_DIR}/plat_linux.c
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_sdl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/media_linux.c)

add_compile_options(-fsigned-char)   # as on Apple arm64 (dm/android/CMakeLists.txt says why)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w -fvisibility=hidden)

set_source_files_properties(${AND}/gfx_vk.c PROPERTIES COMPILE_OPTIONS "-O2" OBJECT_DEPENDS "${SHADER_INC}")
set_source_files_properties(${DM}/gfx/nvn/hle_nvn.c PROPERTIES COMPILE_OPTIONS "-fno-omit-frame-pointer")
add_library(dmcore OBJECT ${HOST_C} ${GFX_C} ${LINUX_C} ${ASM_ELF} ${SHADER_INC})
add_dependencies(dmcore game_profile build_stamp)
target_include_directories(dmcore PUBLIC
  ${AND}/compat ${AND} ${DM}/host ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include
  ${GEN}/${GAME} ${GEN} ${DM}/third_party/libvpx ${DM}/third_party/stb_vorbis)
# DM_ANDROID: the shared sources' switch for this host family (the Vulkan replay, its present fences and frame step); DM_LINUX: where glibc Linux differs from Android
set(DM_C_FLAGS -include dm_android.h -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format
  -Wno-deprecated-declarations -Wno-gnu-folding-constant -Wno-multichar -Wno-four-char-constants -Wno-unknown-pragmas -Wno-missing-braces)
target_compile_options(dmcore PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_C_FLAGS}> -fvisibility=hidden -march=armv8.2-a+lse)
target_compile_definitions(dmcore PRIVATE DM_ANDROID=1 DM_LINUX=1)
target_link_libraries(dmcore PRIVATE PkgConfig::SDL3 PkgConfig::AV)

# libvulkan.so.1 is opened by vk_driver.c, not linked
set(DM_LIBS vpx PkgConfig::SDL3 PkgConfig::AV m dl pthread)

# main.c twice: as is for the command-line host, with its main renamed for the windowed one
foreach(m front cli)
  add_library(main_${m} OBJECT ${DM}/host/main.c)
  target_include_directories(main_${m} PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_options(main_${m} PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_C_FLAGS}> -march=armv8.2-a+lse)
  target_compile_definitions(main_${m} PRIVATE DM_ANDROID=1 DM_LINUX=1)
  add_dependencies(main_${m} game_profile build_stamp)
endforeach()
target_compile_definitions(main_front PRIVATE main=dm_host_main)

add_executable(dmhost $<TARGET_OBJECTS:dmcore> $<TARGET_OBJECTS:main_front> ${CMAKE_CURRENT_SOURCE_DIR}/front_linux.c ${AND}/nvn_demo.c)
set_target_properties(dmhost PROPERTIES OUTPUT_NAME dmhost_${GAME})
target_include_directories(dmhost PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_C_FLAGS}> -march=armv8.2-a+lse)
target_compile_definitions(dmhost PRIVATE DM_ANDROID=1 DM_LINUX=1)
target_link_libraries(dmhost ${DM_LIBS})

add_executable(dmhost_cli $<TARGET_OBJECTS:dmcore> $<TARGET_OBJECTS:main_cli> ${AND}/front_cli.c ${CMAKE_CURRENT_SOURCE_DIR}/front_cli_linux.c ${AND}/nvn_demo.c)
target_include_directories(dmhost_cli PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost_cli PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_C_FLAGS}> -march=armv8.2-a+lse)
target_compile_definitions(dmhost_cli PRIVATE DM_ANDROID=1 DM_LINUX=1)
target_link_libraries(dmhost_cli ${DM_LIBS})

# Host unit tests (-DDM_TESTS=ON): Android's list (dm/android/CMakeLists.txt) less its Android-only ones, run by ctest
option(DM_TESTS "build the host unit tests" OFF)
if(DM_TESTS)
  enable_testing()
  set(T ${DM}/host/tests)
  set(TEST_COMMON ${AND}/plat_android.c ${CMAKE_CURRENT_SOURCE_DIR}/plat_linux.c)
  function(dm_test name)
    cmake_parse_arguments(A "" "PROFILE" "SOURCES;DEFS" ${ARGN})
    add_executable(${name} ${A_SOURCES} ${TEST_COMMON})
    add_dependencies(${name} game_profile)
    if(A_PROFILE)
      target_include_directories(${name} PRIVATE ${AND}/compat ${DM}/host ${A_PROFILE})
    else()
      target_include_directories(${name} PRIVATE ${AND}/compat ${DM}/host ${GEN}/${GAME} ${DM}/games/${GAME})
    endif()
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -Wno-unused-function -Wno-format -Wno-deprecated-declarations> -O1 -g)
    target_compile_definitions(${name} PRIVATE DM_LINUX=1 ${A_DEFS})
    target_link_libraries(${name} m pthread)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  endfunction()
  dm_test(t_thread_leak SOURCES ${T}/t_thread_leak.c)
  dm_test(t_condvar SOURCES ${T}/t_condvar.c ${T}/t_condvar_stub.c ${DM}/host/hle_thread.c DEFS DM_TEST_HOOKS)
  dm_test(t_fs_commit_list SOURCES ${T}/t_fs_commit_list.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c ${DM}/host/save_sync.c)
  dm_test(t_svc_memory SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile)
  dm_test(t_svc_memory_unlimited SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile DEFS DM_GAME_MEM_MAP_LIMIT=0)
  dm_test(t_nro SOURCES ${T}/t_nro.c)
  dm_test(t_module_pack SOURCES ${AND}/tests/t_module_pack.c ${AND}/module_pack.c)
  dm_test(t_vm_remap SOURCES ${AND}/tests/t_vm_remap.c)
  dm_test(t_hid SOURCES ${T}/t_hid.c ${DM}/host/hle_haptics.c)
  dm_test(t_preempt SOURCES ${T}/t_preempt.c ${T}/t_condvar_stub.c)
  dm_test(t_fs SOURCES ${T}/t_fs.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c ${DM}/host/save_sync.c)
endif()
