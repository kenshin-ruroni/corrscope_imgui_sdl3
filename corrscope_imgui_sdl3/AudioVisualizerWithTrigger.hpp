#pragma once

#include "imgui.h"
#include "imfilebrowser.h"
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <algorithm>
#include <optional>
#include <memory>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION

#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"

#include "TriggerBufferUpdate.hpp"
#include "CrossCorrelation26.hpp"
#include "NodalReverbProcessor.hpp"
#include "NodalFDN2x2Processor.hpp"
#include "NodalFDN4x4Processor.hpp"

// Moteurs d'overdrive nodaux
#include "NodalOverdriveStatic.hpp"
#include "NodalOverdriveShockley.hpp"
#include "NodalOverdriveTriode.hpp"

// Structure pour stocker le cache brut et les métadonnées en RAM
struct AudioTrack
{
    std::string filePath;
    std::string fileName;
    unsigned int sampleRate = 0;
    unsigned int channels = 0;
    unsigned int bitDepth = 0;
    unsigned long long totalFrames = 0;
    std::string durationStr;

    // Cache RAM permanent des canaux d'origine avant DSP
    std::vector<float> originalChannelLeft;
    std::vector<float> originalChannelRight;

    void ComputeDurationString()
    {
        if (sampleRate == 0)
        {
            durationStr = "00:00";
            return;
        }
        unsigned long long totalSeconds = totalFrames / sampleRate;
        unsigned int minutes = static_cast<unsigned int>(totalSeconds / 60);
        unsigned int seconds = static_cast<unsigned int>(totalSeconds % 60);

        std::stringstream ss;
        ss << std::setw(2) << std::setfill('0') << minutes << ":"
           << std::setw(2) << std::setfill('0') << seconds;
        durationStr = ss.str();
    }
};

class AudioVisualizerWithTrigger
{
private:
    // Tampons de l'oscilloscope d'affichage
    std::vector<float> channelLeft;
    std::vector<float> channelRight;

    unsigned int sampleRate = 0;
    unsigned int channelsCount = 0;
    unsigned long long totalSampleCount = 0;
    std::string currentFilePath = "Aucun fichier chargé";
    bool isAudioLoaded = false;

    std::unique_ptr<CorrelationTrigger> triggerLeft;
    std::unique_ptr<CorrelationTrigger> triggerRight;

    // Moteurs DSP : Réverbération
    std::unique_ptr<NodalReverbProcessor> reverbLeft;
    std::unique_ptr<NodalReverbProcessor> reverbRight;
    std::unique_ptr<NodalFDN2x2Processor> fdn2x2Processor;
    std::unique_ptr<NodalFDN4x4Processor> fdn4x4Processor;

    // Moteurs DSP : Overdrive/Saturation
    std::unique_ptr<NodalOverdriveStatic> overdriveStatic;
    std::unique_ptr<NodalOverdriveShockley> overdriveShockley;
    std::unique_ptr<NodalOverdriveTriode> overdriveTriode;

    int currentPlaybackPos = 0;
    int viewWindowSize = 2048;
    int samplesPerFrame = 64;
    bool isPlaybackPaused = false;

    // Repères de la Combo Box (7 choix d'effets)
    int selectedEffectIndex = 0;
    const char *effectNames[7] = {
        "Aucun",
        "Reverb Comb",
        "Reverb FDN 2x2",
        "Reverb FDN 4x4 (Studio Pro)",
        "Overdrive Statique (SIMD Pure)",
        "Overdrive Shockley (Diodes)",
        "Overdrive Triode (Lampes 12AX7)"};

    // Configuration UI Réverbérations
    int uiReverbDelay = 3500;
    float uiReverbFeedback = 0.65f;
    int uiFdnDelayBase = 2800;
    float uiFdnFeedback = 0.70f;

    // Configuration UI Saturations
    float uiDriveGain = 2.0f;
    float uiAsymmetry = 0.1f;
    float uiTriodeBias = -1.2f;
    float uiWetMix = 0.7f;

    // Mémoires de traitement persistantes
    std::vector<float> originalInterleavedBuffer;
    std::vector<float> currentProcessedBuffer;

    int uiTsamp = 1024;
    int uiStride = 1;

