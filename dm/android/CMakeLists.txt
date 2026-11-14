# Android build of the host (arm64-v8a): the shared host sources (dm/host, the C parts of dm/gfx) with the Darwin stand-ins of compat/, the
# Android front end and graphics backend. Two products from one set of objects, and the app's loader:
#   libdmhost_<GAME>.so  the game's host in the app: one APK carries the host of every game packed on the computer (scripts/build_android_from_nsp.py)
#   dmhost               a command-line host for adb shell (headless: no window, silent audio unless DM_AUDIO=1), like the Mac's dmhost
#   libnxloader.so       the NativeActivity's library: starts the host of the game the launcher picked (loader.c)
#   libadrenotools.so, libhook_impl.so, libmain_hook.so   what DM_VK_DRIVER opens a Vulkan driver of the player's through (vk_driver.c), in every APK beside the hosts
# Configure with the NDK's toolchain file and the game profile, as the Mac host is built per profile:
#   cmake -G Ninja -S dm/android -B <build dir> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
#         -DANDROID_PLATFORM=android-29 -DGAME=<base game title ID> [-DDM_GAMES_DIR=<profiles folder outside dm/games>]
cmake_minimum_required(VERSION 3.22)
project(dmhost_android C CXX ASM)

if(NOT GAME)
  message(FATAL_ERROR "GAME is not set: -DGAME=<base game title ID> (dm/games/<id>)")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN}/${GAME} ${GEN}/asm)

# The profile header (dm/games/game.py header), regenerated at every configure and build; game.py writes it only when it changes
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

# The commit and version the host is built from, for its build: line at start (build_stamp.c, main.c): asked at every build (git in a checkout; VERSION and
# tools/build_commit in a release's archive), the header rewritten only when it changes
set(STAMP_H ${GEN}/build_stamp.h)
add_custom_target(build_stamp ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_stamp.py ${STAMP_H}
  BYPRODUCTS ${STAMP_H} COMMENT "build stamp")

# Assembly: the Mach-O sources converted to ELF (tools/asm_elf.py)
set(ASM_ELF)
foreach(s ${DM}/host/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  add_custom_command(OUTPUT ${GEN}/asm/${n}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py ${s} ${GEN}/asm/${n}
    DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py)
  list(APPEND ASM_ELF ${GEN}/asm/${n})
endforeach()

# The Vulkan replay's own shaders (shaders/*.vert, *.frag, *.comp: gfx_vk_dscopy.inc's depth-stencil copies, gfx_vk_bcdec.inc's BC decode), compiled by the NDK's glslc into the SPIR-V words gfx_vk.c
# includes (-mfmt=c writes a C initializer)
file(GLOB GLSLC ${ANDROID_NDK}/shader-tools/*/glslc ${ANDROID_NDK}/shader-tools/*/glslc.exe)
if(NOT GLSLC)
  message(FATAL_ERROR "no glslc in ${ANDROID_NDK}/shader-tools: the NDK's shader tools build the Vulkan replay's shaders")
endif()
list(GET GLSLC 0 GLSLC)
file(MAKE_DIRECTORY ${GEN}/shaders)
set(SHADER_INC)
foreach(s ds_copy.vert ds_copy_export.frag ds_copy_bits.frag bc_decode.comp gmem_test.comp gmem_test_noptr.comp gmem_probe.comp upscale_square.comp)
  add_custom_command(OUTPUT ${GEN}/shaders/${s}.inc
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s} -o ${GEN}/shaders/${s}.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s})
  list(APPEND SHADER_INC ${GEN}/shaders/${s}.inc)
endforeach()

