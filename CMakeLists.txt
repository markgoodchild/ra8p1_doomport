# Please see README.md for build instructions

cmake_minimum_required(VERSION 3.16.4)

set(CMAKE_SYSTEM_NAME Generic)

set(CMAKE_SYSTEM_PROCESSOR arm)

# Use add_library() with the STATIC option to name the source file in the generated project.
# This avoids running the linker and is intended for use with cross-compiling toolchains that
# cannot link without custom flags or linker scripts
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo MinSizeRel)

if(BUILD_KIT)
	message(NOTICE "Build Kit is defined as ${BUILD_KIT}")
endif()

message(NOTICE "CMAKE_CURRENT_BINARY_DIR is defined as ${CMAKE_CURRENT_BINARY_DIR}")

get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(isMultiConfig)
    message(NOTICE "Multi-config generator(${CMAKE_GENERATOR}) is active")
else()
    message(NOTICE "Single-config generator(${CMAKE_GENERATOR}) is active")
	if(DEFINED CMAKE_BUILD_TYPE)
	 	message(NOTICE "CMAKE_BUILD_TYPE is defined as ${CMAKE_BUILD_TYPE}")
	elseif(DEFINED BUILD_TYPE)
		set(CMAKE_BUILD_TYPE ${BUILD_TYPE})
		message(NOTICE "CMAKE_BUILD_TYPE is set to BUILD_TYPE ENV ${CMAKE_BUILD_TYPE}")
	else()
		message(FATAL_ERROR "Please specify the CMAKE_BUILD_TYPE with -DCMAKE_BUILD_TYPE=")
	endif()
    #set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE})
	message(NOTICE "CMAKE_RUNTIME_OUTPUT_DIRECTORY is defined as ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

cmake_minimum_required(VERSION 3.16.4)

# Project configuration
project(ra8p1_doom
	VERSION 1.0.0
	LANGUAGES C CXX ASM)

set(CMAKE_CROSSCOMPILING "TRUE")
set(CMAKE_EXPORT_COMPILE_COMMANDS "TRUE")

include(${CMAKE_CURRENT_LIST_DIR}/Config.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratedCfg.cmake)

# If required user can amend RASC generated flags here, e.g. list(REMOVE_ITEM RASC_CMAKE_C_FLAGS "-Wall")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratedSrc.cmake)

# ---------------------------------------------------------------------------
# Doom engine (doomgeneric, GPL-2.0) - see doom/engine/README.doomgeneric.md
# Platform specific code lives in src/port (picked up by GeneratedSrc.cmake).
# ---------------------------------------------------------------------------
set(DOOM_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/doom/engine)
set(DOOM_ENGINE_SOURCES
    dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main
    d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom
    i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat
    m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy
    p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg
    p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw
    r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff
    s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone
    doomgeneric)
list(TRANSFORM DOOM_ENGINE_SOURCES PREPEND ${DOOM_ENGINE_DIR}/)
list(TRANSFORM DOOM_ENGINE_SOURCES APPEND .c)

target_sources(${PROJECT_NAME}.elf PRIVATE ${DOOM_ENGINE_SOURCES})
target_include_directories(${PROJECT_NAME}.elf PRIVATE ${DOOM_ENGINE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src/port)
# FEATURE_SOUND: sound effects go to USB headphones (src/port/i_sound_ra8.c, usb_audio.c)
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RA8_DOOM=1 FEATURE_SOUND=1)

# Third party code: silence warnings, allow GNU extensions and optimise for
# speed even in Debug builds (the renderer is the hot path).
set_source_files_properties(${DOOM_ENGINE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;-std=gnu99;-O3")
