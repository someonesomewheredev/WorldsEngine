#include "SceneSerialization.hpp"
#include "../Core/AssetDB.hpp"
#include "../Core/Engine.hpp"
#include "../Core/Transform.hpp"
#include "../Physics/PhysicsActor.hpp"
#include "../Physics/Physics.hpp"
#include <filesystem>
#include "../Core/Log.hpp"
#include "../Util/TimingUtil.hpp"
#include "SceneSerializationFuncs.hpp"
#include "../ComponentMeta/ComponentMetadata.hpp"

namespace worlds {
    std::function<void(entt::registry&)> onSceneLoad;

    typedef void(*LoadSceneFunc)(PHYSFS_File*, entt::registry&, bool);

    const LoadSceneFunc idFuncs[] = {nullptr, loadScene01, loadScene02, loadScene03, loadScene04};

    const unsigned char ESCN_FORMAT_MAGIC[5] = { 'E','S','C','N', '\0' };
    const unsigned char WSCN_FORMAT_MAGIC[5] = "WSCN";
    const int MAX_ESCN_FORMAT_ID = 4;
    const int MAX_WSCN_FORMAT_ID = 4;

    bool deserializeEScene(PHYSFS_File* file, entt::registry& reg, 
        bool additive, char* magicCheck, unsigned char formatId) {
        if (formatId > MAX_ESCN_FORMAT_ID) {
            logErr(WELogCategoryEngine, "scene has incompatible format id: got %i, expected %i or lower", formatId, MAX_ESCN_FORMAT_ID);
            return false;
        }

        if (memcmp(magicCheck, ESCN_FORMAT_MAGIC, 4) != 0) {
            logErr(WELogCategoryEngine, "failed magic check: got %s, expected %s", magicCheck, ESCN_FORMAT_MAGIC);
            return false;
        }

        logMsg("Loading experimental scene version %i", formatId);

        idFuncs[formatId](file, reg, additive);
        return true;
    }

    void saveSceneToFile(PHYSFS_File* file, entt::registry& reg) {
        PerfTimer timer;

        PHYSFS_writeBytes(file, WSCN_FORMAT_MAGIC, 4);
        PHYSFS_writeBytes(file, &MAX_WSCN_FORMAT_ID, 1);

        uint32_t numEnts = (uint32_t)reg.view<Transform>().size();
        PHYSFS_writeULE32(file, numEnts);

        reg.view<Transform>().each([file, &reg](entt::entity ent, Transform&) {
            PHYSFS_writeULE32(file, (uint32_t)ent); 

            uint8_t numComponents = 0;

            for (auto& mdata : ComponentMetadataManager::sorted) {
                std::array<ENTT_ID_TYPE, 1> arr = { mdata->getComponentID() };
                auto rView = reg.runtime_view(arr.begin(), arr.end());

                if (!rView.contains(ent)) continue;
                numComponents++;
            }

            PHYSFS_writeBytes(file, &numComponents, 1);

            for (auto& mdata : ComponentMetadataManager::sorted) {
                std::array<ENTT_ID_TYPE, 1> arr = { mdata->getComponentID() };
                auto rView = reg.runtime_view(arr.begin(), arr.end());

                if (!rView.contains(ent)) continue;

                PHYSFS_writeULE32(file, mdata->getSerializedID());
                mdata->writeToFile(ent, reg, file);
            }
        });

        PHYSFS_close(file);

        logMsg("Saved scene in %.3fms", timer.stopGetMs());

        g_assetDB.save();
    }

    void saveScene(AssetID id, entt::registry& reg) {
        PHYSFS_File* file = g_assetDB.openAssetFileWrite(id);
        saveSceneToFile(file, reg);
    }

    bool deserializeWScene(PHYSFS_File* file, entt::registry& reg, 
        bool additive, char* magicCheck, unsigned char formatId) {
        PerfTimer timer;

        if (formatId > MAX_WSCN_FORMAT_ID) {
            logErr(WELogCategoryEngine, "Tried to load a wscn that is too new (%i, while we can load up to %i)", 
                formatId, MAX_WSCN_FORMAT_ID);
            return false;
        }

        if (memcmp(magicCheck, WSCN_FORMAT_MAGIC, 4) != 0) {
            logErr(WELogCategoryEngine, "failed magic check: got %s, expected %s", magicCheck, WSCN_FORMAT_MAGIC);
            return false;
        }

        logMsg("Loading WSCN version %i", formatId);

        if (!additive)
            reg.clear();

        uint32_t numEntities;
        PHYSFS_readULE32(file, &numEntities);

        for (uint32_t i = 0; i < numEntities; i++) {
            uint32_t oldEntId;
            PHYSFS_readULE32(file, &oldEntId);

            auto newEnt = reg.create((entt::entity)oldEntId);

            uint8_t numComponents;
            PHYSFS_readBytes(file, &numComponents, 1);

            for (uint8_t j = 0; j < numComponents; j++) {
                uint32_t compType = ~0u;
                PHYSFS_readULE32(file, &compType);

                auto* mdata = ComponentMetadataManager::bySerializedID.at(compType);
                mdata->readFromFile(newEnt, reg, file, formatId);
            }
        }

        logMsg("loaded WSCN in %.3fms", timer.stopGetMs());
        return true;
    }

    void deserializeScene(PHYSFS_File* file, entt::registry& reg, bool additive) {
        char magicCheck[5];
        magicCheck[4] = 0; // Null byte so we can interpret it as a C string if we really need to
        PHYSFS_readBytes(file, magicCheck, 4);
        unsigned char formatId;
        PHYSFS_readBytes(file, &formatId, sizeof(formatId));

        // check first character of magic to determine WSCN or ESCN
        if (magicCheck[0] == 'W') {
            if (!deserializeWScene(file, reg, additive, magicCheck, formatId)) {
                PHYSFS_close(file);
                return;
            }
        } else if (magicCheck[0] == 'E') {
            if (!deserializeEScene(file, reg, additive, magicCheck, formatId)) {
                PHYSFS_close(file);
                return;
            }
        } else {
            logMsg(WELogCategoryEngine, "Scene has unrecognized magic %s", magicCheck);
            PHYSFS_close(file);
            return;
        }

        PHYSFS_close(file);
        if (onSceneLoad)
            onSceneLoad(reg);
    }

    void deserializeScene(AssetID id, entt::registry& reg, bool additive) {
        PHYSFS_File* file = g_assetDB.openAssetFileRead(id);
        deserializeScene(file, reg, additive);
    }
}
