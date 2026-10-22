
cmake_minimum_required(VERSION 3.24)
project(AudioVisualizerSIMD CXX)

# =================================================================
# 1. GESTION STRICTE DU MODE DE BUILD & PURGE DES DRAPEAUX PAR DÉFAUT
# =================================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Options: Debug Release" FORCE)
endif()

message(STATUS "🔧 Mode de build actif : ${CMAKE_BUILD_TYPE}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "🧼 Mode Debug : Purge totale des variables d'optimisation par défaut...")
    
    # Écrasement forcé des variables de cache pour empêcher la concaténation système
    set(CMAKE_CXX_FLAGS_DEBUG "" CACHE STRING "Flags Debug" FORCE)
    set(CMAKE_C_FLAGS_DEBUG "" CACHE STRING "Flags Debug" FORCE)
    
    # Injection stricte des drapeaux de débogage (ZÉRO -O3, ZÉRO -march=native)
    set(CMAKE_CXX_FLAGS_DEBUG "-g3 -O0 -Wall -Wextra" CACHE STRING "Flags Debug" FORCE)
else()
    # Mode Release : Optimisations maximales pour le DSP audio
    set(CMAKE_CXX_FLAGS_RELEASE "" CACHE STRING "Flags Release" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG" CACHE STRING "Flags Release" FORCE)
endif()

# Configuration globale commune
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-fdiagnostics-color=always" CACHE STRING "Flags Globaux" FORCE)

# =================================================================
# 2. RECHERCHE DES DEPENDANCES SYSTEME (SDL3 & OpenMP)
# =================================================================
# Recherche de OpenMP pour la vectorisation #pragma omp simd
find_package(OpenMP REQUIRED)

# Recherche de SDL3 installé sur le système
find_package(SDL3 REQUIRED)

# =================================================================
# 3. TELECHARGEMENT ET CONFIGURATION DE DEAR IMGUI (FetchContent)
# =================================================================
include(FetchContent)

# Téléchargement de Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY  https://github.com/ocornut/imgui.git
    GIT_TAG        master # À ajuster selon la version souhaitée
)
FetchContent_MakeAvailable(imgui)

# Téléchargement du FileBrowser pour ImGui
FetchContent_Declare(
    imgui_filebrowser
    GIT_REPOSITORY https://github.com/AirGuanZ/imgui-filebrowser.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(imgui_filebrowser)

# Création d'une bibliothèque statique pour ImGui afin d'isoler ses sources
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_lib PUBLIC 
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_filebrowser_SOURCE_DIR}
)

# Appliquer les drapeaux OpenMP à ImGui si nécessaire
target_link_libraries(imgui_lib PRIVATE OpenMP::OpenMP_CXX)

# =================================================================
# 4. CONFIGURATION DE L'EXECUTABLE PRINCIPAL
# =================================================================
# Déclaration de votre exécutable (cible finale appelée par build.sh)
add_executable(AudioVisualizerSIMD 
    main.cpp
)

# Inclusion des répertoires de votre projet
target_include_directories(AudioVisualizerSIMD PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Liens vers les bibliothèques indispensables
target_link_libraries(AudioVisualizerSIMD PRIVATE 
    imgui_lib
    SDL3::SDL3
    OpenMP::OpenMP_CXX
)

# Liens pour le rendu graphique (OpenGL sous Linux)
if(UNIX AND NOT APPLE)
    target_link_libraries(AudioVisualizerSIMD PRIVATE GL)
endif()

# =================================================================
# 5. NETTOYAGE CHIRURGICAL ET ASSIGNATION DES OPTIONS SUR LES CIBLES
# =================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Forcer la désactivation de toute optimisation résiduelle sur l'exécutable
    target_compile_options(AudioVisualizerSIMD PRIVATE -g3 -O0)
    
    # Forcer la désactivation de toute optimisation sur la lib ImGui
    target_compile_options(imgui_lib PRIVATE -g3 -O0)
    set_target_properties(imgui_lib PROPERTIES COMPILE_FLAGS "-g3 -O0")
endif()

