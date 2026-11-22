# Устанавливаем минимально требуемую версию CMake для Zephyr.
cmake_minimum_required(VERSION 3.20.0)

# Подключаем пакет Zephyr: инициализирует всю сборочную инфраструктуру Zephyr.
# Создаём целевой объект app обладающий свойствами, зависимостями и правилами сборки.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
# Подключаем Python для Zephyr, так как он использует Python-скрипты для генерации кода.
find_package(Python REQUIRED COMPONENTS Interpreter)

# Задаём имя проекта.
project(correct_video)

# Задаём переменную gen_dir — путь к каталогу сгенерированных файлов.
# Куда Zephyr и его скрипты складывают сгенерированные заголовочные файлы.
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)


# Задаём путь к скрипту для преобразования jpg изображение в raw.inc массив данных.
set(LOGO_CONVERT_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/src/custom_mpix/scripts/convert_logo.py)
# Задаём путь к jpg изображению.
set(LOGO_SOURCE_IMAGE   ${CMAKE_CURRENT_LIST_DIR}/src/image/logo.png)
# Задаём путь к raw.inc массиву данных.
set(LOGO_OUTPUT_INC     ${gen_dir}/logo.raw.inc)
# Задаём ширину и высоту изображения.
set(LOGO_WIDTH  57)
set(LOGO_HEIGHT 33)

# Преобразуем jpg изображение в raw.inc массив данных.
add_custom_command(OUTPUT ${LOGO_OUTPUT_INC}
    COMMAND ${Python_EXECUTABLE} ${LOGO_CONVERT_SCRIPT}
            ${LOGO_SOURCE_IMAGE} ${LOGO_OUTPUT_INC}
            --width ${LOGO_WIDTH} --height ${LOGO_HEIGHT}
    DEPENDS ${LOGO_SOURCE_IMAGE} ${LOGO_CONVERT_SCRIPT}
    COMMENT "Converting ${LOGO_SOURCE_IMAGE} to RGB24 .inc (${LOGO_WIDTH}x${LOGO_HEIGHT})"
)


# Добавляем исходный файл src/main.c в целевой объект app.
target_sources(app PRIVATE src/main.c
                           src/thread_processing.c
                           ${LOGO_OUTPUT_INC})

# Добавление подкаталога с собственной библиотекой в процесс сборки.
add_subdirectory(src/basic_functions)

# Линковка библиотеки с приложением связывает библиотеку с app.
target_link_libraries(app PRIVATE basic_functions)

# Добавление подкаталога с собственной библиотекой в процесс сборки.
add_subdirectory(src/custom_mpix)

# Линковка библиотеки с приложением связывает библиотеку с app.
target_link_libraries(app PRIVATE custom_mpix)

# Устанавливаем переменную gen_dir — путь к каталогу сгенерированных файлов.
# Куда Zephyr и его скрипты складывают сгенерированные заголовочные файлы.
# set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)


if(CONFIG_SERVER_TRUE)
  # Опция INCLUDE_HTML_CONTENT это CMake-переменная, которая будет использована
  # для условного включения HTML-файлов в прошивку.
  option(INCLUDE_HTML_CONTENT "Include the HTML content" ON)

  # Добавляем фрагмент скрипта компоновщика в общую сборку.
  # Этот файл определяет итерируемую ROM-секцию для дескрипторов HTTP-ресурсов.
  # Без этого файла HTTP-сервер не сможет найти зарегистрированные статические ресурсы.
  zephyr_linker_sources(SECTIONS src/static_web_resources/sections-rom.ld)

  # Zephyr создаёт специальную область памяти, где будут храниться HTTP ресурсы.
  zephyr_linker_section_ifdef(CONFIG_NET_VIDEO_HTTP_SERVICE
    NAME http_resource_desc_video_http_service
    KVMA RAM_REGION GROUP RODATA_REGION
  )

  # Цикл по списку web-ресурсов для их сжатия и дальнейшего преобразование в C-заголовочный файл с байтовым массивом.
  foreach(web_resource index.html)
    generate_inc_file_for_target(app
      src/static_web_resources/${web_resource}
      ${gen_dir}/${web_resource}.gz.inc
      --gzip
    )
  endforeach()
endif()

# Функция подключает все необходимые .c-файлы библиотеки libmpix к проекту.
libmpix_init(app)