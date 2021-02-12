#include "ComponentEditorUtil.hpp"
#include "D6Joint.hpp"
#include <foundation/PxTransform.h>
#include "imgui.h"
#include <physx/PxPhysicsAPI.h>
#include <physx/extensions/PxJoint.h>
#include <physx/extensions/PxD6Joint.h>
#include "Physics.hpp"
#include "PhysicsActor.hpp"
#include "IconsFontAwesome5.h"
#include "GuiUtil.hpp"
#include "Log.hpp"
#include <entt/entity/registry.hpp>
#include "Fatal.hpp"

namespace worlds {
#define WRITE_FIELD(file, field) PHYSFS_writeBytes(file, &field, sizeof(field))
#define READ_FIELD(file, field) PHYSFS_readBytes(file, &field, sizeof(field))
    const char* motionNames[3] = {
        "Locked",
        "Limited",
        "Free"
    };

    const char* motionAxisLabels[physx::PxD6Axis::eCOUNT] = {
        "X Motion",
        "Y Motion",
        "Z Motion",
        "Twist Motion",
        "Swing 1 Motion",
        "Swing 2 Motion"
    };

    bool motionDropdown(const char* label, physx::PxD6Motion::Enum& val) {
        bool ret = false;
        if (ImGui::BeginCombo(label, motionNames[(int)val])) {
            for (int iType = 0; iType < 3; iType++) {
                auto type = (physx::PxD6Motion::Enum)iType;
                bool isSelected = val == type;
                if (ImGui::Selectable(motionNames[iType], &isSelected)) {
                    val = type;
                    ret = true;
                }

                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        return ret;
    }

    float readFloat(PHYSFS_File* file) {
        float f;
        PHYSFS_readBytes(file, &f, sizeof(f));
        return f;
    }

    class D6JointEditor : public BasicComponentUtil<D6Joint> {
    public:
        int getSortID() override { return 1; }
        const char* getName() override { return "D6 Joint"; }

        void create(entt::entity ent, entt::registry& reg) override {
            if (!reg.has<DynamicPhysicsActor>(ent)) {
                logWarn("Can't add a D6 joint to an entity without a dynamic physics actor!");
                return;
            }

            reg.emplace<D6Joint>(ent);
        }

        void edit(entt::entity ent, entt::registry& reg) override {
            auto& j = reg.get<D6Joint>(ent);
            if (!reg.has<DynamicPhysicsActor>(ent)) {
                reg.remove<D6Joint>(ent);
                return;
            }

            auto& dpa = reg.get<DynamicPhysicsActor>(ent);
            auto* pxj = j.pxJoint;

            if (ImGui::CollapsingHeader(ICON_FA_ATOM u8" D6 Joint")) {
                if (ImGui::Button("Remove##D6")) {
                    logMsg("removing d6");
                    reg.remove<D6Joint>(ent);
                    return;
                }

                dpa.actor->is<physx::PxRigidDynamic>()->wakeUp();
                for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eCOUNT; ((int&)axis)++) {
                    auto motion = j.pxJoint->getMotion(axis);
                    if (motionDropdown(motionAxisLabels[axis], motion)) {
                        j.pxJoint->setMotion(axis, motion);
                    }
                }

                auto t0 = j.pxJoint->getLocalPose(physx::PxJointActorIndex::eACTOR0);
                auto t1 = j.pxJoint->getLocalPose(physx::PxJointActorIndex::eACTOR1);

                if (ImGui::DragFloat3("Local Offset", &t0.p.x)) {
                    j.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0, t0);
                }

                if (ImGui::DragFloat3("Connected Offset", &t1.p.x)) {
                    j.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR1, t1);
                }

                if (!reg.valid(j.getTarget())) {
                    if (ImGui::Button("Set Connected Offset")) {
                        auto p = dpa.actor->getGlobalPose();
                        j.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR1, p);
                    }
                }

                if (ImGui::TreeNode("Linear Limits")) {
                    for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eTWIST; ((int&)axis)++) {
                        if (ImGui::TreeNode(motionAxisLabels[axis])) {
                            auto lim = j.pxJoint->getLinearLimit(axis);

                            ImGui::DragFloat("Lower", &lim.lower, 1.0f, -(PX_MAX_F32 / 3.0f), lim.upper);
                            ImGui::DragFloat("Upper", &lim.upper, 1.0f, lim.lower, (PX_MAX_F32 / 3.0f));
                            ImGui::DragFloat("Stiffness", &lim.stiffness);
                            tooltipHover("If greater than zero, the limit is soft, i.e. a spring pulls the joint back to the limit");
                            ImGui::DragFloat("Damping", &lim.damping);
                            ImGui::DragFloat("Contact Distance", &lim.contactDistance);
                            tooltipHover("The distance inside the limit value at which the limit will be considered to be active by the solver.");
                            ImGui::DragFloat("Bounce Threshold", &lim.bounceThreshold);
                            tooltipHover("The minimum velocity for which the limit will bounce.");
                            ImGui::DragFloat("Restitution", &lim.restitution);
                            tooltipHover("Controls the amount of bounce when the joint hits a limit.");

                            if (!lim.isValid()) {
                                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Invalid limit settings!");
                            }

                            j.pxJoint->setLinearLimit(axis, lim);
                            ImGui::TreePop();
                        }
                    }
                    ImGui::TreePop();
                }

                float localMassScale = 1.0f / j.pxJoint->getInvMassScale0();
                if (ImGui::DragFloat("Local Mass Scale", &localMassScale)) {
                    j.pxJoint->setInvMassScale0(1.0f / localMassScale);
                }

