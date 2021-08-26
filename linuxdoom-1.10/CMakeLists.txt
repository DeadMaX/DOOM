cmake_minimum_required(VERSION 3.4)

project(Doom C)

add_executable(cppdoom

am_map.c         doomdef.c        doomstat.c       dstrings.c
d_items.c        d_main.c         d_net.c          f_finale.c       f_wipe.c         g_game.c         hu_lib.c
hu_stuff.c       info.c           i_main.c         i_net.c          i_sound.c        i_system.c       i_video.c
m_argv.c         m_bbox.c         m_cheat.c        m_fixed.c        m_menu.c         m_misc.c         m_random.c
m_swap.c         p_ceilng.c       p_doors.c        p_enemy.c        p_floor.c        p_inter.c
p_lights.c       p_map.c          p_maputl.c       p_mobj.c         p_plats.c        p_pspr.c         p_saveg.c
p_setup.c        p_sight.c        p_spec.c         p_switch.c       p_telept.c       p_tick.c         p_user.c
r_bsp.c          r_data.c         r_draw.c         r_main.c         r_plane.c        r_segs.c         r_sky.c
r_things.c       sounds.c         st_lib.c         st_stuff.c       s_sound.c        tables.c         v_video.c
wi_stuff.c       w_wad.c          z_zone.c
 
)

set_property(TARGET cppdoom PROPERTY C_STANDARD 11)

target_compile_definitions (cppdoom PRIVATE NORMALUNIX)
if (MSVC)
	target_compile_options(cppdoom PUBLIC -nologo)
	target_compile_definitions(cppdoom PUBLIC _CRT_SECURE_NO_WARNINGS)
	target_link_libraries(cppdoom PUBLIC wsock32 ws2_32 winmm)
endif()
