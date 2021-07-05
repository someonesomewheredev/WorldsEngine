#pragma once
#include <glm/glm.hpp>
#include <SDL.h>
#include "AssetDB.hpp"
#include "IGameEventHandler.hpp"
#include "JobSystem.hpp"
#include <bitset>
#include "ISystem.hpp"
#include "../Render/Camera.hpp"
#include "Console.hpp"

namespace worlds {
    const int NUM_SUBMESH_MATS = 32;
    extern glm::ivec2 windowSize;
    extern JobSystem* g_jobSys;
    class VKRenderer;
    class PolyRenderPass;
    class AudioSystem;
    class InputManager;
    class Editor;
    class DotNetScriptEngine;
    class OpenVRInterface;
    class RTTPass;

    struct SceneInfo {
        std::string name;
        AssetID id;
    };

    struct EngineInitOptions {
        EngineInitOptions()
            : useEventThread(false)
            , workerThreadOverride(-1)
            , runAsEditor(false)
            , enableVR(false)
            , dedicatedServer(false)
            , eventHandler(nullptr)
            , gameName("Untitled") {}
        bool useEventThread;
        int workerThreadOverride;
        bool runAsEditor;
        bool enableVR;
        bool dedicatedServer;
        IGameEventHandler* eventHandler;
        const char* gameName;
    };

    struct SceneSettings {
        AssetID skybox;
    };

    struct PrefabInstanceComponent {
        AssetID prefab;
    };

    class WorldsEngine {
    public:
        WorldsEngine(EngineInitOptions initOptions, char* argv0);
        ~WorldsEngine();

        void mainLoop();
        void loadScene(AssetID scene);
        void createStartupScene();
        void addSystem(ISystem* system);
        SDL_Window* getMainWindow() const { return window; }
        const SceneInfo& getCurrentSceneInfo() const { return currentScene; }
        void quit() { running = false; }
        bool pauseSim;
        bool runAsEditor;
        void destroyNextFrame(entt::entity ent) { this->nextFrameKillList.push_back(ent); }
        [[nodiscard]] double getGameTime() const { return gameTime; }
    private:
        struct DebugTimeInfo {
            double deltaTime;
            double updateTime;
            double simTime;
            double lastUpdateTime;
            int frameCounter;
        };

        void processEvents();
        static int windowThread(void* data);
        void setupSDL();
        static SDL_Window* createSDLWindow();
        void setupPhysfs(char* argv0);
        void drawDebugInfoWindow(DebugTimeInfo timeInfo);
        void updateSimulation(float& interpAlpha, double deltaTime);
        void doSimStep(float deltaTime);

        SDL_Window* window;

        bool running;
        bool dedicatedServer;
        entt::registry registry;

        IGameEventHandler* evtHandler;
        RTTPass* screenRTTPass;
        Camera cam;

        SceneInfo currentScene;
        bool sceneLoadQueued = false;
        AssetID queuedSceneID;

        double timeScale = 1.0;
        double gameTime = 0.0;
        double simAccumulator;

        std::unique_ptr<VKRenderer> renderer;
        std::unique_ptr<InputManager> inputManager;
        std::unique_ptr<AudioSystem> audioSystem;
        std::unique_ptr<Console> console;
        std::unique_ptr<Editor> editor;
        std::unique_ptr<DotNetScriptEngine> scriptEngine;
        std::unique_ptr<OpenVRInterface> openvrInterface;

        std::vector<ISystem*> systems;
        std::vector<entt::entity> nextFrameKillList;
    };

    enum class StaticFlags : uint8_t {
        None = 0,
        Audio = 1,
        Rendering = 2,
        Navigation = 4
    };

    enum class UVOverride {
        None,
        XY,
        XZ,
        ZY,
        PickBest
    };

    struct WorldObject {
        WorldObject(AssetID material, AssetID mesh)
            : staticFlags(StaticFlags::None)
            , mesh(mesh)
            , texScaleOffset(1.0f, 1.0f, 0.0f, 0.0f)
            , uvOverride(UVOverride::None) {
            for (int i = 0; i < NUM_SUBMESH_MATS; i++) {
                materials[i] = material;
                presentMaterials[i] = false;
                materialIdx[i] = ~0u;
            }
            presentMaterials[0] = true;
        }

        StaticFlags staticFlags;
        AssetID materials[NUM_SUBMESH_MATS];
        std::bitset<NUM_SUBMESH_MATS> presentMaterials;
        AssetID mesh;
        glm::vec4 texScaleOffset;
        UVOverride uvOverride;
        uint32_t materialIdx[NUM_SUBMESH_MATS];
    };

    struct UseWireframe {};

    enum class LightType {
        Point,
        Spot,
        Directional,
        Sphere,
        Tube
    };

    struct WorldLight {
        WorldLight() {}
        WorldLight(LightType type) : type(type) {}
        bool enabled = true;
        LightType type = LightType::Point;
        glm::vec3 color = glm::vec3{1.0f};
        float intensity = 1.0f;
        float spotCutoff = 0.7f;
        float tubeLength = 0.25f;
        float tubeRadius = 0.1f;
        bool enableShadows = false;
        uint32_t shadowmapIdx = ~0u;
        float distanceCutoff = 1.0f;
        float shadowNear = 0.05f;
        float shadowFar = 100.0f;
    };
}
