#include "EventHandler.hpp"
#include <Util/RichPresence.hpp>
#include <openvr.h>
#include <Core/Log.hpp>
#include <Render/Render.hpp>
#include "Core/AssetDB.hpp"
#include "DebugArrow.hpp"
#include "NetMessage.hpp"
#include "Physics/D6Joint.hpp"
#include "Physics/PhysicsActor.hpp"
#include <physx/PxRigidDynamic.h>
#include "Render/Loaders/SourceModelLoader.hpp"
#include "Core/Transform.hpp"
#include <VR/OpenVRInterface.hpp>
#include <Physics/Physics.hpp>
#include <Core/Console.hpp>
#include <ImGui/imgui.h>
#include <Util/MatUtil.hpp>
#include <Core/Engine.hpp>
#include "Core/NameComponent.hpp"
#include "LocospherePlayerSystem.hpp"
#include "PhysHandSystem.hpp"
#include <enet/enet.h>
#include <Core/JobSystem.hpp>
#include "Util/CreateModelObject.hpp"
#include "ObjectParentSystem.hpp"
#ifdef DISCORD_RPC
#include <core.h>
#endif
#include "Util/VKImGUIUtil.hpp"
#include <Scripting/ScriptComponent.hpp>
#include <Physics/FilterEntities.hpp>
#include <physxit.h>
#include "MathsUtil.hpp"
#include "PlayerStartPoint.hpp"
#include "RPGStats.hpp"
#include <Scripting/WrenVM.hpp>
#include "GripPoint.hpp"
#include <Input/Input.hpp>
#include <Physics/FixedJoint.hpp>

namespace lg {

    struct SyncedRB {};

    void cmdToggleVsync(void* obj, const char*) {
        auto renderer = (worlds::VKRenderer*)obj;
        renderer->setVsync(!renderer->getVsync());
    }

    EventHandler::EventHandler(bool dedicatedServer)
        : isDedicated {dedicatedServer}
        , client {nullptr}
        , server {nullptr}
        , lHandEnt {entt::null}
        , rHandEnt {entt::null} {
    }

    void EventHandler::init(entt::registry& registry, worlds::EngineInterfaces interfaces) {
        vrInterface = interfaces.vrInterface;
        renderer = interfaces.renderer;
        camera = interfaces.mainCamera;
        inputManager = interfaces.inputManager;
        engine = interfaces.engine;
        scriptEngine = interfaces.scriptEngine;
        reg = &registry;

        worlds::g_console->registerCommand(cmdToggleVsync, "r_toggleVsync", "Toggles Vsync.", renderer);
        interfaces.engine->addSystem(new ObjectParentSystem);

        lsphereSys = new LocospherePlayerSystem { interfaces, registry };
        interfaces.engine->addSystem(lsphereSys);
        interfaces.engine->addSystem(new PhysHandSystem{ interfaces, registry });

        if (enet_initialize() != 0) {
            logErr("Failed to initialize enet.");
        }

        mpManager = new MultiplayerManager{registry, isDedicated};

        new DebugArrows(registry);

        if (vrInterface) {
            worlds::g_console->registerCommand([&](void*, const char*) {
                auto& wActor = registry.get<worlds::DynamicPhysicsActor>(lHandEnt);
                auto* body = static_cast<physx::PxRigidBody*>(wActor.actor);
                body->setLinearVelocity(physx::PxVec3{ 0.0f });

                auto& lTf = registry.get<Transform>(lHandEnt);
                auto& lPh = registry.get<PhysHand>(lHandEnt);
                auto lPose = body->getGlobalPose();
                lPose.p = worlds::glm2px(lPh.targetWorldPos);
                lTf.position = lPh.targetWorldPos;
                body->setGlobalPose(lPose);

                auto& wActorR = registry.get<worlds::DynamicPhysicsActor>(rHandEnt);
                auto* rBody = static_cast<physx::PxRigidBody*>(wActorR.actor);
                rBody->setLinearVelocity(physx::PxVec3{ 0.0f });

                auto& rTf = registry.get<Transform>(rHandEnt);
                auto& rPh = registry.get<PhysHand>(rHandEnt);
                auto rPose = rBody->getGlobalPose();
                rPose.p = worlds::glm2px(rPh.targetWorldPos);
                rTf.position = rPh.targetWorldPos;
                rBody->setGlobalPose(rPose);

                lPh.posController.reset();
                lPh.rotController.reset();

                rPh.posController.reset();
                rPh.rotController.reset();
            }, "cnvrg_resetHands", "Resets hand PID controllers.", nullptr);
        }
    }

