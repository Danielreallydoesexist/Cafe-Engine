# Each examples/<game>/main.uml folder becomes its own Aroma .wuhb.
# Optional files in the game folder:
#   wuhb.meta   name= / short_name= / author=
#   icon.png    128x128
#   tv.png      1280x720 splash
#   drc.png     854x480 splash

function(cafe_read_wuhb_meta META_FILE NAME_VAR SHORT_VAR AUTHOR_VAR)
    if(NOT EXISTS "${META_FILE}")
        return()
    endif()
    file(STRINGS "${META_FILE}" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^[ \t]*#" OR _line MATCHES "^[ \t]*$")
            continue()
        endif()
        if(_line MATCHES "^[ \t]*name[ \t]*=[ \t]*(.*)$")
            set(${NAME_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        elseif(_line MATCHES "^[ \t]*short_name[ \t]*=[ \t]*(.*)$")
            set(${SHORT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        elseif(_line MATCHES "^[ \t]*author[ \t]*=[ \t]*(.*)$")
            set(${AUTHOR_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

function(cafe_add_wiiu_game GAME_DIR)
    get_filename_component(GAME_ID "${GAME_DIR}" NAME)
    string(REPLACE "_" " " GAME_NAME "${GAME_ID}")
    set(GAME_SHORT "${GAME_NAME}")
    set(GAME_AUTHOR "Cafe Engine")
    cafe_read_wuhb_meta("${GAME_DIR}/wuhb.meta" GAME_NAME GAME_SHORT GAME_AUTHOR)

    set(_icon_args)
    set(_tv_args)
    set(_drc_args)
    if(EXISTS "${GAME_DIR}/icon.png")
        set(_icon_args ICON "${GAME_DIR}/icon.png")
    endif()
    if(EXISTS "${GAME_DIR}/tv.png")
        set(_tv_args TVSPLASH "${GAME_DIR}/tv.png")
    endif()
    if(EXISTS "${GAME_DIR}/drc.png")
        set(_drc_args DRCSPLASH "${GAME_DIR}/drc.png")
    endif()

    # Passing a directory directly to wut_create_wuhb does not make its files
    # build dependencies. Track every bundled file so script, markup, style,
    # and asset edits always produce a fresh WUHB.
    file(GLOB_RECURSE _game_content
         CONFIGURE_DEPENDS
         LIST_DIRECTORIES false
         RELATIVE "${GAME_DIR}"
         "${GAME_DIR}/*")
    set(_content_target "${GAME_ID}_content")
    dkp_add_asset_target("${_content_target}" "${GAME_DIR}")
    dkp_track_assets("${_content_target}" FILES ${_game_content})

    wut_create_wuhb(${GAME_ID}
        TARGET cafe
        CONTENT "${_content_target}"
        NAME "${GAME_NAME}"
        SHORTNAME "${GAME_SHORT}"
        AUTHOR "${GAME_AUTHOR}"
        ${_icon_args}
        ${_tv_args}
        ${_drc_args}
    )

    set(GAME_META_XML "${CMAKE_CURRENT_BINARY_DIR}/meta/${GAME_ID}.xml")
    configure_file("${CMAKE_SOURCE_DIR}/platform/wiiu/meta.xml.in" "${GAME_META_XML}" @ONLY)

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${GAME_ID}.wuhb" DESTINATION .)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cafe.rpx" DESTINATION "${GAME_ID}" RENAME "${GAME_ID}.rpx")
    install(FILES "${GAME_META_XML}" DESTINATION "${GAME_ID}" RENAME meta.xml)
    install(FILES
            "${GAME_DIR}/main.uml"
            "${GAME_DIR}/main.cstyle"
            "${GAME_DIR}/main.cafe"
            DESTINATION "${GAME_ID}")
    if(IS_DIRECTORY "${GAME_DIR}/assets")
        install(DIRECTORY "${GAME_DIR}/assets" DESTINATION "${GAME_ID}")
    endif()
    if(EXISTS "${GAME_DIR}/icon.png")
        install(FILES "${GAME_DIR}/icon.png" DESTINATION "${GAME_ID}")
    endif()
endfunction()

function(cafe_add_wiiu_games EXAMPLES_ROOT)
    file(GLOB _game_dirs LIST_DIRECTORIES true "${EXAMPLES_ROOT}/*")
    set(_count 0)
    foreach(_dir IN LISTS _game_dirs)
        if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/main.uml")
            cafe_add_wiiu_game("${_dir}")
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    if(_count EQUAL 0)
        message(WARNING "Cafe Engine: no examples/*/main.uml games found to pack as WUHB")
    endif()
endfunction()
