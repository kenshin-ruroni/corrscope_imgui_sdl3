#!/usr/bin/env bash

# Arrêter immédiatement le script en cas d'erreur
set -e

# Définition des couleurs pour l'affichage dans le terminal
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # Pas de couleur (Reset)

BUILD_DIR="build"

# 1. Gestion dynamique du type de Build (Debug par défaut)
BUILD_TYPE=${1:-"Debug"}

echo -e "${BLUE}[1/3] Vérification des outils de build...${NC}"

if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Erreur: CMake n'est pas installé.${NC}"
    exit 1
fi

if ! command -v ninja &> /dev/null; then
    echo -e "${RED}Erreur: Ninja n'est pas installé.${NC}"
    exit 1
fi

echo -e "${GREEN}Outils validés : CMake et Ninja sont présents.${NC}\n"

# 2. Utilisation d'un tableau Bash pour éviter les problèmes de découpage d'arguments
CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_COLOR_DIAGNOSTICS=ON
)

if [ "$BUILD_TYPE" = "Debug" ]; then
    echo -e "${BLUE}Configuration en mode DEBUG active.${NC}"
    # Utilisation des variables d'environnement standards lues par CMake
    export CXXFLAGS="-g3 -O0 -fopenmp -Wall -Wextra"
    echo -e "Flags appliqués : $CXXFLAGS\n"
elif [ "$BUILD_TYPE" = "Release" ]; then
    echo -e "${BLUE}Configuration en mode RELEASE active.${NC}"
    export CXXFLAGS="-O3 -march=native -fopenmp -DNDEBUG"
    echo -e "Flags appliqués : $CXXFLAGS\n"
else
    echo -e "${RED}Erreur: Mode inconnu '$BUILD_TYPE'. Utilisez 'Debug' ou 'Release'.${NC}"
    exit 1
fi

echo -e "${BLUE}[2/3] Configuration du projet avec CMake (Générateur: Ninja)...${NC}"
# Appel sécurisé avec le tableau Bash expansion
cmake "${CMAKE_ARGS[@]}"

echo -e "\n${BLUE}[3/3] Compilation du projet avec Ninja (Mode: ${BUILD_TYPE} | Verbeux)...${NC}"
# 1. On force le nettoyage des anciens fichiers objets pour obliger Ninja à tout réécrire
echo -e "${BLUE}Nettoyage des anciennes cibles...${NC}"
cmake --build "$BUILD_DIR" --target clean

# 2. On lance la compilation verbeuse
cmake --build "$BUILD_DIR" --verbose

echo -e "\n${GREEN}==================================================${NC}"
echo -e "${GREEN}  Compilation réussie avec succès (${BUILD_TYPE}) !${NC}"
echo -e "${GREEN}  L'exécutable se trouve dans : ./${BUILD_DIR}/AudioVisualizerSIMD${NC}"
echo -e "${GREEN}==================================================${NC}"