    void EventHandler::preSimUpdate(entt::registry&, float) {
        g_dbgArrows->newFrame();
    }

    entt::entity fakeLHand;
    entt::entity fakeRHand;

    void EventHandler::update(entt::registry& reg, float deltaTime, float) {
        if (vrInterface) {
            static float yRot = 0.0f;
            static float targetYRot = 0.0f;
            static bool rotated = false;
            auto rStickInput = vrInterface->getActionV2(rStick);
            auto rotateInput = rStickInput.x;
            ImGui::Text("s: %.3f, %.3f", rStickInput.x, rStickInput.y);

            float threshold = 0.5f;
            bool rotatingNow = glm::abs(rotateInput) > threshold;

            if (rotatingNow && !rotated) {
                targetYRot += glm::radians(45.0f) * -glm::sign(rotateInput);
            }

            const float rotateSpeed = 15.0f;

            yRot += glm::clamp((targetYRot - yRot), -deltaTime * rotateSpeed, deltaTime * rotateSpeed);

            camera->rotation = glm::quat{glm::vec3{0.0f, yRot, 0.0f}};

            rotated = rotatingNow;
        }

        if (reg.view<RPGStats>().size() > 0) {
            auto& rpgStat = reg.get<RPGStats>(reg.view<RPGStats>()[0]);
            if (ImGui::Begin("RPG Stats")) {
                ImGui::DragScalar("maxHP", ImGuiDataType_U64, &rpgStat.maxHP, 1.0f);
                ImGui::DragScalar("currentHP", ImGuiDataType_U64, &rpgStat.currentHP, 1.0f);
                ImGui::DragScalar("level", ImGuiDataType_U64, &rpgStat.level, 1.0f);
                ImGui::DragScalar("totalExperience", ImGuiDataType_U64, &rpgStat.totalExperience, 1.0f);
                ImGui::DragScalar("strength", ImGuiDataType_U8, &rpgStat.strength, 1.0f);
            }
            ImGui::End();

            if (reg.valid(lHandEnt) && reg.valid(rHandEnt)) {
                auto& phl = reg.get<PhysHand>(lHandEnt);
                auto& phr = reg.get<PhysHand>(rHandEnt);

                float forceLimit = 150.0f + (100.0f * rpgStat.strength);
                float torqueLimit = 2.0f + (5.0f * rpgStat.strength);

                phl.forceLimit = forceLimit;
                phr.forceLimit = forceLimit;

                phl.torqueLimit = torqueLimit;
                phr.torqueLimit = torqueLimit;

                if (reg.valid(fakeLHand) && reg.valid(fakeRHand)) {
                    auto& tfl = reg.get<Transform>(fakeLHand);
                    auto& trl = reg.get<Transform>(fakeRHand);

                    tfl.position = phl.targetWorldPos;
                    tfl.rotation = phl.targetWorldRot;

                    trl.position = phr.targetWorldPos;
                    trl.rotation = phr.targetWorldRot;
                }
            }
        }
    }

    int syncTimer = 0;
    worlds::ConVar itCompDbg { "lg_itCompDbg", "0", "Shows physics shapes for grabbed objects." };

