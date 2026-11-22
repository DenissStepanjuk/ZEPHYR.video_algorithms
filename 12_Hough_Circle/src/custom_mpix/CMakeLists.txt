# Базовые файлы, которые нужны всегда
set(CUSTOM_MPIX_SOURCES
    src/custom_mpix_color_KNN.c
    src/custom_mpix_combine.c
    src/custom_mpix_convolution.c
    src/custom_mpix_draw.c
    src/custom_mpix_Hough_transform_circle.c
    src/custom_mpix_Hough_transform_line.c
    src/custom_mpix_HSV.c
    src/custom_mpix_line_border_detect.c
)

# Создаём статическую библиотеку с именем custom_mpix.
add_library(custom_mpix STATIC
    ${CUSTOM_MPIX_SOURCES}
)

# Регистрируем каталог с заголовочными файлами библиотеки.
target_include_directories(
    custom_mpix
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Линкуем библиотеку с интерфейсной целью zephyr_interface.
# Через неё распространяются все общие флаги компиляции, определения препроцессора, include-пути ядра Zephyr.
target_link_libraries(custom_mpix PUBLIC zephyr_interface)