                float localInertiaScale = 1.0f / j.pxJoint->getInvInertiaScale0();
                if (ImGui::DragFloat("Local Inertia Scale", &localInertiaScale)) {
                    j.pxJoint->setInvInertiaScale0(1.0f / localInertiaScale);
                }

                float connectedMassScale = 1.0f / j.pxJoint->getInvMassScale1();
                if (ImGui::DragFloat("Connected Mass Scale", &connectedMassScale)) {
                    j.pxJoint->setInvMassScale0(1.0f / connectedMassScale);
                }

                float connectedInertiaScale = 1.0f / j.pxJoint->getInvInertiaScale1();
                if (ImGui::DragFloat("Connected Inertia Scale", &connectedInertiaScale)) {
                    j.pxJoint->setInvInertiaScale0(1.0f / connectedInertiaScale);
                }

                float breakForce, breakTorque;
                pxj->getBreakForce(breakForce, breakTorque);

                if (ImGui::DragFloat("Break Torque", &breakTorque)) {
                    pxj->setBreakForce(breakForce, breakTorque);
                }

                if (ImGui::DragFloat("Break Force", &breakForce)) {
                    pxj->setBreakForce(breakForce, breakTorque);
                }
            }
        }

        void clone(entt::entity from, entt::entity to, entt::registry& reg) override {
            assert(reg.has<DynamicPhysicsActor>(to));
            auto& dpa = reg.get<DynamicPhysicsActor>(to);
            auto& newD6 = reg.emplace<D6Joint>(to);
            auto& oldD6 = reg.get<D6Joint>(from);

            auto* newJ = newD6.pxJoint;
            auto* oldJ = oldD6.pxJoint;

            if (reg.valid(oldD6.getTarget()))
                newD6.setTarget(oldD6.getTarget(), reg);

            for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eCOUNT; ((int&)axis)++) {
                newD6.pxJoint->setMotion(axis, oldD6.pxJoint->getMotion(axis));
            }

            newD6.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0,
                oldD6.pxJoint->getLocalPose(physx::PxJointActorIndex::eACTOR0));

            newD6.pxJoint->setLocalPose(physx::PxJointActorIndex::eACTOR1,
                oldD6.pxJoint->getLocalPose(physx::PxJointActorIndex::eACTOR1));

            for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eTWIST; ((int&)axis)++) {
                newJ->setLinearLimit(axis, oldJ->getLinearLimit(axis));
            }
        }

        void writeToFile(entt::entity ent, entt::registry& reg, PHYSFS_File* file) override {
            auto& d6 = reg.get<D6Joint>(ent);
            auto* px = d6.pxJoint;
            
            for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eCOUNT; ((int&)axis)++) {
                auto motion = (unsigned char)px->getMotion(axis);
                WRITE_FIELD(file, motion);
            }

            auto p0 = px->getLocalPose(physx::PxJointActorIndex::eACTOR0);
            auto p1 = px->getLocalPose(physx::PxJointActorIndex::eACTOR1);

            WRITE_FIELD(file, p0);
            WRITE_FIELD(file, p1);

            for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eTWIST; ((int&)axis)++) {
                auto lim = px->getLinearLimit(axis);
                WRITE_FIELD(file, lim);
            }

            float invMS0 = px->getInvMassScale0();
            float invMS1 = px->getInvMassScale1();
            float invIS0 = px->getInvInertiaScale0();
            float invIS1 = px->getInvInertiaScale1();

            WRITE_FIELD(file, invMS0);
            WRITE_FIELD(file, invMS1);
            WRITE_FIELD(file, invIS0);
            WRITE_FIELD(file, invIS1);

            float breakTorque, breakForce;
            px->getBreakForce(breakForce, breakTorque);

            WRITE_FIELD(file, breakTorque);
            WRITE_FIELD(file, breakForce);
        }

        void readFromFile(entt::entity ent, entt::registry& reg, PHYSFS_File* file, int version) override {
            assert(reg.has<DynamicPhysicsActor>(ent));

            auto& d6 = reg.emplace<D6Joint>(ent);
            auto* px = d6.pxJoint;

            for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eCOUNT; ((int&)axis)++) {
                unsigned char motion;
                READ_FIELD(file, motion);
                px->setMotion(axis, (physx::PxD6Motion::Enum)motion);
            }

            physx::PxTransform p0;
            physx::PxTransform p1;

            READ_FIELD(file, p0);
            READ_FIELD(file, p1);

            px->setLocalPose(physx::PxJointActorIndex::eACTOR0, p0);
            px->setLocalPose(physx::PxJointActorIndex::eACTOR1, p1);

            for (physx::PxD6Axis::Enum axis = physx::PxD6Axis::eX; axis < physx::PxD6Axis::eTWIST; ((int&)axis)++) {
                physx::PxJointLinearLimitPair lim{ 0.0f, 0.0f, physx::PxSpring{0.0f, 0.0f} };
                READ_FIELD(file, lim);
                px->setLinearLimit(axis, lim);
            }

            if (version >= 2) {
                px->setInvMassScale0(readFloat(file));
                px->setInvMassScale1(readFloat(file));
                px->setInvInertiaScale0(readFloat(file));
                px->setInvInertiaScale1(readFloat(file));

                float breakTorque = readFloat(file);
                float breakForce = readFloat(file);
                px->setBreakForce(breakForce, breakTorque);
            }
        }
    };

    D6JointEditor d6Ed;
}