    void addShapeTensor(entt::registry& reg, worlds::PhysicsShape& shape, physx::IT::InertiaTensorComputer& itComp,
            physx::PxTransform shapeTransform, physx::PxTransform handTransform,
            glm::vec3 scale,
            physx::PxTransform shapeWSTransform = physx::PxTransform{physx::PxIdentity}, bool showWS = false) {
        physx::IT::InertiaTensorComputer shapeComp(false);

        shapeTransform.p.multiply(worlds::glm2px(scale));

        physx::PxTransform wsTransform = handTransform * shapeTransform;

        if (itCompDbg.getInt()) {
            if (showWS) {
                worlds::createModelObject(reg, worlds::px2glm(shapeWSTransform.p), worlds::px2glm(shapeWSTransform.q),
                        worlds::g_assetDB.addOrGetExisting(shape.type == worlds::PhysicsShapeType::Sphere ?
                            "uvsphere.obj" : "model.obj"),
                        worlds::g_assetDB.addOrGetExisting("Materials/dev.json"),
                        shape.type == worlds::PhysicsShapeType::Sphere ?
                        glm::vec3{shape.sphere.radius * 0.5f} :
                        shape.box.halfExtents * scale);
            } else {
                worlds::createModelObject(reg, worlds::px2glm(wsTransform.p), worlds::px2glm(wsTransform.q),
                        worlds::g_assetDB.addOrGetExisting(shape.type == worlds::PhysicsShapeType::Sphere ?
                            "uvsphere.obj" : "model.obj"),
                        worlds::g_assetDB.addOrGetExisting("Materials/dev.json"),
                        shape.type == worlds::PhysicsShapeType::Sphere ?
                        glm::vec3{shape.sphere.radius * 0.5f} :
                        shape.box.halfExtents * scale);
            }
        }

        switch (shape.type) {
        case worlds::PhysicsShapeType::Sphere:
            shapeComp.setSphere(shape.sphere.radius * glm::compAdd(scale) / 3.0f, &shapeTransform);
            break;
        case worlds::PhysicsShapeType::Box:
            shapeComp.setBox(worlds::glm2px(shape.box.halfExtents * scale), &shapeTransform);
            break;
        case worlds::PhysicsShapeType::Capsule:
            shapeComp.setCapsule(0, shape.capsule.radius, shape.capsule.height, &shapeTransform);
            break;
        default:
            logErr("unknown shape type used in inertia tensor calculation");
            break;
        }

        itComp.add(shapeComp);
    }

    void setPhysHandTensor(PhysHand& hand, worlds::DynamicPhysicsActor& dpa, physx::PxTransform& handT, Transform& objectT, entt::registry& reg) {
        // find offset of other physics actor
        auto otherT = dpa.actor->getGlobalPose();

        // calculate combined inertia tensor
        physx::IT::InertiaTensorComputer itComp(true);

        for (auto& shape : dpa.physicsShapes) {
            auto worldSpace = otherT * physx::PxTransform(worlds::glm2px(shape.pos), worlds::glm2px(shape.rot));

            auto handSpace = handT.getInverse() * worldSpace;

            auto scale = dpa.scaleShapes ? objectT.scale : glm::vec3{1.0f};

            addShapeTensor(reg, shape, itComp, handSpace, handT, scale, worldSpace, false);
        }

        for (auto& shape : dpa.physicsShapes) {
            auto shapeT = physx::PxTransform(worlds::glm2px(shape.pos), worlds::glm2px(shape.rot));
            addShapeTensor(reg, shape, itComp, shapeT, handT, glm::vec3{1.0f});
        }

        itComp.scaleDensity((dpa.mass + dpa.mass) / itComp.getMass());

        physx::PxMat33 it = itComp.getInertia();

        hand.overrideIT = it;
        hand.rotController.reset();
    }

    worlds::ConVar useTensorCompensation{"lg_compensateTensors", "1", "Enables inertia tensor compensation on grabs."};
    worlds::ConVar enableGripPoints { "lg_enableGripPoints", "1", "Enables grip points." };

