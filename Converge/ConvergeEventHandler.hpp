#pragma once
#include <enet/enet.h>
#include <IGameEventHandler.hpp>
#include <Console.hpp>
#include "PidController.hpp"
#include <Camera.hpp>
#include "LocospherePlayerSystem.hpp"
#include "Client.hpp"

namespace converge {
    class EventHandler : public worlds::IGameEventHandler {
    public:
        EventHandler(bool dedicatedServer);
        void init(entt::registry& registry, worlds::EngineInterfaces interfaces) override;
        void preSimUpdate(entt::registry& registry, float deltaTime) override;
        void update(entt::registry& registry, float deltaTime, float interpAlpha) override;
        void simulate(entt::registry& registry, float simStep) override;
        void onSceneStart(entt::registry& reg) override;
        void shutdown(entt::registry& registry) override;
    private:
        worlds::IVRInterface* vrInterface;
        worlds::VKRenderer* renderer;
        worlds::InputManager* inputManager;
        worlds::Camera* camera;
        LocospherePlayerSystem* lsphereSys;
        bool isDedicated;
        ENetHost* enetHost;
        entt::entity otherLocosphere;
        Client* client;
    };
}
