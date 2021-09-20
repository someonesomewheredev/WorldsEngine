#include <SDL_audio.h>
#include "../Core/Log.hpp"
#include "Audio.hpp"
#include <glm/glm.hpp>
#include <glm/ext/scalar_constants.hpp>
#include "../Util/TimingUtil.hpp"
#include "../IO/IOUtil.hpp"
#include "../ImGui/imgui.h"
#include "../Core/Transform.hpp"
#include "phonon.h"
#include "../Core/Fatal.hpp"
#include <slib/DynamicLibrary.hpp>
#include "../Physics/Physics.hpp"
#include <fmod_errors.h>
#include <stdlib.h>

#define FMCHECK(_result) checkFmodErr(_result, __FILE__, __LINE__)
#define SACHECK(_result) checkSteamAudioErr(_result, __FILE__, __LINE__)

namespace worlds {
    void checkFmodErr(FMOD_RESULT result, const char* file, int line) {
        if (result != FMOD_OK) {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "FMOD error: %s", FMOD_ErrorString(result));
            fatalErrInternal(buffer, file, line);
        }
    }

    void checkSteamAudioErr(IPLerror result, const char* file, int line) {
        if (result != IPLerror::IPL_STATUS_SUCCESS) {
            const char* iplErrs[] = {
                "The operation completed successfully.",
                "An unspecified error occurred.",
                "The system ran out of memory."
                "An error occurred while initializing an external dependency."
            };

            char buffer[256];
            snprintf(buffer, sizeof(buffer), "Steam Audio error: %s", iplErrs[(int)result]);
            fatalErrInternal(buffer, file, line);
        }
    }

    FMOD_RESULT convertPhysFSError(PHYSFS_ErrorCode errCode) {
        switch (errCode) {
        case PHYSFS_ERR_NOT_FOUND:
            return FMOD_ERR_FILE_NOTFOUND;
        case PHYSFS_ERR_OUT_OF_MEMORY:
            return FMOD_ERR_MEMORY;
        case PHYSFS_ERR_OK:
            return FMOD_OK;
        default:
            return FMOD_ERR_FILE_BAD;
        }
    }

    FMOD_RESULT F_CALL fileOpenCallback(const char* name, unsigned int* filesize, void** handle, void* userdata) {
        PHYSFS_File* file = PHYSFS_openRead(name);

        if (file == nullptr) {
            PHYSFS_ErrorCode err = PHYSFS_getLastErrorCode();

            return convertPhysFSError(err);
        }

        *handle = file;
        *filesize = (uint32_t)PHYSFS_fileLength(file);

        return FMOD_OK;
    }

    FMOD_RESULT F_CALL fileCloseCallback(void* handle, void* userdata) {
        if (PHYSFS_close((PHYSFS_File*)handle) == 0)
            return convertPhysFSError(PHYSFS_getLastErrorCode());
        else
            return FMOD_OK;
    }

    FMOD_RESULT F_CALL fileReadCallback(void* handle, void* buffer, uint32_t sizeBytes, uint32_t* bytesRead, void* userdata) {
        int64_t result = PHYSFS_readBytes((PHYSFS_File*)handle, buffer, sizeBytes);
        if (result == -1) {
            PHYSFS_ErrorCode err = PHYSFS_getLastErrorCode();
            *bytesRead = 0;

            return convertPhysFSError(err);
        } else {
            *bytesRead = result;
            return FMOD_OK;
        }
    }

    FMOD_RESULT F_CALL fileSeekCallback(void* handle, uint32_t pos, void* userdata) {
        if (PHYSFS_seek((PHYSFS_File*)handle, pos) == 0)
            return convertPhysFSError(PHYSFS_getLastErrorCode());
        else
            return FMOD_OK;
    }

    AudioSystem* AudioSystem::instance;

    typedef void(*PFN_iplFMODInitialize)(IPLContext context);
    typedef void(*PFN_iplFMODSetHRTF)(IPLHRTF hrtf);
    typedef void(*PFN_iplFMODSetSimulationSettings)(IPLSimulationSettings simulationSettings);

    FMOD_RESULT F_CALL fmodDebugCallback(FMOD_DEBUG_FLAGS flags, const char* file, int line, const char* func, const char* message) {
        if (flags & FMOD_DEBUG_LEVEL_ERROR) {
            logErr(WELogCategoryAudio, "FMOD: %s (%s:%s, %i)", message, file, func, line);
        }

        if (flags & FMOD_DEBUG_LEVEL_WARNING) {
            logWarn(WELogCategoryAudio, "FMOD: %s (%s:%s, %i)", message, file, func, line);
        }

        if (flags & FMOD_DEBUG_LEVEL_LOG) {
            logVrb(WELogCategoryAudio, "FMOD: %s (%s:%s, %i)", message, file, func, line);
        }

        return FMOD_OK;
    }

    void steamAudioDebugCallback(IPLLogLevel logLevel, const char* message) {
        logMsg(WELogCategoryAudio, "%s", message);
    }

    void AudioSource::changeEventPath(const std::string_view& eventPath) {
        FMOD::Studio::EventDescription* desc;

        AudioSystem* _this = AudioSystem::getInstance();

        FMOD_RESULT result;
        result = _this->studioSystem->getEvent(eventPath.data(), &desc);
        if (result != FMOD_OK) {
            logErr("Failed to get event %s: %s", eventPath.data(), FMOD_ErrorString(result));
            return;
        }

        if (eventInstance != nullptr) {
            eventInstance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
            eventInstance->release();
        }

        result = desc->createInstance(&eventInstance);
        if (result != FMOD_OK) {
            logErr("Failed to create event %s: %s", eventPath.data(), FMOD_ErrorString(result));
            return;
        }

        _eventPath.assign(eventPath);
    }

    FMOD_STUDIO_PLAYBACK_STATE AudioSource::playbackState() {
        FMOD_STUDIO_PLAYBACK_STATE ret;
        FMCHECK(eventInstance->getPlaybackState(&ret));
        return ret;
    }

    AudioSystem::AudioSystem() {
        const char* phononPluginName;

#ifdef _WIN32
        phononPluginName = "phonon_fmod.dll";
#elif defined(__linux__)
        phononPluginName = "libphonon_fmod.so";
#endif
        //FMCHECK(FMOD::Debug_Initialize(FMOD_DEBUG_LEVEL_WARNING, FMOD_DEBUG_MODE_CALLBACK, fmodDebugCallback));
        void* fmodHeap = malloc(20000 * 2 * 512); // 20MB
        FMOD::Memory_Initialize(fmodHeap, 20000 * 2 * 512, nullptr, nullptr, nullptr);

        FMCHECK(FMOD::Studio::System::create(&studioSystem));
        FMCHECK(studioSystem->getCoreSystem(&system));
        FMCHECK(system->setSoftwareFormat(0, FMOD_SPEAKERMODE_STEREO, 0));

        FMCHECK(studioSystem->initialize(1024, FMOD_STUDIO_INIT_NORMAL, FMOD_INIT_NORMAL, nullptr));
        FMCHECK(studioSystem->setNumListeners(1));

        FMCHECK(system->setFileSystem(fileOpenCallback, fileCloseCallback, fileReadCallback, fileSeekCallback, nullptr, nullptr, -1));

        FMCHECK(system->loadPlugin(phononPluginName, &phononPluginHandle));

        // Get Steam Audio's setting equivalents from FMOD
        IPLAudioSettings audioSettings{};

        {
            FMOD_SPEAKERMODE speakerMode;
            int numRawSpeakers;
            system->getSoftwareFormat(&audioSettings.samplingRate, &speakerMode, &numRawSpeakers);

            int numBuffers;
            uint32_t bufferLen;
            system->getDSPBufferSize(&bufferLen, &numBuffers);
            audioSettings.frameSize = bufferLen;
        }

        // Load function pointers for the Steam Audio FMOD plugin
        slib::DynamicLibrary fmodPlugin(phononPluginName);

        PFN_iplFMODInitialize iplFMODInitialize;
        PFN_iplFMODSetHRTF iplFMODSetHRTF;
        PFN_iplFMODSetSimulationSettings iplFMODSetSimulationSettings;

        iplFMODInitialize = (PFN_iplFMODInitialize)fmodPlugin.getFunctionPointer("iplFMODInitialize");
        iplFMODSetHRTF = (PFN_iplFMODSetHRTF)fmodPlugin.getFunctionPointer("iplFMODSetHRTF");
        iplFMODSetSimulationSettings = (PFN_iplFMODSetSimulationSettings)fmodPlugin.getFunctionPointer("iplFMODSetSimulationSettings");

        // Create the Steam Audio context
        IPLContextSettings contextSettings{};
        contextSettings.version = STEAMAUDIO_VERSION;
        contextSettings.simdLevel = IPL_SIMDLEVEL_SSE4;
        contextSettings.logCallback = steamAudioDebugCallback;

        SACHECK(iplContextCreate(&contextSettings, &phononContext));

        // Create HRTF
        IPLHRTFSettings hrtfSettings{};
        hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;

        iplFMODInitialize(phononContext);
        SACHECK(iplHRTFCreate(phononContext, &audioSettings, &hrtfSettings, &phononHrtf));
        iplFMODSetHRTF(phononHrtf);

        IPLSimulationSettings simulationSettings{};
        simulationSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
        simulationSettings.sceneType = IPL_SCENETYPE_DEFAULT;
        simulationSettings.maxNumOcclusionSamples = 1024;
        simulationSettings.maxNumRays = 64;
        simulationSettings.numDiffuseSamples = 1024;
        simulationSettings.maxDuration = 0.5f;
        simulationSettings.maxOrder = 8;
        simulationSettings.maxNumSources = 512;
        simulationSettings.numThreads = 5;
        simulationSettings.rayBatchSize = 16;
        simulationSettings.numVisSamples = 512;
        simulationSettings.samplingRate = audioSettings.samplingRate;
        simulationSettings.frameSize = audioSettings.frameSize;

        iplFMODSetSimulationSettings(simulationSettings);

        instance = this;
    }

    void AudioSystem::initialise(entt::registry& worldState) {
        worldState.on_destroy<AudioSource>().connect<&AudioSystem::onAudioSourceDestroy>(*this);
    }

    void AudioSystem::onAudioSourceDestroy(entt::registry& reg, entt::entity entity) {
        AudioSource& as = reg.get<AudioSource>(entity);

        if (as.eventInstance) {
            FMCHECK(as.eventInstance->stop(FMOD_STUDIO_STOP_IMMEDIATE));
            FMCHECK(as.eventInstance->release());
        }
    }

    void AudioSystem::loadMasterBanks() {
        masterBank = loadBank("FMOD/Master.bank");
        stringsBank = loadBank("FMOD/Master.strings.bank");
    }

    FMOD_VECTOR convVec(glm::vec3 v3) {
        FMOD_VECTOR v{};
        v.x = -v3.x;
        v.y = v3.y;
        v.z = v3.z;
        return v;
    }

    void AudioSystem::update(entt::registry& worldState, glm::vec3 listenerPos, glm::quat listenerRot, float deltaTime) {
        glm::vec3 movement = listenerPos - lastListenerPos;
        movement /= deltaTime;

        FMOD_3D_ATTRIBUTES listenerAttributes{};
        listenerAttributes.forward = convVec(listenerRot * glm::vec3(0.0f, 0.0f, 1.0f));
        listenerAttributes.up = convVec(listenerRot * glm::vec3(0.0f, 1.0f, 0.0f));

        listenerAttributes.position = convVec(listenerPos);
        listenerAttributes.velocity = convVec(movement);

        worldState.view<AudioSource, Transform>().each([](AudioSource& as, Transform& t) {
            if (as.eventInstance == nullptr) return;

            FMOD_3D_ATTRIBUTES sourceAttributes{};
            sourceAttributes.position = convVec(t.position);
            sourceAttributes.forward = convVec(t.rotation * glm::vec3(0.0f, 0.0f, 1.0f));
            sourceAttributes.up = convVec(t.rotation * glm::vec3(0.0f, 1.0f, 0.0f));

            FMCHECK(as.eventInstance->set3DAttributes(&sourceAttributes));
        });

        FMCHECK(studioSystem->setListenerAttributes(0, &listenerAttributes, &listenerAttributes.position));
        FMCHECK(studioSystem->update());
    }

    void AudioSystem::stopEverything(entt::registry& reg) {
        for (auto& pair : eventDescs) {
            FMCHECK(pair.second->releaseAllInstances());
        }

        reg.view<AudioSource>().each([](AudioSource& as) {
            as.eventInstance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
        });
    }

    void AudioSystem::playOneShotClip(AssetID id, glm::vec3 location, bool spatialise, float volume, MixerChannel) {
        FMOD::Sound* sound;

        if (sounds.contains(id)) {
            sound = sounds.at(id);
        } else {
            FMCHECK(system->createSound(AssetDB::idToPath(id).c_str(), FMOD_CREATESAMPLE, nullptr, &sound));

            sounds.insert({ id, sound });
        }

        FMOD::Channel* channel;
        FMCHECK(system->playSound(sound, nullptr, false, &channel));

        if (spatialise) {
            FMOD_VECTOR fmLocation = convVec(location);
            FMOD_VECTOR vel{0.0f, 0.0f, 0.0f};
            channel->set3DAttributes(&fmLocation, &vel);
        }
    }

    void AudioSystem::playOneShotEvent(const char* eventPath, glm::vec3 location, float volume) {
        FMOD_RESULT result;

        FMOD::Studio::EventDescription* desc;

        if (!eventDescs.contains(eventPath)) {
            result = studioSystem->getEvent(eventPath, &desc);

            int instCount;
            FMCHECK(desc->getInstanceCount(&instCount));
            //logMsg("desc had %i instances", instCount);

            if (result != FMOD_OK) {
                logErr("Failed to get event %s: %s", eventPath, FMOD_ErrorString(result));
                return;
            }

            eventDescs.insert({ eventPath, desc });
        } else {
            desc = eventDescs.at(eventPath);
        }

        FMOD::Studio::EventInstance* instance;

        result = desc->createInstance(&instance);
        if (result != FMOD_OK) {
            logErr("Failed to create instance of event %s: %s", eventPath, FMOD_ErrorString(result));
            return;
        }

        FMOD_3D_ATTRIBUTES attr{};
        attr.position = convVec(location);
        attr.forward = convVec(glm::vec3(0.0f, 0.0f, 1.0f));
        attr.up = convVec(glm::vec3(0.0f, 1.0f, 0.0f));
        attr.velocity = convVec(glm::vec3(0.0f));

        FMCHECK(instance->set3DAttributes(&attr));
        FMCHECK(instance->setVolume(volume));

        FMCHECK(instance->start());
        FMCHECK(instance->release());
    }

    void AudioSystem::shutdown(entt::registry& worldState) {
        FMCHECK(studioSystem->release());
    }

    FMOD::Studio::Bank* AudioSystem::loadBank(const char* path) {
        PHYSFS_getLastErrorCode();
        FMOD::Studio::Bank* bank;

        if (loadedBanks.contains(path))
            return loadedBanks.at(path);

        FMCHECK(studioSystem->loadBankFile(path, FMOD_STUDIO_LOAD_BANK_NORMAL, &bank));
        loadedBanks.insert({ path, bank });

        return bank;
    }
}