    SDL_AudioStream *audioStream = nullptr;
    ImGui::FileBrowser fileDialog;

    std::vector<AudioTrack> playlist;
    int selectedTrackIndex = -1;

    // Variables d'infrastructure Multi-Thread et Synchronisation
    std::thread loadingThread;
    std::atomic<bool> isFileLoading{false};
    std::string lastLoadingError = "";

    std::mutex m_triggerMutex;                       // Verrou d'isolement matériel global
    std::unordered_set<size_t> m_loadedTracksHashes; // Index anti-doublon O(1)

    alignas(64) std::atomic<bool> m_hasNewTrackToProcess{false}; // Prévention du False Sharing

    void CleanAudioStream()
    {
        if (audioStream)
        {
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
    }

    void ReallocateTriggers()
    {
        if (!isAudioLoaded)
            return;
        if (uiTsamp < 2)
            uiTsamp = 2;
        if (uiTsamp % 2 != 0)
            uiTsamp++;
        if (uiStride < 1)
            uiStride = 1;

        triggerLeft = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
        triggerRight = std::make_unique<CorrelationTrigger>(uiTsamp, uiStride, samplesPerFrame, 0.5f, 0.3f, 1.0f);
    }

    void InternalLoadTrack(const std::string filename)
    {
        std::string ext = filename.substr(filename.find_last_of(".") + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        AudioTrack track;
        track.filePath = filename;
        track.fileName = filename.substr(filename.find_last_of("/\\") + 1);

        std::vector<float> tempInterleavedBuffer;

        if (ext == "wav")
        {
            drwav wav;
            if (!drwav_init_file(&wav, filename.c_str(), nullptr))
            {
                lastLoadingError = "Impossible d'initialiser le fichier WAV.";
                isFileLoading = false;
                return;
            }
            unsigned long long totalSamples = wav.totalPCMFrameCount * wav.channels;
            if (wav.totalPCMFrameCount == 0 || wav.channels == 0 || totalSamples > 1200000000ULL)
            {
                lastLoadingError = "Taille WAV invalide.";
                drwav_uninit(&wav);
                isFileLoading = false;
                return;
            }

            tempInterleavedBuffer.resize(totalSamples);
            drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, tempInterleavedBuffer.data());
            track.sampleRate = wav.sampleRate;
            track.channels = wav.channels;
            track.bitDepth = wav.bitsPerSample;
            track.totalFrames = wav.totalPCMFrameCount;
            drwav_uninit(&wav);
        }
        else if (ext == "flac")
        {
            drflac *pFlac = drflac_open_file(filename.c_str(), nullptr);
            if (!pFlac)
            {
                lastLoadingError = "Impossible d'ouvrir le fichier FLAC.";
                isFileLoading = false;
                return;
            }
            unsigned long long totalSamples = pFlac->totalPCMFrameCount * pFlac->channels;
            if (pFlac->totalPCMFrameCount == 0 || pFlac->channels == 0 || totalSamples > 1200000000ULL)
            {
                lastLoadingError = "Taille FLAC invalide.";
                drflac_close(pFlac);
                isFileLoading = false;
                return;
            }

            tempInterleavedBuffer.resize(totalSamples);
            drflac_read_pcm_frames_f32(pFlac, pFlac->totalPCMFrameCount, tempInterleavedBuffer.data());
            track.sampleRate = pFlac->sampleRate;
            track.channels = pFlac->channels;
            track.bitDepth = pFlac->bitsPerSample;
            track.totalFrames = pFlac->totalPCMFrameCount;
            drflac_close(pFlac);
        }
        else if (ext == "mp3")
        {
            drmp3 mp3;
            if (!drmp3_init_file(&mp3, filename.c_str(), nullptr))
            {
                lastLoadingError = "Impossible d'initialiser le fichier MP3.";
                isFileLoading = false;
                return;
            }
            uint64_t totalFrames = drmp3_get_pcm_frame_count(&mp3);
            unsigned long long totalSamples = totalFrames * mp3.channels;
            if (totalFrames == 0 || mp3.channels == 0 || totalSamples > 1200000000ULL)
            {
                lastLoadingError = "Taille MP3 invalide.";
                drmp3_uninit(&mp3);
                isFileLoading = false;
                return;
            }

            tempInterleavedBuffer.resize(totalSamples);
            drmp3_read_pcm_frames_f32(&mp3, totalFrames, tempInterleavedBuffer.data());
            track.sampleRate = mp3.sampleRate;
            track.channels = mp3.channels;
            track.bitDepth = 0;
            track.totalFrames = totalFrames;
            drmp3_uninit(&mp3);
        }
        else
        {
            lastLoadingError = "Format d'extension non supporté.";
            isFileLoading = false;
            return;
        }

        track.ComputeDurationString();

        // Séparation et extraction des canaux bruts d'origine dans le cache RAM
        track.originalChannelLeft.reserve(track.totalFrames);
        if (track.channels >= 2)
            track.originalChannelRight.reserve(track.totalFrames);

        for (unsigned long long i = 0; i < track.totalFrames; ++i)
        {
            if (track.channels >= 2)
            {
                track.originalChannelLeft.push_back(tempInterleavedBuffer[i * track.channels]);
                track.originalChannelRight.push_back(tempInterleavedBuffer[i * track.channels + 1]);
            }
            else
            {
                track.originalChannelLeft.push_back(tempInterleavedBuffer[i]);
            }
        }
        tempInterleavedBuffer.clear();
        tempInterleavedBuffer.shrink_to_fit();

        // Enregistrement atomique sécurisé sous verrou
        {
            std::lock_guard<std::mutex> lock(m_triggerMutex);
            size_t pathHash = std::hash<std::string>{}(filename);

            auto it = std::find_if(playlist.begin(), playlist.end(), [&](const AudioTrack &t)
                                   { return t.filePath == filename; });
            if (it == playlist.end())
            {
                playlist.push_back(std::move(track));
                m_loadedTracksHashes.insert(pathHash);
                selectedTrackIndex = static_cast<int>(playlist.size()) - 1;
            }
            else
            {
                *it = std::move(track);
                selectedTrackIndex = static_cast<int>(std::distance(playlist.begin(), it));
            }
        }

        m_hasNewTrackToProcess.store(true);
        isFileLoading = false;
    }

    void ExecuteDSPPipeline(unsigned long long frameCount, unsigned int channels)
    {
        if (selectedEffectIndex == 0)
            return;

        if (selectedEffectIndex == 1)
        {
            if (channels >= 2)
            {
                std::vector<float> tempL(frameCount), tempR(frameCount);
                for (size_t i = 0; i < frameCount; ++i)
                {
                    tempL[i] = currentProcessedBuffer[i * channels];
                    tempR[i] = currentProcessedBuffer[i * channels + 1];
                }
                reverbLeft->ProcessNodalBufferSIMD(tempL.data(), frameCount);
                reverbRight->ProcessNodalBufferSIMD(tempR.data(), frameCount);
                for (size_t i = 0; i < frameCount; ++i)
                {
                    currentProcessedBuffer[i * channels] = tempL[i];
                    currentProcessedBuffer[i * channels + 1] = tempR[i];
                }
            }
            else
            {
                reverbLeft->ProcessNodalBufferSIMD(currentProcessedBuffer.data(), frameCount);
            }
        }
        else if (selectedEffectIndex == 2)
        {
            if (channels >= 2)
            {
                std::vector<float> tempL(frameCount), tempR(frameCount);
                for (size_t i = 0; i < frameCount; ++i)
                {
                    tempL[i] = currentProcessedBuffer[i * channels];
                    tempR[i] = currentProcessedBuffer[i * channels + 1];
                }
                fdn2x2Processor->ProcessFDNBufferSIMD(tempL.data(), tempR.data(), frameCount);
                for (size_t i = 0; i < frameCount; ++i)
                {
                    currentProcessedBuffer[i * channels] = tempL[i];
                    currentProcessedBuffer[i * channels + 1] = tempR[i];
                }
            }
            else
            {
                std::vector dummyR(frameCount, 0.0f);
                fdn2x2Processor->ProcessFDNBufferSIMD(currentProcessedBuffer.data(), dummyR.data(), frameCount);
            }
        }
        else if (selectedEffectIndex == 3)
        {
            if (channels >= 2)
            {
                std::vector<float> tempL(frameCount), tempR(frameCount);
                for (size_t i = 0; i < frameCount; ++i)
                {
                    tempL[i] = currentProcessedBuffer[i * channels];
                    tempR[i] = currentProcessedBuffer[i * channels + 1];
                }
                fdn4x4Processor->ProcessFDN4x4BufferSIMD(tempL.data(), tempR.data(), frameCount);
                for (size_t i = 0; i < frameCount; ++i)
                {
                    currentProcessedBuffer[i * channels] = tempL[i];
                    currentProcessedBuffer[i * channels + 1] = tempR[i];
                }
            }
            else
            {
                std::vector dummyR(frameCount, 0.0f);
                fdn4x4Processor->ProcessFDN4x4BufferSIMD(currentProcessedBuffer.data(), dummyR.data(), frameCount);
            }
        }
        else if (selectedEffectIndex == 4)
        {
            overdriveStatic->ProcessSIMD(currentProcessedBuffer.data(), currentProcessedBuffer.size());
        }
        else if (selectedEffectIndex == 5)
        {
            overdriveShockley->ProcessSIMD(currentProcessedBuffer.data(), currentProcessedBuffer.size());
        }
        else if (selectedEffectIndex == 6)
        {
            overdriveTriode->ProcessSIMD(currentProcessedBuffer.data(), currentProcessedBuffer.size());
        }
    }

public:
    AudioVisualizerWithTrigger()
    {
        fileDialog.SetTitle("Ajouter un morceau à la liste");
        fileDialog.SetTypeFilters({".wav", ".flac", ".mp3"});
        reverbLeft = std::make_unique<NodalReverbProcessor>(3500, 0.65f);
        reverbRight = std::make_unique<NodalReverbProcessor>(3850, 0.65f);
        fdn2x2Processor = std::make_unique<NodalFDN2x2Processor>(2800, 4530, 0.70f);
        fdn4x4Processor = std::make_unique<NodalFDN4x4Processor>(2800, 3668, 4564, 5908, 0.70f);

        overdriveStatic = std::make_unique<NodalOverdriveStatic>(uiDriveGain, uiAsymmetry, uiWetMix);
        overdriveShockley = std::make_unique<NodalOverdriveShockley>(uiDriveGain, uiWetMix);
        overdriveTriode = std::make_unique<NodalOverdriveTriode>(uiDriveGain, uiTriodeBias, uiWetMix);
    }
    ~AudioVisualizerWithTrigger()
    {
        if (loadingThread.joinable())
            loadingThread.join();
        CleanAudioStream();
    }
    bool StartAsyncLoad(const std::string &filename)
    {
        if (isFileLoading)
            return false;
        size_t pathHash = std::hash<std::string>{}(filename);
        {
            std::lock_guard<std::mutex> lock(m_triggerMutex);
            if (m_loadedTracksHashes.find(pathHash) != m_loadedTracksHashes.end())
            {
                return false;
            }
        }
        lastLoadingError = "";
        isFileLoading = true;

        loadingThread = std::thread(&AudioVisualizerWithTrigger::InternalLoadTrack, this, filename);
        loadingThread.detach();
        return true;
    }
    void SwitchToTrack(int index)
    {
        if (index < 0 || index >= playlist.size())
            return;
        std::lock_guard<std::mutex> lock(m_triggerMutex);
        selectedTrackIndex = index;
        const auto &track = playlist[index];
        CleanAudioStream();
        sampleRate = track.sampleRate;
        channelsCount = track.channels;
        totalSampleCount = track.totalFrames;
        currentFilePath = track.fileName;
        originalInterleavedBuffer.clear();
        originalInterleavedBuffer.reserve(totalSampleCount * channelsCount);
        for (unsigned long long i = 0; i < totalSampleCount; ++i)
        {
            if (channelsCount >= 2)
            {
                originalInterleavedBuffer.push_back(track.originalChannelLeft[i]);
                originalInterleavedBuffer.push_back(track.originalChannelRight[i]);
            }
            else
            {
                originalInterleavedBuffer.push_back(track.originalChannelLeft[i]);
            }
        }
        currentProcessedBuffer = originalInterleavedBuffer;
        reverbLeft->ResetNodalCircuit();
        reverbRight->ResetNodalCircuit();
        fdn2x2Processor->Reset();
        fdn4x4Processor->Reset();
        ExecuteDSPPipeline(totalSampleCount, channelsCount);
        channelLeft = track.originalChannelLeft;
        channelRight = track.originalChannelRight;
        if (selectedEffectIndex > 0)
        {
            channelLeft.clear();
            channelRight.clear();
            for (unsigned long long i = 0; i < totalSampleCount; ++i)
            {
                if (channelsCount >= 2)
                {
                    channelLeft.push_back(currentProcessedBuffer[i * channelsCount]);
                    channelRight.push_back(currentProcessedBuffer[i * channelsCount + 1]);
                }
                else
                {
                    channelLeft.push_back(currentProcessedBuffer[i]);
                }
            }
        }
        SDL_AudioSpec srcSpec = {SDL_AUDIO_F32, (int)channelsCount, (int)sampleRate};
        audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &srcSpec, nullptr, nullptr);
        if (audioStream)
        {
            SDL_PutAudioStreamData(audioStream, currentProcessedBuffer.data(), currentProcessedBuffer.size() * sizeof(float));
            if (!isPlaybackPaused)
                SDL_ResumeAudioStreamDevice(audioStream);
        }
        currentPlaybackPos = 0;
        isAudioLoaded = true;
        ReallocateTriggers();
    }
    void DrawTriggeredOscilloscope(const char *label, const std::vector<float> &channelData, CorrelationTrigger &triggerObj, ImVec2 size, ImU32 color)
    {
        std::lock_guard<std::mutex> lock(m_triggerMutex);
        if (channelData.empty())
        {
            ImGui::Dummy(size);
            return;
        }
        ImGui::Text("%s", label);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(p0, p1, IM_COL32(15, 15, 15, 255));
        drawList->AddLine(ImVec2(p0.x, p0.y + size.y / 2.0f), ImVec2(p1.x, p0.y + size.y / 2.0f), IM_COL32(45, 45, 45, 255));
        TriggerResult tr = triggerObj.get_trigger(channelData, currentPlaybackPos, 120);
        int stableStartIndex = tr.trigger;
        CorrelationTrigger::PerFrameCache cache;
        float sum = 0.0f;
        int count = 0;
        for (int i = 0; i < uiTsamp; ++i)
        {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size())
            {
                sum += channelData[idx];
                count++;
            }
        }
        cache.result_mean = (count > 0) ? (sum / count) : 0.0f;
        cache.period = 40.0f;
        std::vector mathSegment(uiTsamp, 0.0f);
        for (int i = 0; i < uiTsamp; ++i)
        {
            size_t idx = stableStartIndex + i;
            if (idx < channelData.size())
                mathSegment[i] = channelData[idx];
        }
        triggerObj._update_buffer(mathSegment, cache);
        int widthPixels = size.x;
        float samplesPerPixel = static_cast<float>(viewWindowSize) / size.x;
        float midY = p0.y + (size.y / 2.0f);
        for (int x = 0; x < widthPixels - 1; ++x)
        {
            size_t startOffset = static_cast<size_t>(x * samplesPerPixel);
            size_t endOffset = static_cast<size_t>((x + 1) * samplesPerPixel);
            float minVal = 0.0f;
            float maxVal = 0.0f;
            bool hasData = false;
            for (size_t s = startOffset; s < endOffset && s < static_cast<size_t>(viewWindowSize); ++s)
            {
                size_t idx = stableStartIndex + s;
                if (idx < channelData.size())
                {
                    float val = channelData[idx];
                    if (!hasData)
                    {
                        minVal = maxVal = val;
                        hasData = true;
                    }
                    else
                    {
                        if (val < minVal)
                            minVal = val;
                        if (val > maxVal)
                            maxVal = val;
                    }
                }
            }
            float yTop = midY - (maxVal * (size.y / 2.0f));
            float yBottom = midY - (minVal * (size.y / 2.0f));
            drawList->AddLine(ImVec2(p0.x + x, yTop), ImVec2(p0.x + x, yBottom), color, 1.5f);
        }
        ImGui::Dummy(size);
    }
    void RenderUI()
    {
        if (m_hasNewTrackToProcess.load())
        {
            SwitchToTrack(selectedTrackIndex);
            m_hasNewTrackToProcess.store(false);
        }
        ImGui::Begin("Oscilloscope Audio Synchronisé (SIMD Trigger)");
        if (isFileLoading.load())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "🔄 Décodage en cours... Veuillez patienter.");
        }
        else if (!lastLoadingError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "❌ Erreur : %s", lastLoadingError.c_str());
        }
        else
        {
            ImGui::Text("Fichier actuel : %s", currentFilePath.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Importer un nouveau fichier..."))
        {
            if (!isFileLoading.load())
                fileDialog.Open();
        }
        ImGui::Separator();
        ImGui::Text("Bibliothèque de morceaux chargés :");
        ImVec2 treeSize(ImGui::GetContentRegionAvail().x, 120.0f);
        ImGui::BeginChild("TreeViewChild", treeSize, true);
        if (ImGui::TreeNodeEx("Mes Fichiers Audio", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (size_t i = 0; i < playlist.size(); ++i)
            {
                const auto &track = playlist[i];
                std::string bitDepthInfo = (track.bitDepth > 0) ? std::to_string(track.bitDepth) + "-bit" : "VBR";
                std::string chInfo = (track.channels == 2) ? "Stéréo" : (track.channels == 1 ? "Mono" : std::to_string(track.channels) + "ch");
                std::string itemText = track.fileName + " [" + chInfo + " | " + track.durationStr + " | " + bitDepthInfo + "]";
                ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (i == selectedTrackIndex)
                    nodeFlags |= ImGuiTreeNodeFlags_Selected;
                ImGui::TreeNodeEx((void *)(intptr_t)i, nodeFlags, "%s", itemText.c_str());
                if (ImGui::IsItemClicked() && !isFileLoading.load())
                {
                    SwitchToTrack(static_cast<int>(i));
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (isAudioLoaded)
        {
            ImGui::Text("Fréquence : %u Hz | Canaux : %u | Taille : %llu échantillons", sampleRate, channelsCount, totalSampleCount);
            if (ImGui::CollapsingHeader("Configuration Avancée du Déclencheur", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool triggerParamsChanged = false;
                if (ImGui::SliderInt("Avance par Image", &samplesPerFrame, 64, 2048, "%d samples"))
                    triggerParamsChanged = true;
                if (ImGui::SliderInt("Taille Analyse (tsamp)", &uiTsamp, 128, 4096, "%d samples"))
                {
                    if (uiTsamp % 2 != 0)
                        uiTsamp++;
                    triggerParamsChanged = true;
                }
                if (ImGui::SliderInt("Sous-échantillonnage (stride)", &uiStride, 1, 8, "1/%d"))
                    triggerParamsChanged = true;
                if (triggerParamsChanged)
                    ReallocateTriggers();
            }
            ImGui::Separator();
            ImGui::Text("Effet DSP Audio (Analyse Nodale / Saturation) :");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            bool effectSwitched = false;
            bool slidersChanged = false;
            if (ImGui::Combo("##EffectCombo", &selectedEffectIndex, effectNames, 7))
                effectSwitched = true;
            float currentFs = (sampleRate > 0) ? static_cast<float>(sampleRate) : 44100.0f;
            if (selectedEffectIndex == 1)
            {
                ImGui::Spacing();
                ImGui::Text("Paramètres de la réverbération Comb :");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                int uiReverbDelayMs = static_cast<int>((static_cast<float>(uiReverbDelay) * 1000.0f) / currentFs);
                if (ImGui::SliderInt("Longueur du retard##Comb", &uiReverbDelayMs, 10, 350, "%d ms"))
                {
                    uiReverbDelay = static_cast<int>((static_cast<float>(uiReverbDelayMs) * currentFs) / 1000.0f);
                    if (uiReverbDelay < 64)
                        uiReverbDelay = 64;
                    slidersChanged = true;
                }
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                float uiReverbFeedbackDb = (uiReverbFeedback > 0.001f) ? (20.0f * std::log10(uiReverbFeedback)) : -60.0f;
                if (ImGui::SliderFloat("Intensité de l'écho##Comb", &uiReverbFeedbackDb, -60.0f, -0.17f, "%.1f dB"))
                {
                    uiReverbFeedback = (uiReverbFeedbackDb <= -59.9f) ? 0.0f : std::pow(10.0f, uiReverbFeedbackDb / 20.0f);
                    uiReverbFeedback = std::clamp(uiReverbFeedback, 0.0f, 0.98f);
                    slidersChanged = true;
                }
            }
            else if (selectedEffectIndex == 2 || selectedEffectIndex == 3)
            {
                ImGui::Spacing();
                ImGui::Text(selectedEffectIndex == 2 ? "Paramètres de la réverbération FDN 2x2 :" : "Paramètres de la réverbération FDN 4x4 Pro :");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                int uiFdnDelayMs = static_cast<int>((static_cast<float>(uiFdnDelayBase) * 1000.0f) / currentFs);
                if (ImGui::SliderInt("Taille de la pièce##FDN", &uiFdnDelayMs, 10, 350, "%d ms"))
                {
                    uiFdnDelayBase = static_cast<int>((static_cast<float>(uiFdnDelayMs) * currentFs) / 1000.0f);
                    if (uiFdnDelayBase < 64)
                        uiFdnDelayBase = 64;
                    slidersChanged = true;
                }
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                float uiFdnFeedbackDb = (uiFdnFeedback > 0.001f) ? (20.0f * std::log10(uiFdnFeedback)) : -60.0f;
                if (ImGui::SliderFloat("Densité de diffusion##FDN", &uiFdnFeedbackDb, -60.0f, -0.17f, "%.1f dB"))
                {
                    uiFdnFeedback = (uiFdnFeedbackDb <= -59.9f) ? 0.0f : std::pow(10.0f, uiFdnFeedbackDb / 20.0f);
                    uiFdnFeedback = std::clamp(uiFdnFeedback, 0.0f, 0.98f);
                    slidersChanged = true;
                }
            }
            else if (selectedEffectIndex >= 4 && selectedEffectIndex <= 6)
            {
                ImGui::Spacing();
                ImGui::Text("Paramètres de la Saturation Nodale :");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                if (ImGui::SliderFloat("Gain / Drive##OD", &uiDriveGain, 1.0f, 15.0f, "x%.1f"))
                    slidersChanged = true;
                if (selectedEffectIndex == 4)
                {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                    if (ImGui::SliderFloat("Polarisation Asymétrique##OD", &uiAsymmetry, -0.4f, 0.4f, "%.2f V"))
                        slidersChanged = true;
                }
                else if (selectedEffectIndex == 6)
                {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                    if (ImGui::SliderFloat("Tension Grille (Bias)##Tube", &uiTriodeBias, -2.5f, -0.2f, "%.2f V"))
                        slidersChanged = true;
                }
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                if (ImGui::SliderFloat("Mélange (Dry / Wet)##OD", &uiWetMix, 0.0f, 1.0f, "%.2f"))
                    slidersChanged = true;
            }
            if ((effectSwitched || slidersChanged) && !originalInterleavedBuffer.empty())
            {
                std::lock_guard<std::mutex> lock(m_triggerMutex);
                reverbLeft->ResetNodalCircuit();
                reverbRight->ResetNodalCircuit();
                fdn2x2Processor->Reset();
                fdn4x4Processor->Reset();
                if (selectedEffectIndex == 1)
                {
                    reverbLeft->UpdateNodalParameters(uiReverbDelay, uiReverbFeedback);
                    reverbRight->UpdateNodalParameters(static_cast<float>(uiReverbDelay * 1.1f), uiReverbFeedback);
                }
                else if (selectedEffectIndex == 2)
                {
                    fdn2x2Processor->UpdateParameters(uiFdnDelayBase, static_cast<float>(uiFdnDelayBase * 1.618f), uiFdnFeedback);
                }
                else if (selectedEffectIndex == 3)
                {
                    fdn4x4Processor->UpdateParameters(uiFdnDelayBase, static_cast<float>(uiFdnDelayBase * 1.31f), static_cast<float>(uiFdnDelayBase * 1.63f), static_cast<float>(uiFdnDelayBase * 2.11f), uiFdnFeedback);
                }
                else if (selectedEffectIndex == 4)
                {
                    overdriveStatic->UpdateParameters(uiDriveGain, uiAsymmetry, uiWetMix);
                }
                else if (selectedEffectIndex == 5)
                {
                    overdriveShockley->UpdateParameters(uiDriveGain, uiWetMix);
                }
                else if (selectedEffectIndex == 6)
                {
                    overdriveTriode->UpdateParameters(uiDriveGain, uiTriodeBias, uiWetMix);
                }
                currentProcessedBuffer = originalInterleavedBuffer;
                ExecuteDSPPipeline(totalSampleCount, channelsCount);
                channelLeft.clear();
                channelRight.clear();
                for (unsigned long long i = 0; i < totalSampleCount; ++i)
                {
                    if (channelsCount >= 2)
                    {
                        channelLeft.push_back(currentProcessedBuffer[i * channelsCount]);
                        channelRight.push_back(currentProcessedBuffer[i * channelsCount + 1]);
                    }
                    else
                    {
                        channelLeft.push_back(currentProcessedBuffer[i]);
                    }
                }
                if (audioStream)
                {
                    SDL_ClearAudioStream(audioStream);
                    size_t sampleOffset = static_cast<size_t>(currentPlaybackPos) * channelsCount;
                    if (sampleOffset < currentProcessedBuffer.size())
                    {
                        SDL_PutAudioStreamData(audioStream, &currentProcessedBuffer[sampleOffset], (currentProcessedBuffer.size() - sampleOffset) * sizeof(float));
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::Button(isPlaybackPaused ? "Reprendre" : "Pause"))
            {
                isPlaybackPaused = !isPlaybackPaused;
                if (audioStream)
                {
                    if (isPlaybackPaused)
                        SDL_PauseAudioStreamDevice(audioStream);
                    else
                        SDL_ResumeAudioStreamDevice(audioStream);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Position"))
            {
                currentPlaybackPos = 0;
                if (audioStream)
                {
                    std::lock_guard<std::mutex> lock(m_triggerMutex);
                    SDL_ClearAudioStream(audioStream);
                    if (!currentProcessedBuffer.empty())
                    {
                        SDL_PutAudioStreamData(audioStream, currentProcessedBuffer.data(), currentProcessedBuffer.size() * sizeof(float));
                    }
                }
            }
            if (!isPlaybackPaused)
            {
                currentPlaybackPos += samplesPerFrame;
                if (static_cast<size_t>(currentPlaybackPos) >= channelLeft.size())
                {
                    currentPlaybackPos = 0;
                    std::lock_guard<std::mutex> lock(m_triggerMutex);
                    if (audioStream && !currentProcessedBuffer.empty())
                    {
                        SDL_ClearAudioStream(audioStream);
                        SDL_PutAudioStreamData(audioStream, currentProcessedBuffer.data(), currentProcessedBuffer.size() * sizeof(float));
                    }
                }
            }
            ImGui::SliderInt("Fenêtre d'affichage (Zoom)", &viewWindowSize, 256, 4096, "%d samples");
            float progress = static_cast<float>(static_cast<float>(currentPlaybackPos) / static_cast<float>(channelLeft.size()));
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
            ImVec2 widgetSize(ImGui::GetContentRegionAvail().x, 160.0f);
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal GAUCHE (L) - Aligné SIMD", channelLeft, *triggerLeft, widgetSize, IM_COL32(0, 255, 200, 255));
            ImGui::Spacing();
            DrawTriggeredOscilloscope("Canal DROIT (R) - Aligné SIMD", channelRight, *triggerRight, widgetSize, IM_COL32(255, 180, 0, 255));
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Veuillez sélectionner un morceau dans la bibliothèque pour démarrer l'analyse de phase.");
        }
        ImGui::End();
        fileDialog.Display();
        if (fileDialog.HasSelected())
        {
            if (!isFileLoading.load())
                StartAsyncLoad(fileDialog.GetSelected().string());
            fileDialog.ClearSelected();
        }
    }
};