idf_component_register(
    SRCS "main.c" "td5_usb.c" "td5_display.c"
         "fonts/font_arial_bold_16.c"
         "fonts/font_dseg7_bold_270.c"
         "fonts/font_montserrat_bold_72.c"
    INCLUDE_DIRS "."
    REQUIRES td5_protocol espressif__esp32_p4_function_ev_board
)
