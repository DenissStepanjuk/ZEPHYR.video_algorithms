# Базовые файлы, которые нужны всегда
set(BASIC_FUNCTIONS_SOURCES
    video.c
)

# Если CONFIG_DISPLAY_TRUE=y
if(CONFIG_DISPLAY_TRUE)
    list(APPEND BASIC_FUNCTIONS_SOURCES
        display.c
    )
endif()

# Если CONFIG_SERVER_TRUE=y
if(CONFIG_SERVER_TRUE)
    list(APPEND BASIC_FUNCTIONS_SOURCES
        wifi.c
        dhcp.c
        server.c
    )
endif()

# Создаём статическую библиотеку с именем basic_functions.
add_library(basic_functions STATIC
    ${BASIC_FUNCTIONS_SOURCES}
)

# Регистрируем каталог с заголовочными файлами библиотеки.
target_include_directories(
    basic_functions
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Линкуем библиотеку с интерфейсной целью zephyr_interface.
# Через неё распространяются все общие флаги компиляции, определения препроцессора, include-пути ядра Zephyr.
target_link_libraries(basic_functions PUBLIC zephyr_interface)