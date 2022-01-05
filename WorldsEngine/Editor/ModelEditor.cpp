#include "ModelEditor.hpp"
#include <ImGui/imgui.h>
#include <Core/MeshManager.hpp>
#include <Editor/GuiUtil.hpp>
#include <IO/IOUtil.hpp>
#include <nlohmann/json.hpp>
#include <AssetCompilation/AssetCompilerUtil.hpp>

namespace worlds {
    void ModelEditor::importAsset(std::string filePath, std::string newAssetPath) {
        AssetID id = AssetDB::createAsset(newAssetPath);
        PHYSFS_File* f = PHYSFS_openWrite(("SourceData/" + newAssetPath).c_str());
        nlohmann::json j = {
            { "srcPath", filePath }
        };
        std::string serializedJson = j.dump(4);
        PHYSFS_writeBytes(f, serializedJson.data(), serializedJson.size());
        PHYSFS_close(f);
        open(id);
    }

    void ModelEditor::create(std::string path) {
        AssetID id = AssetDB::createAsset(path);
        PHYSFS_File* f = PHYSFS_openWrite(path.c_str());
        const char emptyJson[] = "{}";
        PHYSFS_writeBytes(f, emptyJson, sizeof(emptyJson));
        PHYSFS_close(f);
        open(id);
    }

    void ModelEditor::open(AssetID id) {
        editingID = id;

        std::string contents = LoadFileToString(AssetDB::idToPath(id)).value;
        try {
            nlohmann::json j = nlohmann::json::parse(contents);
            srcModel = AssetDB::pathToId(j.value("srcPath", "Raw/Models/cube.obj"));
            preTransformVerts = j.value("preTransformVerts", false);
            uniformScale = j.value("uniformScale", 1.0f);
        } catch (nlohmann::detail::exception except) {
            addNotification(("Error opening " + AssetDB::idToPath(id)), NotificationType::Error);
            srcModel = INVALID_ASSET;
        }
    }

    void ModelEditor::drawEditor() {
        ImGui::Text("Source model: %s", AssetDB::idToPath(srcModel).c_str());
        ImGui::SameLine();
        selectRawAssetPopup("Source Model", srcModel, ImGui::Button("Change##SrcModel"));
        ImGui::Checkbox("Pre-Transform Vertices", &preTransformVerts);
        ImGui::DragFloat("Uniform Scaling", &uniformScale);

        if (AssetDB::exists(srcModel)) {
            AssetID outputAsset = getOutputAsset(AssetDB::idToPath(editingID));
            ImGui::Text("Compiled path: %s", AssetDB::idToPath(outputAsset).c_str());

            if (AssetDB::exists(outputAsset)) {
                if (ImGui::Button("Refresh")) {
                    MeshManager::unload(outputAsset);
                }

                const LoadedMesh& lm = MeshManager::loadOrGet(outputAsset);
                ImGui::Text("%i submeshes", lm.numSubmeshes);

                if (ImGui::TreeNode("Submeshes")) {
                    for (int i = 0; i < lm.numSubmeshes; i++) {
                        ImGui::Text("%i indices", lm.submeshes[i].indexCount);
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    void ModelEditor::save() {
        nlohmann::json j = {
            { "srcPath", AssetDB::idToPath(srcModel) },
            { "uniformScale", uniformScale }
        };

        if (preTransformVerts)
            j["preTransformVerts"] = true;

        std::string s = j.dump(4);
        std::string path = AssetDB::idToPath(editingID);
        PHYSFS_File* file = PHYSFS_openWrite(path.c_str());
        PHYSFS_writeBytes(file, s.data(), s.size());
        PHYSFS_close(file);
    }

    const char* ModelEditor::getHandledExtension() {
        return ".wmdlj";
    }
}
