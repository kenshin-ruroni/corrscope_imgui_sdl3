#!/usr/bin/env bash

# Arrêter immédiatement le script en cas d'erreur
set -e

# Définition des couleurs pour l'affichage dans le terminal
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # Pas de couleur (Reset)

BUILD_DIR="build"
BUILD_TYPE="Release"

echo -e "${BLUE}[1/3] Vérification des outils de build...${NC}"

# Vérifier si CMake est installé
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Erreur: CMake n'est pas installé. Veuillez l'installer avant de lancer le script.${NC}"
    exit 1
fi

# Vérifier si le générateur Ninja est installé
if ! command -v ninja &> /dev/null; then
    echo -e "${RED}Erreur: Ninja n'est pas installé. Veuillez installer 'ninja-build'.${NC}"
    exit 1
fi

echo -e "${GREEN}Outils validés : CMake et Ninja sont présents.${NC}\n"

echo -e "${BLUE}[2/3] Configuration du projet avec CMake (Générateur: Ninja)...${NC}"
# Créer le répertoire de build s'il n'existe pas, puis configurer
cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo -e "\n${BLUE}[3/3] Compilation du projet avec Ninja (Mode: ${BUILD_TYPE})...${NC}"
# Lancer la compilation (Ninja utilise automatiquement tous les cœurs CPU disponibles)
cmake --build "$BUILD_DIR"

echo -e "\n${GREEN}==================================================${NC}"
echo -e "${GREEN}  Compilation réussie avec succès !${NC}"
echo -e "${GREEN}  L'exécutable se trouve dans : ./${BUILD_DIR}/AudioVisualizerSIMD${NC}"
echo -e "${GREEN}==================================================${NC}"
