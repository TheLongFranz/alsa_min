add_library(alsa_alsa INTERFACE)
add_library(alsa::alsa ALIAS alsa_alsa)

target_sources(alsa_alsa 
PUBLIC FILE_SET HEADERS
BASE_DIRS ${PROJECT_SOURCE_DIR}/include/
FILES ${PROJECT_SOURCE_DIR}/include/alsa/alsa.hpp)

set_property(TARGET alsa_alsa PROPERTY EXPORT_NAME ${PROJECT_NAME})

target_compile_features(alsa_alsa INTERFACE cxx_std_23)
