#include "GuiUtil.hpp"
#include "../Core/Engine.hpp"
#include "../Libs/IconsFontAwesome5.h"
#include "../Libs/IconsFontaudio.h"
#include "ImGui/imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "../ImGui/imgui_internal.h"
#include "../Core/Log.hpp"
#include <filesystem>

namespace worlds {
    const char* getIcon(const std::string& extension) {
        if (extension == ".escn" || extension == ".wscn") {
            return (const char*)(ICON_FA_MAP u8" ");
        } else if (extension == ".ogg") {
            return (const char*)(ICON_FAD_SPEAKER u8" ");
        } else if (extension == ".crn") {
            return (const char*)(ICON_FA_IMAGE u8" ");
        } else if (extension == ".obj" || extension == ".wmdl" || extension == ".mdl") {
            return (const char*)(ICON_FA_SHAPES u8" ");
        }
        return "      ";
    }

    void listDirectoryItems(std::string path,
            std::function<void(std::string, PHYSFS_FileType)> onClick,
            std::function<void(std::string, PHYSFS_FileType)> onDoubleClick) {
            if (!path.empty()) {
                ImGui::Text("..");
                if (ImGui::IsItemClicked()) {
                    std::filesystem::path p(path);
                    onDoubleClick(p.parent_path().string(), PHYSFS_FILETYPE_DIRECTORY);
                }
            }

            char** files = PHYSFS_enumerateFiles(path.c_str());
            for (char** currFile = files; *currFile != nullptr; currFile++) {
                std::string absPath = path.empty() ? *currFile : (path + "/" + (*currFile));
                PHYSFS_Stat stat;
                PHYSFS_stat(absPath.c_str(), &stat);
                std::string extension = std::filesystem::path(*currFile).extension().string();

                const char* icon = "  ";

                if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
                    icon = (const char*)(ICON_FA_FOLDER u8" ");
                } else icon = getIcon(extension);

                ImGui::Text("%s%s%s", icon, *currFile, stat.filetype == PHYSFS_FILETYPE_DIRECTORY ? "/" : "");

                if (ImGui::IsItemClicked()) {
                    if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
                        onClick(absPath, stat.filetype);
                        logMsg("selected %s", absPath.c_str());
                    } else {
                        logMsg("navigated to %s", absPath.c_str());
                    }
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    onDoubleClick(absPath, stat.filetype);
                }
            }

            PHYSFS_freeList(files);
    }

    void saveFileModal(const char* title, std::function<void(const char*)> saveCallback) {
        ImVec2 popupSize(windowSize.x - 50.0f, windowSize.y - 50.0f);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos + ImVec2((windowSize.x / 2) - (popupSize.x / 2), (windowSize.y / 2) - (popupSize.y / 2)));
        ImGui::SetNextWindowSize(popupSize);

        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            std::string* currentFile = (std::string*)ImGui::GetStateStorage()->GetVoidPtr(ImGui::GetID("file"));
            std::string* currentDirectory = (std::string*)ImGui::GetStateStorage()->GetVoidPtr(ImGui::GetID("savedir"));

            if (currentFile == nullptr) {
                currentFile = new std::string;
                ImGui::GetStateStorage()->SetVoidPtr(ImGui::GetID("file"), currentFile);
            }

            if (currentDirectory == nullptr) {
                currentDirectory = new std::string;
                ImGui::GetStateStorage()->SetVoidPtr(ImGui::GetID("savedir"), currentDirectory);
            }

            ImGui::Text("%s", currentDirectory->c_str());

            ImGui::BeginChild("Stuffs", ImVec2(ImGui::GetWindowWidth() - 17.0f, ImGui::GetWindowHeight() - 90.0f), true);

            listDirectoryItems(*currentDirectory,
                [&](std::string path, PHYSFS_FileType type) {
                     if (type == PHYSFS_FILETYPE_DIRECTORY) *currentDirectory = path;
                     else *currentFile = std::filesystem::path(path).filename().string();
                },
                [&](std::string path, PHYSFS_FileType type) {
                     if (type == PHYSFS_FILETYPE_DIRECTORY) *currentDirectory = path;
                     else {
                        *currentFile = std::filesystem::path(path).filename().string();
                        saveCallback((*currentDirectory + "/" + *currentFile).c_str());
                     }
                });


            ImGui::EndChild();

            ImGui::PushItemWidth(ImGui::GetWindowWidth() - 109.0f);
            ImGui::InputText("", currentFile);

            std::string fullPath = *currentDirectory + "/" + *currentFile;

            ImGui::SameLine();
            if (ImGui::Button("OK") && !fullPath.empty()) {
                logMsg("fullPath: %s", fullPath.c_str());
                saveCallback(fullPath.c_str());
                ImGui::GetStateStorage()->Clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::GetStateStorage()->Clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void openFileModal(const char* title, std::function<void(const char*)> openCallback, const char** fileExtensions, int fileExtensionCount, const char* startingDir) {
        ImVec2 popupSize(windowSize.x - 50.0f, windowSize.y - 50.0f);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos + ImVec2((windowSize.x / 2) - (popupSize.x / 2), (windowSize.y / 2) - (popupSize.y / 2)));
        ImGui::SetNextWindowSize(popupSize);

        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            std::string* fullFilePath = (std::string*)ImGui::GetStateStorage()->GetVoidPtr(ImGui::GetID("savepath"));
            std::string* currentDirectory = (std::string*)ImGui::GetStateStorage()->GetVoidPtr(ImGui::GetID("savedir"));

            if (fullFilePath == nullptr) {
                fullFilePath = new std::string;
                ImGui::GetStateStorage()->SetVoidPtr(ImGui::GetID("savepath"), fullFilePath);
            }

            if (currentDirectory == nullptr) {
                currentDirectory = new std::string;
                if (startingDir)
                    *currentDirectory = startingDir;
                ImGui::GetStateStorage()->SetVoidPtr(ImGui::GetID("savedir"), currentDirectory);
            }

            ImGui::Text("%s", currentDirectory->c_str());

            ImGui::BeginChild("Stuffs", ImVec2(ImGui::GetWindowWidth() - 17.0f, ImGui::GetWindowHeight() - 90.0f), true);
            char** files = PHYSFS_enumerateFiles(currentDirectory->c_str());

            std::string titleDoesntExist = "File Doesn't Exist##";
            titleDoesntExist += title;

            if (!currentDirectory->empty()) {
                ImGui::Text("..");
                if (ImGui::IsItemClicked()) {
                    std::filesystem::path p(*currentDirectory);
                    *currentDirectory = p.parent_path().string();
                }
            }

            for (char** currFile = files; *currFile != nullptr; currFile++) {
                std::string absPath = currentDirectory->empty() ? *currFile : (*currentDirectory + "/" + (*currFile));
                PHYSFS_Stat stat;
                PHYSFS_stat(absPath.c_str(), &stat);
                std::string extension = std::filesystem::path(*currFile).extension().string();

                bool hasExtension = fileExtensionCount == 0;

                for (int i = 0; i < fileExtensionCount; i++) {
                    if (fileExtensions[i] == extension)
                        hasExtension = true;
                }

                if (!hasExtension && stat.filetype == PHYSFS_FILETYPE_REGULAR)
                    continue;

                const char* icon = "  ";

                if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
                    icon = (const char*)(ICON_FA_FOLDER u8" ");
                } else icon = getIcon(extension);

                ImGui::Text("%s%s%s", icon, *currFile, stat.filetype == PHYSFS_FILETYPE_DIRECTORY ? "/" : "");

                if (ImGui::IsItemClicked()) {
                    if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
                        *fullFilePath = absPath;
                        logMsg("selected %s", fullFilePath->c_str());
                    } else {
                        if (*currentDirectory != "/")
                            *currentDirectory += "/";
                        *currentDirectory += *currFile;

                        if ((*currentDirectory)[0] == '/') {
                            *currentDirectory = currentDirectory->substr(1);
                        }

                        logMsg("navigated to %s", currentDirectory->c_str());
                    }
                }

                if (ImGui::IsItemHovered() && stat.filetype == PHYSFS_FILETYPE_REGULAR) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (!PHYSFS_exists(absPath.c_str())) {
                            ImGui::OpenPopup(titleDoesntExist.c_str());
                        } else {
                            openCallback(absPath.c_str());
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            ImGui::EndChild();

            ImGui::PushItemWidth(ImGui::GetWindowWidth() - 109.0f);
            ImGui::InputText("", fullFilePath);

            ImGui::SameLine();
            if (ImGui::Button("OK") && !fullFilePath->empty()) {
                if (!PHYSFS_exists(fullFilePath->c_str())) {
                    ImGui::OpenPopup(titleDoesntExist.c_str());
                } else {
                    openCallback(fullFilePath->c_str());
                    ImGui::GetStateStorage()->Clear();
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::GetStateStorage()->Clear();
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::BeginPopup(titleDoesntExist.c_str())) {
                ImGui::Text("File not found.");
                ImGui::EndPopup();
            }

            ImGui::EndPopup();
        }
    }

    void openFileModal(const char* title, std::function<void(const char*)> openCallback, const char* fileExtension, const char* startingDir) {
        int extCount = fileExtension != nullptr;
        openFileModal(title, openCallback, &fileExtension, extCount, startingDir);
    }

    void messageBoxModal(const char* title, const char* desc, std::function<void(bool)> callback) {
        ImVec2 popupSize(500.0f, 150.0f);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos + ImVec2((windowSize.x / 2) - (popupSize.x / 2), (windowSize.y / 2) - (popupSize.y / 2)));
        ImGui::SetNextWindowSize(popupSize);

        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("%s", desc);

            if (ImGui::Button("Yes")) {
                callback(true);
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("No")) {
                callback(false);
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    bool selectAssetPopup(const char* title, AssetID& id, bool open) {
        static std::string path;
        bool changed = false;

        std::filesystem::path p(path);
        p = p.parent_path();

        openFileModal(title, [&](const char* path) {
            id = g_assetDB.addOrGetExisting(path);
            changed = true;
        }, nullptr, p.string().c_str());

        if (open) {
            if (id != ~0u)
                path = g_assetDB.getAssetPath(id);
            else
                path = "";
            ImGui::OpenPopup(title);
        }

        return changed;
    }

    // Based on code from the ImGui demo
    void tooltipHover(const char* desc) {
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}
