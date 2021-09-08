#include "Editor/Editor.hpp"
#include "Export.hpp"

using namespace worlds;

extern Editor* csharpEditor;
extern "C" {
    EXPORT uint32_t editor_getCurrentlySelected() {
        return (uint32_t)csharpEditor->getSelectedEntity();
    }

    EXPORT void editor_select(uint32_t ent) {
        csharpEditor->select((entt::entity)ent);
    }
}