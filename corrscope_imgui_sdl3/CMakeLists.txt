cmake_minimum_required(VERSION 3.25)
project(AudioVisualizerSIMD VERSION 1.0.0 LANGUAGES C CXX)

# Force l'utilisation de la norme C++26
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options d'optimisation globales pour les performances audio/SIMD
if(MSVC)
    add_compile_options(/O2 /arch:AVX2 /openmp:experimental)
else()
    add_compile_options(-O3 -march=native -fopenmp-simd)
endif()

# ---------------------------------------------------------------------
# Recherche des dépendances locales (SDL3 et OpenGL)
# ---------------------------------------------------------------------
find_package(SDL3 REQUIRED)
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------
# Téléchargement automatique de Dear ImGui via FetchContent
# ---------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5 # Version stable recommandée (ou 'docking' selon vos besoins)
)

# Récupération du contenu du dépôt
FetchContent_MakeAvailable(imgui)

# ---------------------------------------------------------------------
# Configuration des fichiers du projet
# ---------------------------------------------------------------------

# On crée une bibliothèque intermédiaire pour compiler proprement les sources d'ImGui
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# Configuration des inclusions et liaisons de cette bibliothèque tierce
target_include_directories(imgui_lib PUBLIC 
    ${imgui_SOURCE_DIR} 
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PRIVATE SDL3::SDL3 OpenGL::GL)

# ---------------------------------------------------------------------
# Cible principale (Votre exécutable)
# ---------------------------------------------------------------------
set(PROJECT_SOURCES
    main.cpp
)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# Inclusion des répertoires locaux et de la bibliothèque ImGui créée
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Liaison de votre exécutable avec la bibliothèque ImGui et les composants système
target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui_lib
    SDL3::SDL3
    OpenGL::GL
)

# Activation d'OpenMP pour l'étape de linkage (requis hors MSVC)
if(NOT MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE -fopenmp-simd)
endif()
