# KK_UI 运行时（MIT，青岛波特律动科技有限公司），已按 160x80 调整布局常量
idf_component_register(SRCS "src/kk_ui.c"
                            "src/kk_ui_anim.c"
                            "src/kk_ui_dialog.c"
                            "src/kk_ui_draw.c"
                            "src/kk_ui_home.c"
                            "src/kk_ui_menu.c"
                            "src/kk_ui_nav.c"
                            "src/kk_ui_page.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES kk_oled)

# ST7735 驱动只实现阻塞刷新；KK_UI_REFRESH_BLOCKING = 0
target_compile_definitions(${COMPONENT_LIB} PUBLIC KK_UI_REFRESH_MODE=0)
