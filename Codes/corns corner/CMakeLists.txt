SET(SOURCES screens/ui_Screen1.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    images/ui_img_waterdroplet_white_png.c
    images/ui_img_temp_icon_png.c
    images/ui_img_humidity_icon_png.c
    images/ui_img_sun_white_png.c)

add_library(ui ${SOURCES})
