#include "Export.hpp"
#include "Core/MeshManager.hpp"

using namespace worlds;

extern "C" {
    EXPORT bool meshmanager_isMeshSkinned(AssetID id) {
        return MeshManager::loadOrGet(id).skinned;
    }

    EXPORT uint32_t meshmanager_getBoneId(AssetID id, const char* name) {
        const auto& m = MeshManager::loadOrGet(id);

        uint32_t i = 0;
        for (auto& boneName : m.boneNames) {
            if (boneName == name)
                return i;
            i++;
        }

        return ~0u;
    }

    EXPORT void meshmanager_getBoneRestTransform(AssetID id, uint32_t boneId, Transform* transform) {
        const auto& m = MeshManager::loadOrGet(id);
        transform->fromMatrix(glm::inverse(m.boneRestPositions[boneId]));
    }

    EXPORT void meshmanager_getBoneRelativeTransform(AssetID id, uint32_t boneId, Transform* transform) {
        const auto& m = MeshManager::loadOrGet(id);
        transform->fromMatrix(m.relativeBoneTransforms[boneId]);
    }

    EXPORT uint32_t meshmanager_getBoneCount(AssetID id) {
        const auto& m = MeshManager::loadOrGet(id);
        return m.boneNames.size();
    }
};