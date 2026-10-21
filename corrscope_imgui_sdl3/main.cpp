#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <iostream>

// Inclusion du visualiseur audio qui gère l'UI et le décodeur
#include "AudioVisualizerWithTrigger.hpp"

int main(int argc, char* argv[]) {
    // 1. Initialisation de SDL3
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO )) {
        std::cerr << "Erreur SDL_Init : " << SDL_GetError() << std::endl;
        return -1;
    }

    // Configuration des attributs OpenGL (Version 3.3 Core Profile)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // 2. Création de la fenêtre SDL3
    uint32_t window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("Oscilloscope Audio SIMD - SDL3 + ImGui", 1280, 720, window_flags);
    if (!window) {
        std::cerr << "Erreur SDL_CreateWindow : " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "Erreur SDL_GL_CreateContext : " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Activation du VSync (Synchronisation verticale)
    SDL_ShowWindow(window);

    // 3. Initialisation du contexte Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Navigation au clavier

    // Application du style visuel sombre (Dark Theme)
    ImGui::StyleColorsDark();

    // Configuration des plateformes de rendu (Bindings SDL3 + OpenGL3)
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Instanciation de notre visualiseur audio
    AudioVisualizerWithTrigger visualizer;

    // 4. Boucle de rendu principale
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Transmission des événements SDL3 à ImGui
            ImGui_ImplSDL3_ProcessEvent(&event);
            
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        // Démarrage d'une nouvelle frame ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Appel de la méthode de rendu de notre interface graphique
        visualizer.RenderUI();

        // Rendu final d'ImGui
        ImGui::Render();
        
        // Configuration de la zone d'affichage (Viewport) OpenGL
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        
        // Nettoyage de l'écran (Arrière-plan gris foncé)
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Envoi des géométries d'ImGui au GPU
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Échange des buffers d'affichage (Double Buffering)
        SDL_GL_SwapWindow(window);
    }

    // 5. Nettoyage de la mémoire et fermeture
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
