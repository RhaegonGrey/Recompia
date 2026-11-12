# SPDX-License-Identifier: GPL-3.0-or-later
# The Pokémon Pokopia game module (docs/GAME_BOUNDARY.md, pkp_game.h). Libraries, each a superset of the one before:
#   nxrt_game_pkp_data   identity, branding, factory controls.ini, the [pkp] key table (pure data)
#   nxrt_game_pkp_core   + title facts, test probes (links nxrt)
#   nxrt_game_pkp        + play-mode hooks (links nxrt_gfx; only with NXRT_BUILD_GFX). pkp.exe links this one.
set(_pkp "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")
set(NXRT_GAME_PKP_INCLUDES "${_rt}/include" "${_rt}/games" "${_pkp}" "${_rt}/src/gfx" "${_rt}/src/gfx/play")
if(NXRT_TESTTOOLS)
    list(APPEND NXRT_GAME_PKP_INCLUDES "${_rt}/src/testtools")   # test/pkp_tt_probe.cpp
endif()

add_library(nxrt_game_pkp_data STATIC
    "${_pkp}/core/pkp_data.cpp"
    "${_pkp}/core/pkp_settings_table.cpp")
target_include_directories(nxrt_game_pkp_data PUBLIC ${NXRT_GAME_PKP_INCLUDES})
# release builds: every gameplay mod switch of the [pkp] table defaults on unless opt_in, or the table does not compile (pkp_settings_table.cpp)
option(PKP_GAMEPLAY_DEFAULTS_CHECK "refuse a [pkp] gameplay mod that defaults off (game-build sets it with PKP_RELEASE)" OFF)
target_compile_definitions(nxrt_game_pkp_data PRIVATE PKP_GAMEPLAY_DEFAULTS_CHECK=$<BOOL:${PKP_GAMEPLAY_DEFAULTS_CHECK}>)
target_compile_features(nxrt_game_pkp_data PUBLIC cxx_std_20)

add_library(nxrt_game_pkp_core STATIC "${_pkp}/core/pkp_core.cpp")
if(NXRT_TESTTOOLS)
    target_sources(nxrt_game_pkp_core PRIVATE "${_pkp}/test/pkp_tt_probe.cpp")   # the test tooling's probes (registered in pkp_core.cpp)
endif()
target_link_libraries(nxrt_game_pkp_core PUBLIC nxrt_game_pkp_data nxrt)

if(TARGET nxrt_gfx)
    add_library(nxrt_game_pkp STATIC
        # core
        "${_pkp}/core/pkp_game.cpp"
        "${_pkp}/core/pkp_linkplay.cpp"
        "${_pkp}/core/pkp_save_uuid.cpp"
        "${_pkp}/core/pkp_mods.cpp"
        "${_pkp}/core/pkp_mods_notice.cpp"
        "${_pkp}/core/pkp_pokemon_cap.cpp"
        "${_pkp}/core/pkp_resident_census.cpp"
        "${_pkp}/core/pkp_settings.cpp"
        "${_pkp}/core/pkp_view_offset.cpp"
        # render
        "${_pkp}/render/pkp_render_size.cpp"
        "${_pkp}/render/pkp_render_targets.cpp"
        "${_pkp}/render/pkp_aspect.cpp"
        "${_pkp}/render/pkp_wide_text.cpp"
        "${_pkp}/render/pkp_frame_rate.cpp"
        "${_pkp}/render/pkp_gfx_heap.cpp"
        "${_pkp}/render/pkp_quality.cpp"
        "${_pkp}/render/pkp_taa.cpp"
        "${_pkp}/render/pkp_ui_backdrop.cpp"
        "${_pkp}/render/pkp_shoulder_camera.cpp"
        "${_pkp}/render/pkp_view_projection.cpp"
        # input
        "${_pkp}/input/pkp_play.cpp"
        "${_pkp}/input/pkp_mouse_camera.cpp"
        "${_pkp}/input/pkp_mouse_mode.cpp"
        "${_pkp}/input/pkp_aim.cpp"
        "${_pkp}/input/pkp_aim_debug.cpp"
        "${_pkp}/input/pkp_jump.cpp"
        "${_pkp}/input/pkp_kbm.cpp"
        # ui
        "${_pkp}/ui/pkp_menu.cpp"
        "${_pkp}/ui/pkp_menu_scan.cpp"
        "${_pkp}/ui/pkp_menu_draw.cpp"
        "${_pkp}/ui/pkp_menu_pick.cpp"
        "${_pkp}/ui/pkp_menu_in_game.cpp"
        "${_pkp}/ui/pkp_options.cpp"
        "${_pkp}/ui/pkp_options_model.cpp"
        "${_pkp}/ui/pkp_options_controller.cpp"
        "${_pkp}/ui/pkp_options_capture.cpp"
        "${_pkp}/ui/pkp_options_mods.cpp"
        "${_pkp}/ui/pkp_options_rows.cpp"
        "${_pkp}/ui/pkp_options_time.cpp"
        "${_pkp}/ui/pkp_options_items.cpp"
        "${_pkp}/ui/pkp_key_guide.cpp"
        "${_pkp}/ui/pkp_title.cpp"
        "${_pkp}/ui/pkp_sort.cpp"
        "${_pkp}/ui/pkp_craft.cpp"
        "${_pkp}/ui/pkp_item_edit.cpp"
        "${_pkp}/ui/pkp_time.cpp")
    if(NXRT_TESTTOOLS)
        target_sources(nxrt_game_pkp PRIVATE
            "${_pkp}/test/pkp_movie_test.cpp"
            "${_pkp}/test/pkp_taa_probe.cpp")   # test-only features (pkp_game.cpp, NXRT_TESTTOOLS)
    endif()
    target_link_libraries(nxrt_game_pkp PUBLIC nxrt_game_pkp_core nxrt_gfx PRIVATE nxrt_miniz)
    target_sources(nxrt_game_pkp PRIVATE "${_pkp}/render/pkp_render_info.cpp")   # game::Info::render (nxrt/game_render.h)
endif()
