#include "DebugArrow.hpp"
#include "MathsUtil.hpp"
#include "AssetDB.hpp"
#include "CreateModelObject.hpp"

namespace converge {
    DebugArrows* g_dbgArrows;
    DebugArrows::DebugArrows(entt::registry& reg) : reg(reg), arrowsInUse(0) {
        g_dbgArrows = this;

        createEntities();
    }

    void DebugArrows::drawArrow(glm::vec3 start, glm::vec3 dir) {
        glm::vec3 ndir = glm::normalize(dir);
        glm::quat q = safeQuatLookat(ndir);

        auto ent = arrowEntities[arrowsInUse];
        arrowsInUse++;

        auto& t = reg.get<Transform>(ent);
        t.position = start;
        t.rotation = q;
    }

    void DebugArrows::newFrame() {
        for (auto& ent : arrowEntities) {
            auto& t = reg.get<Transform>(ent);
            t.position = glm::vec3(0.0f, -10000.0f, 0.0f);
        }

        arrowsInUse = 0;
    }
    
    void DebugArrows::createEntities() {
        for (auto& ent : arrowEntities) {
            if (reg.valid(ent))
                reg.destroy(ent);
        }

        arrowEntities.clear();
        auto meshId = worlds::g_assetDB.addOrGetExisting("arrow.obj");
        auto matId = worlds::g_assetDB.addOrGetExisting("Materials/glowred.json");
        for (size_t i = 0; i < 16; i++) {
            auto ent = worlds::createModelObject(reg, glm::vec3{0.0f}, glm::quat{}, meshId, matId);

            auto& t = reg.get<Transform>(ent); 
            t.position = glm::vec3(0.0f, -10000.0f, 0.0f);

            arrowEntities.push_back(ent);
        }
    }

    void DebugArrows::destroyEntities() {
        for (auto& ent : arrowEntities) {
            if (reg.valid(ent))
                reg.destroy(ent);
        }

        arrowEntities.clear();
    }
}