# Host sources: every dm/host/*.c, as the Mac Makefile's wildcard, less the ones Android replaces
file(GLOB HOST_C ${DM}/host/*.c)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_movie_avc.c ${DM}/host/hle_movie_av1.c)   # VideoToolbox / dav1d: media_android.c and av1_android.c (MediaCodec)
# Not on Android yet, stubbed in apple_stubs.c: the cheat engine (blocks and dispatch queues)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_cheats.c)
# Graphics: the NVN interface layer and texture code (the Metal replay layer, the Vulkan and GL layers are Objective-C on Metal: backend_*.c stand in)
file(GLOB GFX_C ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
file(GLOB_RECURSE VPX_C ${DM}/third_party/libvpx/*.c)

# The NVN replay: gfx_vk.c (Vulkan) by default; -DDM_GFX=null builds gfx_null.c (no GPU: submissions completed on the CPU, nothing drawn)
set(DM_GFX vk CACHE STRING "NVN replay backend: vk or null")
set(ANDROID_C
  ${CMAKE_CURRENT_SOURCE_DIR}/plat_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/pac_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_aaudio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/ue_dynres_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/av1_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/haptics_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/apple_stubs.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk_driver.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk_start.c
  ${CMAKE_CURRENT_SOURCE_DIR}/module_pack.c
  ${CMAKE_CURRENT_SOURCE_DIR}/build_stamp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/gfx_${DM_GFX}.c)

# char is signed, as on Apple arm64 (the host and its third-party code were written there); AArch64 Linux / Android makes plain char unsigned. bcdec read BC4 / BC5 SNORM
# endpoints through (char): negative ones came out as 128..255 and every signed normal map decoded to noise (sparkling ground, bright seams in clouds)
add_compile_options(-fsigned-char)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w -fvisibility=hidden)

# The replay records every draw on the game's render thread (descriptor writes, pipeline and view lookups, vertex and uniform copies): -O2 there, after the target's -O1
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/gfx_vk.c PROPERTIES COMPILE_OPTIONS "-O2" OBJECT_DEPENDS "${SHADER_INC}")
set_source_files_properties(${DM}/gfx/nvn/hle_nvn.c PROPERTIES COMPILE_OPTIONS "-fno-omit-frame-pointer")   # frame records: DM_NVN_SIZE_TRACE walks the game's call chain from them
add_library(dmcore OBJECT ${HOST_C} ${GFX_C} ${ANDROID_C} ${ASM_ELF} ${SHADER_INC})
add_dependencies(dmcore game_profile build_stamp)
target_include_directories(dmcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include
  ${GEN}/${GAME} ${GEN} ${DM}/third_party/libvpx ${DM}/third_party/stb_vorbis)
# x18 holds the guest thread's slot block (dm.h dm_slot_get): the compiler must never use it (Android reserves it by default; stated anyway)
target_compile_options(dmcore PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-format -Wno-deprecated-declarations -Wno-gnu-folding-constant -Wno-multichar -Wno-four-char-constants -Wno-unknown-pragmas -Wno-missing-braces>
  -ffixed-x18 -fvisibility=hidden -fPIC -march=armv8.2-a+lse)
target_compile_definitions(dmcore PRIVATE DM_ANDROID=1)

# no libvulkan.so: vk_driver.c opens it, or DM_VK_DRIVER's driver in its place
set(DM_LIBS vpx log android nativewindow aaudio mediandk m dl)   # nativewindow: AHardwareBuffer (raw global memory's pools without placed maps, gfx_vk_gmem.inc)

set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(dmhost_app SHARED $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_android.c ${CMAKE_CURRENT_SOURCE_DIR}/nvn_demo.c ${GLUE}/android_native_app_glue.c)
target_include_directories(dmhost_app PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES> ${GLUE})
target_compile_options(dmhost_app PRIVATE -include dm_android.h -ffixed-x18 -O1 -g -Wall)
target_link_libraries(dmhost_app ${DM_LIBS} jnigraphics)
target_link_options(dmhost_app PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384 -u ANativeActivity_onCreate)
set_target_properties(dmhost_app PROPERTIES OUTPUT_NAME dmhost_${GAME})
add_library(nxloader SHARED ${CMAKE_CURRENT_SOURCE_DIR}/loader.c)
target_compile_options(nxloader PRIVATE -O2 -Wall -fvisibility=hidden)
target_link_libraries(nxloader log android dl)
target_link_options(nxloader PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)

add_executable(dmhost_cli $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_cli.c ${CMAKE_CURRENT_SOURCE_DIR}/nvn_demo.c)
set_target_properties(dmhost_cli PROPERTIES OUTPUT_NAME dmhost)
target_include_directories(dmhost_cli PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost_cli PRIVATE -include dm_android.h -ffixed-x18 -O1 -g -Wall)
target_link_libraries(dmhost_cli ${DM_LIBS})
target_link_options(dmhost_cli PRIVATE -Wl,-z,max-page-size=16384 -Wl,--error-limit=0)

# libadrenotools (dm/third_party/libadrenotools, BSD 2-Clause), built as its CMake files build it. The host opens libadrenotools.so only when DM_VK_DRIVER is set: the
# constructor of liblinkernsbypass, linked into it, rewrites the protection of the linker's dlopen page as it loads. The hooks are looked for in the app's native library
# folder, which the manifests extract (extractNativeLibs)
set(ADRENOTOOLS ${DM}/third_party/libadrenotools)
add_library(linkernsbypass STATIC ${ADRENOTOOLS}/lib/linkernsbypass/android_linker_ns.cpp ${ADRENOTOOLS}/lib/linkernsbypass/elf_soname_patcher.cpp)
target_include_directories(linkernsbypass PUBLIC ${ADRENOTOOLS}/lib/linkernsbypass)
target_compile_options(linkernsbypass PRIVATE -fPIC -O2 -w)   # (its soname patcher truncates a snprintf on purpose)
target_link_libraries(linkernsbypass android dl)
add_library(adrenotools SHARED ${ADRENOTOOLS}/src/driver.cpp)
target_include_directories(adrenotools PRIVATE ${ADRENOTOOLS}/include)
target_compile_options(adrenotools PRIVATE -O2)
target_link_libraries(adrenotools android log linkernsbypass)
target_link_options(adrenotools PRIVATE -Wl,-s,--exclude-libs,liblinkernsbypass.a -Wl,--no-undefined -Wl,-z,max-page-size=16384)
add_library(hook_impl SHARED ${ADRENOTOOLS}/src/hook/hook_impl.cpp)
target_include_directories(hook_impl PRIVATE ${ADRENOTOOLS}/include)
target_compile_options(hook_impl PRIVATE -O2)
set_target_properties(hook_impl PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_link_libraries(hook_impl linkernsbypass log)
target_link_options(hook_impl PRIVATE -Wl,-s -Wl,--no-undefined -Wl,-z,max-page-size=16384)
add_library(main_hook SHARED ${ADRENOTOOLS}/src/hook/main_hook.c)
target_compile_options(main_hook PRIVATE -O2)
target_link_libraries(main_hook hook_impl)
target_link_options(main_hook PRIVATE -Wl,-s -Wl,-z,global -Wl,--no-undefined -Wl,-z,max-page-size=16384)

# Host unit tests for adb shell (-DDM_TESTS=ON): the sources and flags of dm/host/tests/Makefile, with the Android stand-ins (plat_android.c)
option(DM_TESTS "build dm/host/tests for the device" OFF)
if(DM_TESTS)
  set(T ${DM}/host/tests)
  set(TEST_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/plat_android.c)
  function(dm_test name)
    cmake_parse_arguments(A "" "PROFILE" "SOURCES;DEFS" ${ARGN})
    add_executable(${name} ${A_SOURCES} ${TEST_COMMON})
    add_dependencies(${name} game_profile)
    if(A_PROFILE)
      target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${A_PROFILE})
    else()
      target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${GEN}/${GAME} ${DM}/games/${GAME})
    endif()
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -Wno-unused-function -Wno-format -Wno-deprecated-declarations> -O1 -g -ffixed-x18)
    target_compile_definitions(${name} PRIVATE ${A_DEFS})
    target_link_libraries(${name} log)
  endfunction()
  dm_test(t_thread_leak SOURCES ${T}/t_thread_leak.c)
  dm_test(t_condvar SOURCES ${T}/t_condvar.c ${T}/t_condvar_stub.c ${DM}/host/hle_thread.c DEFS DM_TEST_HOOKS)
  dm_test(t_fs_commit_list SOURCES ${T}/t_fs_commit_list.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c ${DM}/host/save_sync.c)
  dm_test(t_svc_memory SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile)
  dm_test(t_svc_memory_unlimited SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile DEFS DM_GAME_MEM_MAP_LIMIT=0)
  # t_svc_memory_16k is not built: it simulates 16 KiB pages over the kernel's, which works where mmap aligns to 16 KiB (Apple silicon); a 4 KiB kernel gives
  # the guest address space only 4 KiB alignment. A 16 KiB-page Android kernel runs t_svc_memory itself at its real page size
  dm_test(t_nro SOURCES ${T}/t_nro.c)
  dm_test(t_module_pack SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_module_pack.c ${CMAKE_CURRENT_SOURCE_DIR}/module_pack.c)
  dm_test(t_vm_remap SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_vm_remap.c)   # vm_remap to a fixed destination: a refused duplication leaves no hole (plat_android.c)
  dm_test(t_audio SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_audio.c ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_aaudio.c)
  target_link_libraries(t_audio aaudio mediandk m)
  dm_test(t_media SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_media.c ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c)
  target_link_libraries(t_media mediandk android m)
  # hle_movie.c's decoders on media_android.c, driven the way The World Ends with You: Final Remix's movie player drives them (droidplay-core#480)
  dm_test(t_movie_client SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_movie_client.c ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c)
  target_include_directories(t_movie_client PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config ${DM}/third_party/stb_vorbis)
  target_link_libraries(t_movie_client vpx mediandk android m)
  dm_test(t_hid SOURCES ${T}/t_hid.c ${DM}/host/hle_haptics.c)
  dm_test(t_preempt SOURCES ${T}/t_preempt.c ${T}/t_condvar_stub.c)
  dm_test(t_fs SOURCES ${T}/t_fs.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c ${DM}/host/save_sync.c)
  # The NVN interface layer's tests (dm/gfx/tests/t_nvn.c includes hle_nvn.c) against the GPU-less replay of gfx_null.c
  set(GFX_NO_IF ${GFX_C})
  list(REMOVE_ITEM GFX_NO_IF ${DM}/gfx/nvn/hle_nvn.c)
  dm_test(t_nvn SOURCES ${DM}/gfx/tests/t_nvn.c ${DM}/host/crash.c ${DM}/host/hle_bigstack.c ${GFX_NO_IF} ${CMAKE_CURRENT_SOURCE_DIR}/gfx_${DM_GFX}.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_android_stubs.c ${DM}/gfx/tests/t_host_stubs.c ${CMAKE_CURRENT_SOURCE_DIR}/apple_stubs.c ${GEN}/asm/nvn_entry.S ${GEN}/asm/nvn_thunks.S
          ${CMAKE_CURRENT_SOURCE_DIR}/vk_driver.c ${CMAKE_CURRENT_SOURCE_DIR}/vk_start.c)
  target_include_directories(t_nvn PRIVATE ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include)
  target_compile_options(t_nvn PRIVATE -march=armv8.2-a+lse)
  target_link_libraries(t_nvn dl)
  target_link_options(t_nvn PRIVATE -Wl,--error-limit=0)
endif()
