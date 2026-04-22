cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The common_components folder from the manufacturer's SDK
# must be placed alongside this project directory:
#   some_folder/
#     common_components/    ← from manufacturer SDK
#     td5_speedo/           ← this project
set(EXTRA_COMPONENT_DIRS
    ../common_components
)

add_compile_options(-Wno-format)
add_compile_options(-Wno-ignored-qualifiers)
add_compile_options(-Wno-attributes)
add_compile_options(-Wno-unused-function)

project(td5_speedo)
