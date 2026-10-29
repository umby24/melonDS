# Switch app: melonDS core + borealis UI (deko3d backend).
#   scripts/build.sh            -> switch/melonds-next.nro
# Configure by hand (inside devkitpro/devkita64):
#   cmake -S switch -B switch/build -DCMAKE_BUILD_TYPE=Release && cmake --build switch/build --target melonds-next.nro
cmake_minimum_required(VERSION 3.13)

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/../borealis/library)
set(MDS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../mds_upstream/src)
set(RCHEEVOS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../rcheevos)

# borealis build options (must be set before its cmake helpers are included)
set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(USE_DEKO3D ON CACHE BOOL "" FORCE)
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)  # pulls in devkitPro's Switch.cmake

project(melonds-next C CXX ASM)
set(APP_TITLE "melonds-next")
set(APP_AUTHOR "umby24")
set(APP_VERSION "0.1.0")

# borealis' toolchain.cmake overwrites CMAKE_C_FLAGS, which drops devkitPro's arch flags.
# They are mandatory: without -mtp=soft every thread_local access (melonDS's NDS::Current)
# reads a bogus thread pointer and crashes.
set(NX_ARCH_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec")
string(APPEND CMAKE_C_FLAGS " ${NX_ARCH_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS " ${NX_ARCH_FLAGS}")
string(APPEND CMAKE_ASM_FLAGS " ${NX_ARCH_FLAGS}")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

add_subdirectory(${BOREALIS_LIBRARY} borealis)

#---------------------------------------------------------------------------------
# melonDS core. Mirrors mds_upstream/src/CMakeLists.txt (core target without OpenGL,
# GDB stub or JIT) and src/teakra/src/CMakeLists.txt. Re-check both when rebasing.
#---------------------------------------------------------------------------------
set(MELONDS_VERSION "1.1")
set(melonDS_VERSION ${MELONDS_VERSION})
set(melonDS_HOMEPAGE_URL "https://melonds.kuribo64.net")
set(MELONDS_VERSION_SUFFIX "-switch")
configure_file(${MDS_SRC}/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen/version.h)

set(MELONDS_SOURCES
    ARCodeFile.cpp ARDatabaseDAT.cpp AREngine.cpp ARM.cpp
    ARMInterpreter.cpp ARMInterpreter_ALU.cpp ARMInterpreter_Branch.cpp ARMInterpreter_LoadStore.cpp
    CP15.cpp CRC32.cpp DMA.cpp DMA_Timings.cpp
    DSi.cpp DSi_AES.cpp DSi_Camera.cpp DSi_DSP.cpp DSi_I2C.cpp DSi_I2S.cpp DSi_NAND.cpp
    DSi_NDMA.cpp DSi_NWifi.cpp DSi_SD.cpp DSi_SPI_TSC.cpp
    FATIO.cpp FATStorage.cpp GBACart.cpp GBACartMotionPak.cpp
    GPU.cpp GPU_Soft.cpp GPU2D.cpp GPU2D_Soft.cpp GPU3D.cpp GPU3D_Soft.cpp GPU3D_Texcache.cpp
    Mic.cpp NDS.cpp NDSCart.cpp ROMList.cpp FreeBIOS.cpp RTC.cpp Savestate.cpp
    SPI.cpp SPI_Firmware.cpp SPU.cpp Utils.cpp Wifi.cpp WifiAP.cpp
    DSP_HLE/UcodeBase.cpp DSP_HLE/AACUcode.cpp DSP_HLE/G711Ucode.cpp DSP_HLE/GraphicsUcode.cpp
    NDSCart/CartCommon.cpp NDSCart/CartRetail.cpp NDSCart/CartRetailNAND.cpp
    NDSCart/CartRetailIR.cpp NDSCart/CartRetailBT.cpp NDSCart/CartSD.cpp
    NDSCart/CartHomebrew.cpp NDSCart/CartR4.cpp
    fatfs/ff.c fatfs/ffsystem.c fatfs/ffunicode.c
    sha1/sha1.c tiny-AES-c/aes.c xxhash/xxhash.c blip-buf/blip_buf.c
    teakra/src/ahbm.cpp teakra/src/apbp.cpp teakra/src/btdmp.cpp teakra/src/disassembler.cpp
    teakra/src/dma.cpp teakra/src/timer.cpp teakra/src/memory_interface.cpp teakra/src/mmio.cpp
    teakra/src/parser.cpp teakra/src/processor.cpp teakra/src/teakra.cpp)
# AArch64 JIT (upstream's ENABLE_JIT + ARM64 branch). Fastmem is separate: see host.
option(MELONDS_JIT "Build melonDS's AArch64 JIT" ON)
if (MELONDS_JIT)
    list(APPEND MELONDS_SOURCES
        ARM_InstrInfo.cpp ARMJIT.cpp ARMJIT_Memory.cpp ARMJIT_Global.cpp
        dolphin/CommonFuncs.cpp dolphin/Arm64Emitter.cpp dolphin/MathUtil.cpp
        ARMJIT_A64/ARMJIT_Compiler.cpp ARMJIT_A64/ARMJIT_ALU.cpp
        ARMJIT_A64/ARMJIT_LoadStore.cpp ARMJIT_A64/ARMJIT_Branch.cpp
        ARMJIT_A64/ARMJIT_Linkage.S)
endif ()
list(TRANSFORM MELONDS_SOURCES PREPEND ${MDS_SRC}/)

add_library(melonds STATIC ${MELONDS_SOURCES})
set_target_properties(melonds PROPERTIES CXX_STANDARD 17 C_STANDARD 11)
target_include_directories(melonds PUBLIC
    ${MDS_SRC} ${MDS_SRC}/teakra/include ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen)
target_compile_definitions(melonds PRIVATE __SWITCH__)
if (MELONDS_JIT)
    target_compile_definitions(melonds PUBLIC JIT_ENABLED)
endif ()
target_compile_options(melonds PUBLIC -fwrapv
    PRIVATE -ffunction-sections -fdata-sections $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof>)

#---------------------------------------------------------------------------------
# rcheevos (RetroAchievements client), minus RAIntegration / external-client / libretro glue.
#---------------------------------------------------------------------------------
file(GLOB RCHEEVOS_SOURCES CONFIGURE_DEPENDS
    ${RCHEEVOS_DIR}/src/*.c ${RCHEEVOS_DIR}/src/rapi/*.c ${RCHEEVOS_DIR}/src/rcheevos/*.c
    ${RCHEEVOS_DIR}/src/rhash/*.c ${RCHEEVOS_DIR}/src/rurl/*.c)
list(FILTER RCHEEVOS_SOURCES EXCLUDE REGEX "(rc_client_raintegration|rc_client_external|rc_libretro)\\.c$")
add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
set_target_properties(rcheevos PROPERTIES C_STANDARD 11)
target_include_directories(rcheevos PUBLIC ${RCHEEVOS_DIR}/include)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH)

#---------------------------------------------------------------------------------
# App
#---------------------------------------------------------------------------------
file(GLOB APP_SOURCES CONFIGURE_DEPENDS
    source/*.cpp source/frontend/*.cpp source/platform/*.cpp source/gpu/*.cpp source/ui/*.cpp)
list(APPEND APP_SOURCES ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
# Upstream's screen layout math (desktop melonDS's sizing/emphasis rules); frontend-side
# but self-contained, so we build it as is.
list(APPEND APP_SOURCES ${MDS_SRC}/frontend/ScreenLayout.cpp)

program_target(${PROJECT_NAME} "${APP_SOURCES}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE source ${MDS_SRC}/frontend)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -ffunction-sections -fdata-sections)
target_link_options(${PROJECT_NAME} PRIVATE ${APP_PLATFORM_LINK_OPTION})
# curl uses libnx's ssl service for HTTPS (RetroAchievements).
target_link_libraries(${PROJECT_NAME} PRIVATE borealis melonds rcheevos curl z)

# RomFS: our resources/ plus the parts of borealis' resources/ the library itself loads
# (i18n strings, system icons, Material icon font) and the deko3d nanovg shaders.
set(ROMFS_DIR ${CMAKE_CURRENT_BINARY_DIR}/romfs)
file(REMOVE_RECURSE ${ROMFS_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/resources/ DESTINATION ${ROMFS_DIR})
file(COPY ${BOREALIS_LIBRARY}/../resources/i18n DESTINATION ${ROMFS_DIR} PATTERN "demo.json" EXCLUDE)
file(COPY ${BOREALIS_LIBRARY}/../resources/img/sys DESTINATION ${ROMFS_DIR}/img)
file(COPY ${BOREALIS_LIBRARY}/../resources/material DESTINATION ${ROMFS_DIR})
gen_dksh(${ROMFS_DIR}/shaders)

# Upstream's compute 3D renderer shaders for gpu/deko_renderer3d: a host tool assembles
# every variant from GPU3D_Compute_shaders.h (adapted to deko3d), uam compiles them.
# Like gen_dksh, at configure time; re-run when the shaders or the tool change.
set(COMPUTE3D_TOOL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_compute_shaders.cpp)
set(COMPUTE3D_SHADERS_H ${MDS_SRC}/GPU3D_Compute_shaders.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${COMPUTE3D_TOOL_SRC} ${COMPUTE3D_SHADERS_H})
set(COMPUTE3D_WORK ${CMAKE_CURRENT_BINARY_DIR}/compute3d)
file(REMOVE_RECURSE ${COMPUTE3D_WORK})
file(MAKE_DIRECTORY ${COMPUTE3D_WORK} ${ROMFS_DIR}/shaders/compute3d)
execute_process(COMMAND c++ -std=c++17 -O1 -I${MDS_SRC} ${COMPUTE3D_TOOL_SRC} -o ${COMPUTE3D_WORK}/gen
                RESULT_VARIABLE COMPUTE3D_RESULT)
if (NOT COMPUTE3D_RESULT EQUAL 0)
    message(FATAL_ERROR "building tools/gen_compute_shaders.cpp (host c++) failed")
endif()
execute_process(COMMAND ${COMPUTE3D_WORK}/gen ${COMPUTE3D_WORK} RESULT_VARIABLE COMPUTE3D_RESULT)
if (NOT COMPUTE3D_RESULT EQUAL 0)
    message(FATAL_ERROR "generating the compute 3D shaders failed")
endif()
find_program(NX_UAM_EXE NAMES uam HINTS "${DEVKITPRO}/tools/bin")
file(GLOB COMPUTE3D_GLSL ${COMPUTE3D_WORK}/*.comp.glsl)
foreach(GLSL ${COMPUTE3D_GLSL})
    get_filename_component(NAME ${GLSL} NAME)
    string(REPLACE ".comp.glsl" ".dksh" NAME ${NAME})
    execute_process(COMMAND ${NX_UAM_EXE} -s comp -o ${ROMFS_DIR}/shaders/compute3d/${NAME} ${GLSL}
                    RESULT_VARIABLE COMPUTE3D_RESULT)
    if (NOT COMPUTE3D_RESULT EQUAL 0)
        message(FATAL_ERROR "uam failed on ${GLSL}")
    endif()
endforeach()

# melonDS's two-screens icon (as in the earlier Switch port), recoloured.
set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg)
add_custom_target(${PROJECT_NAME}.nro ALL DEPENDS ${PROJECT_NAME}
    COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${APP_AUTHOR}" "${APP_VERSION}" ${PROJECT_NAME}.nacp
    COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.nro
            --icon=${APP_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${ROMFS_DIR})