    extern void resetHand(PhysHand& ph, physx::PxRigidBody* rb);

    void EventHandler::updateHandGrab(entt::registry& registry, PlayerRig& rig, entt::entity ent, float deltaTime) {
        static float driveP = 1500.0f;
        static float driveD = 0.0f;
        auto& physHand = registry.get<PhysHand>(ent);
        auto grabAction = physHand.follow == FollowHand::LeftHand ? lGrab : rGrab;
        auto grabButton = physHand.follow == FollowHand::LeftHand ? worlds::MouseButton::Left : worlds::MouseButton::Right;
        bool doGrab = vrInterface ? vrInterface->getActionPressed(grabAction) : inputManager->mouseButtonPressed(grabButton);
        bool doRelease = vrInterface ? vrInterface->getActionReleased(grabAction) : inputManager->mouseButtonReleased(grabButton);
        auto& handTf = registry.get<Transform>(ent);
        auto& dpa = registry.get<worlds::DynamicPhysicsActor>(ent);

        if (physHand.holdingObjectWithGrabPoint) {
            physHand.timeSinceGrabInitiated += deltaTime;
            auto& otherActor = registry.get<worlds::DynamicPhysicsActor>(physHand.holding);
            auto otherTf = worlds::px2glm(otherActor.actor->getGlobalPose());
            auto& gripPoint = registry.get<GripPoint>(physHand.holding);

            glm::vec3 targetHandPos = otherTf.position + (otherTf.rotation * gripPoint.offset);
            glm::quat targetHandRot = otherTf.rotation * gripPoint.rotOffset;
            float distance = glm::distance(handTf.position, targetHandPos);
            float rotDot = glm::dot(fixupQuat(targetHandRot), fixupQuat(handTf.rotation));
            ImGui::Text("%.3f distance, %.3f rotDot", distance, rotDot);

            if (distance < 0.01f && rotDot > 0.95f && physHand.timeSinceGrabInitiated > 0.25f) {
                auto& d6 = registry.get<worlds::D6Joint>(ent);
                logMsg("adding joint");
                physHand.useOverrideIT = true;
                physHand.holdingObjectWithGrabPoint = false;
            }
        }

        if (doGrab && !registry.has<worlds::D6Joint>(ent)) {
            // search for nearby grabbable objects
            physx::PxSphereGeometry sphereGeo{0.1f};
            physx::PxOverlapBuffer hit;
            physx::PxQueryFilterData filterData;
            filterData.flags = physx::PxQueryFlag::eDYNAMIC
                             | physx::PxQueryFlag::eSTATIC
                             | physx::PxQueryFlag::eANY_HIT
                             | physx::PxQueryFlag::ePOSTFILTER;

            worlds::FilterEntities filterEnt;
            filterEnt.ents[0] = (uint32_t)rig.lHand;
            filterEnt.ents[1] = (uint32_t)rig.rHand;
            filterEnt.ents[2] = (uint32_t)rig.locosphere;
            filterEnt.ents[3] = (uint32_t)rig.fender;
            filterEnt.ents[4] = (uint32_t)ent;
            filterEnt.numFilterEnts = 5;
            auto t = dpa.actor->getGlobalPose();
            auto overlapCenter = t;
            overlapCenter.p += t.q.rotate(physx::PxVec3(0.0f, 0.0f, 0.05f));

            if (worlds::g_scene->overlap(sphereGeo, overlapCenter, hit, filterData, &filterEnt)) {
                const auto& touch = hit.getAnyHit(0);
                auto pickUp = (entt::entity)(uint32_t)(uintptr_t)touch.actor->userData;

                if (registry.has<worlds::ScriptComponent>(pickUp)) {
                    scriptEngine->fireEvent(pickUp, "onGrab");
                }

                if (registry.valid(pickUp) && registry.valid(ent)) {
                    Transform otherTf = worlds::px2glm(touch.actor->getGlobalPose());
                    otherTf.scale = registry.get<Transform>(pickUp).scale;
                    auto* gripPoint = registry.try_get<GripPoint>(pickUp);
                    physHand.timeSinceGrabInitiated = 0.0f;

                    //auto& fj = registry.emplace<worlds::FixedJoint>(ent);
                    auto& d6 = registry.emplace<worlds::D6Joint>(ent);
                    physx::PxTransform p2 = touch.actor->getGlobalPose();

                    if (enableGripPoints.getInt() && gripPoint && (!gripPoint->exclusive || !gripPoint->currentlyHeld)) {
                        Transform handTfPx = worlds::px2glm(t);
                        physHand.holdingObjectWithGrabPoint = false;
                        physHand.holding = pickUp;
                        t.p = worlds::glm2px(otherTf.position + (otherTf.rotation * gripPoint->offset));
                        t.q = worlds::glm2px(otherTf.rotation * gripPoint->rotOffset);
                        Transform objGrabTf;
                        objGrabTf.position = handTfPx.position - (handTfPx.rotation * gripPoint->offset);
                        objGrabTf.rotation = handTfPx.rotation * gripPoint->rotOffset;
                        //dpa.actor->setGlobalPose(t);
                        //handTf.position = worlds::px2glm(t.p);
                        //handTf.rotation = worlds::px2glm(t.q);
                        //touch.actor->setGlobalPose(worlds::glm2px(objGrabTf));
                        physx::PxTransform target{worlds::glm2px(-gripPoint->offset), worlds::glm2px(glm::normalize(gripPoint->rotOffset))};
                        gripPoint->currentlyHeld = true;
                        d6.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0, target);
                        d6.pxJoint->setConstraintFlag(physx::PxConstraintFlag::eCOLLISION_ENABLED, false);
                        //d6.pxJoint->setMotion(physx::PxD6Axis::eX, physx::PxD6Motion::eFREE);
                        //d6.pxJoint->setMotion(physx::PxD6Axis::eY, physx::PxD6Motion::eFREE);
                        //d6.pxJoint->setMotion(physx::PxD6Axis::eZ, physx::PxD6Motion::eFREE);
                        //d6.pxJoint->setMotion(physx::PxD6Axis::eSWING1, physx::PxD6Motion::eFREE);
                        //d6.pxJoint->setMotion(physx::PxD6Axis::eSWING2, physx::PxD6Motion::eFREE);
                        //d6.pxJoint->setMotion(physx::PxD6Axis::eTWIST, physx::PxD6Motion::eFREE);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eX, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eY, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eZ, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eSWING1, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eSWING2, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eTWIST, physx::PxD6Motion::eLOCKED);
                        physx::PxD6JointDrive drive{driveP, driveD, PX_MAX_F32, true};
                        physx::PxD6JointDrive rDrive{driveP*2.0f, driveD*1.5f, PX_MAX_F32, true};
                        //d6.pxJoint->setDrive(physx::PxD6Drive::eX, drive);
                        //d6.pxJoint->setDrive(physx::PxD6Drive::eY, drive);
                        //d6.pxJoint->setDrive(physx::PxD6Drive::eZ, drive);
                        //d6.pxJoint->setDrive(physx::PxD6Drive::eSWING, rDrive);
                        //d6.pxJoint->setDrive(physx::PxD6Drive::eTWIST, rDrive);
                        //d6.pxJoint->setDrivePosition(physx::PxTransform{physx::PxIdentity});
                        //d6.pxJoint->setDriveVelocity(physx::PxVec3{0.0f}, physx::PxVec3{0.0f});
                        dpa.actor->setLinearVelocity(physx::PxVec3{0.0f});
                        dpa.actor->setAngularVelocity(physx::PxVec3{0.0f});
                        physHand.useOverrideIT = true;
                        physHand.holdingObjectWithGrabPoint = false;
                    } else {
                        d6.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0, t.transformInv(p2));
                        d6.pxJoint->setMotion(physx::PxD6Axis::eX, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eY, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eZ, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eSWING1, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eSWING2, physx::PxD6Motion::eLOCKED);
                        d6.pxJoint->setMotion(physx::PxD6Axis::eTWIST, physx::PxD6Motion::eLOCKED);
                        physHand.useOverrideIT = true;
                    }

                    d6.setTarget(pickUp, registry);
                    // mass of hands is 2kg
                    auto* otherDpa = registry.try_get<worlds::DynamicPhysicsActor>(pickUp);

                    if (otherDpa && useTensorCompensation.getInt()) {
                        setPhysHandTensor(physHand, *otherDpa, t, otherTf, *reg);
                    }
                }
            }
        }

        if (doRelease) {
            if (registry.has<worlds::D6Joint>(ent)) {
                auto& d6 = registry.get<worlds::D6Joint>(ent);
                auto heldEnt = d6.getTarget();
                GripPoint* gp = registry.try_get<GripPoint>(heldEnt);
                if (gp)
                    gp->currentlyHeld = false;
                registry.remove<worlds::D6Joint>(ent);
                auto& ph = registry.get<PhysHand>(ent);
                ph.useOverrideIT = false;
                ph.forceMultiplier = 1.0f;
                ph.holdingObjectWithGrabPoint = false;
            }
        }
    }

    void EventHandler::simulate(entt::registry& registry, float simStep) {
        mpManager->simulate(simStep);

        entt::entity localLocosphereEnt = entt::null;
        LocospherePlayerComponent* localLpc = nullptr;

        registry.view<LocospherePlayerComponent>().each([&](auto ent, auto& lpc) {
            if (lpc.isLocal) {
                if (!registry.valid(localLocosphereEnt)) {
                    localLocosphereEnt = ent;
                    localLpc = &lpc;
                } else {
                    logWarn("more than one local locosphere!");
                }
            }
        });

        if (!registry.valid(localLocosphereEnt)) {
            // probably dedicated server ¯\_(ツ)_/¯
            return;
        }

        auto& localRig = registry.get<PlayerRig>(localLocosphereEnt);

        updateHandGrab(registry, localRig, localRig.lHand, simStep);
        updateHandGrab(registry, localRig, localRig.rHand, simStep);
    }

    void EventHandler::onSceneStart(entt::registry& registry) {
        registry.view<worlds::DynamicPhysicsActor>().each([&](auto ent, auto&) {
            registry.emplace<SyncedRB>(ent);
        });

        // create our lil' pal the player
        if (!isDedicated && registry.view<PlayerStartPoint>().size() > 0) {
            entt::entity pspEnt = registry.view<PlayerStartPoint, Transform>().front();
            Transform& pspTf = registry.get<Transform>(pspEnt);

            PlayerRig rig = lsphereSys->createPlayerRig(registry, pspTf.position);
            auto& lpc = registry.get<LocospherePlayerComponent>(rig.locosphere);
            lpc.isLocal = true;
            lpc.sprint = false;
            lpc.maxSpeed = 0.0f;
            lpc.xzMoveInput = glm::vec2(0.0f, 0.0f);
            auto& stats = registry.emplace<RPGStats>(rig.locosphere);
            stats.strength = 5;

            if (vrInterface) {
                lGrab = vrInterface->getActionHandle("/actions/main/in/GrabL");
                rGrab = vrInterface->getActionHandle("/actions/main/in/GrabR");
                rStick = vrInterface->getActionHandle("/actions/main/in/RStick");
                camera->rotation = glm::quat{};
            }

            auto& fenderTransform = registry.get<Transform>(rig.fender);
            auto matId = worlds::g_assetDB.addOrGetExisting("Materials/VRHands/placeholder.json");
            auto devMatId = worlds::g_assetDB.addOrGetExisting("Materials/dev.json");
            //auto saberId = worlds::g_assetDB.addOrGetExisting("saber.wmdl");
            auto lHandModel = worlds::g_assetDB.addOrGetExisting("Models/VRHands/hand_placeholder_l.wmdl");
            auto rHandModel = worlds::g_assetDB.addOrGetExisting("Models/VRHands/hand_placeholder_r.wmdl");

            lHandEnt = registry.create();
            registry.get<PlayerRig>(rig.locosphere).lHand = lHandEnt;
            auto& lhWO = registry.emplace<worlds::WorldObject>(lHandEnt, matId, lHandModel);
            auto& lht = registry.emplace<Transform>(lHandEnt);
            lht.position = glm::vec3(0.5, 0.0f, 0.0f) + fenderTransform.position;
            registry.emplace<worlds::NameComponent>(lHandEnt).name = "L. Handy";

            fakeLHand = registry.create();
            registry.emplace<worlds::WorldObject>(fakeLHand, devMatId, lHandModel);
            registry.emplace<Transform>(fakeLHand);
            registry.emplace<worlds::NameComponent>(fakeLHand).name = "Fake L. Handy";

            rHandEnt = registry.create();
            registry.get<PlayerRig>(rig.locosphere).rHand = rHandEnt;
            auto& rhWO = registry.emplace<worlds::WorldObject>(rHandEnt, matId, rHandModel);
            auto& rht = registry.emplace<Transform>(rHandEnt);
            rht.position = glm::vec3(-0.5f, 0.0f, 0.0f) + fenderTransform.position;
            registry.emplace<worlds::NameComponent>(rHandEnt).name = "R. Handy";

            fakeRHand = registry.create();
            registry.emplace<worlds::WorldObject>(fakeRHand, devMatId, rHandModel);
            registry.emplace<Transform>(fakeRHand);
            registry.emplace<worlds::NameComponent>(fakeRHand).name = "Fake R. Handy";

            auto lActor = worlds::g_physics->createRigidDynamic(physx::PxTransform{ physx::PxIdentity });
            // Using the reference returned by this doesn't work unfortunately.
            registry.emplace<worlds::DynamicPhysicsActor>(lHandEnt, lActor);

            auto rActor = worlds::g_physics->createRigidDynamic(physx::PxTransform{ physx::PxIdentity });
            auto& rwActor = registry.emplace<worlds::DynamicPhysicsActor>(rHandEnt, rActor);
            auto& lwActor = registry.get<worlds::DynamicPhysicsActor>(lHandEnt);

            rwActor.physicsShapes.emplace_back(worlds::PhysicsShape::boxShape(glm::vec3{ 0.025f, 0.045f, 0.07f }));
            rwActor.physicsShapes[0].pos = glm::vec3{0.0f, 0.0f, 0.05f};
            lwActor.physicsShapes.emplace_back(worlds::PhysicsShape::boxShape(glm::vec3{ 0.025f, 0.045f, 0.07f }));
            lwActor.physicsShapes[0].pos = glm::vec3{0.0f, 0.0f, 0.05f};

            worlds::updatePhysicsShapes(rwActor);
            worlds::updatePhysicsShapes(lwActor);

            worlds::g_scene->addActor(*rActor);
            worlds::g_scene->addActor(*lActor);

            physx::PxRigidBodyExt::setMassAndUpdateInertia(*rActor, 2.0f);
            physx::PxRigidBodyExt::setMassAndUpdateInertia(*lActor, 2.0f);

            PIDSettings posSettings{ 750.0f, 638.0f, 137.0f };
            PIDSettings rotSettings{ 200.0f, 0.0f, 29.0f };

            auto& lHandPhys = registry.emplace<PhysHand>(lHandEnt);
            lHandPhys.locosphere = rig.locosphere;
            lHandPhys.posController.acceptSettings(posSettings);
            lHandPhys.posController.averageAmount = 5.0f;
            lHandPhys.rotController.acceptSettings(rotSettings);
            lHandPhys.rotController.averageAmount = 2.0f;
            lHandPhys.follow = FollowHand::LeftHand;

            auto& rHandPhys = registry.emplace<PhysHand>(rHandEnt);

            rHandPhys.locosphere = rig.locosphere;
            rHandPhys.posController.acceptSettings(posSettings);
            rHandPhys.posController.averageAmount = 5.0f;
            rHandPhys.rotController.acceptSettings(rotSettings);
            rHandPhys.rotController.averageAmount = 2.0f;
            rHandPhys.follow = FollowHand::RightHand;

            auto fenderActor = registry.get<worlds::DynamicPhysicsActor>(rig.fender).actor;

            lHandJoint = physx::PxD6JointCreate(*worlds::g_physics, fenderActor, physx::PxTransform { physx::PxIdentity }, lActor,
            physx::PxTransform { physx::PxIdentity });

            lHandJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0,
                    physx::PxTransform { physx::PxVec3 { 0.0f, 0.6f, 0.0f }, physx::PxQuat { physx::PxIdentity }});

            lHandJoint->setLinearLimit(physx::PxJointLinearLimit{physx::PxTolerancesScale{}, 0.8f});
            lHandJoint->setMotion(physx::PxD6Axis::eX, physx::PxD6Motion::eLIMITED);
            lHandJoint->setMotion(physx::PxD6Axis::eY, physx::PxD6Motion::eLIMITED);
            lHandJoint->setMotion(physx::PxD6Axis::eZ, physx::PxD6Motion::eLIMITED);
            lHandJoint->setMotion(physx::PxD6Axis::eSWING1, physx::PxD6Motion::eFREE);
            lHandJoint->setMotion(physx::PxD6Axis::eSWING2, physx::PxD6Motion::eFREE);
            lHandJoint->setMotion(physx::PxD6Axis::eTWIST, physx::PxD6Motion::eFREE);

            rHandJoint = physx::PxD6JointCreate(*worlds::g_physics, fenderActor, physx::PxTransform { physx::PxIdentity }, rActor,
            physx::PxTransform { physx::PxIdentity });

            rHandJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0,
                    physx::PxTransform { physx::PxVec3 { 0.0f, 0.6f, 0.0f }, physx::PxQuat { physx::PxIdentity }});

            rHandJoint->setLinearLimit(physx::PxJointLinearLimit{physx::PxTolerancesScale{}, 0.8f});
            rHandJoint->setMotion(physx::PxD6Axis::eX, physx::PxD6Motion::eLIMITED);
            rHandJoint->setMotion(physx::PxD6Axis::eY, physx::PxD6Motion::eLIMITED);
            rHandJoint->setMotion(physx::PxD6Axis::eZ, physx::PxD6Motion::eLIMITED);
            rHandJoint->setMotion(physx::PxD6Axis::eSWING1, physx::PxD6Motion::eFREE);
            rHandJoint->setMotion(physx::PxD6Axis::eSWING2, physx::PxD6Motion::eFREE);
            rHandJoint->setMotion(physx::PxD6Axis::eTWIST, physx::PxD6Motion::eFREE);
            lActor->setSolverIterationCounts(32, 16);
            rActor->setSolverIterationCounts(32, 16);
            lActor->setLinearVelocity(physx::PxVec3{0.0f});
            rActor->setLinearVelocity(physx::PxVec3{0.0f});
        }

        if (isDedicated) {
            mpManager->onSceneStart(registry);
        }

        g_dbgArrows->createEntities();
    }

    void EventHandler::shutdown(entt::registry& registry) {
        if (registry.valid(lHandEnt)) {
            registry.destroy(lHandEnt);
            registry.destroy(rHandEnt);
        }

        if (client)
            delete client;

        if (server)
            delete server;

        enet_deinitialize();
    }
}
