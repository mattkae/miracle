/**
Copyright (C) 2024  Matthew Kosarek

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#ifndef MIRACLE_SCREEN_H
#define MIRACLE_SCREEN_H

#include "animator.h"
#include "direction.h"
#include "miral/window.h"
#include "tiling_window_tree.h"

#include "minimal_window_manager.h"
#include "workspace.h"
#include <memory>
#include <miral/output.h>
#include <nlohmann/json.hpp>

namespace miracle
{
class WorkspaceManager;
class Config;
class WindowManagerToolsWindowController;
class CompositorState;
class Animator;

struct WorkspaceCreationData
{
    uint32_t id;
    std::optional<int> num;
    std::optional<std::string> name;
};

class Output
{
public:
    Output(
        miral::Output const& output,
        WorkspaceManager& workspace_manager,
        geom::Rectangle const& area,
        std::shared_ptr<MinimalWindowManager> const& floating_window_manager,
        CompositorState& state,
        std::shared_ptr<Config> const& options,
        WindowController&,
        Animator&);
    ~Output() = default;

    std::shared_ptr<Container> intersect(float x, float y);
    AllocationHint allocate_position(
        miral::ApplicationInfo const& app_info,
        miral::WindowSpecification& requested_specification,
        AllocationHint hint = AllocationHint());
    [[nodiscard]] std::shared_ptr<Container> create_container(
        miral::WindowInfo const& window_info, AllocationHint const& hint) const;
    void delete_container(std::shared_ptr<Container> const& container);
    void advise_new_workspace(WorkspaceCreationData const&&);
    void advise_workspace_deleted(uint32_t id);
    bool advise_workspace_active(uint32_t id);
    void advise_application_zone_create(miral::Zone const& application_zone);
    void advise_application_zone_update(miral::Zone const& updated, miral::Zone const& original);
    void advise_application_zone_delete(miral::Zone const& application_zone);
    bool point_is_in_output(int x, int y);
    void update_area(geom::Rectangle const& area);

    /// Immediately requests that the provided window be added to the output
    /// with the provided type. This is a deviation away from the typical
    /// window-adding flow where you first call 'place_new_window' followed
    /// by 'create_container'.
    void add_immediately(miral::Window& window, AllocationHint hint = AllocationHint());

    /// Takes an existing [Container] object and places it in an appropriate position
    /// on the active [Workspace].
    void graft(std::shared_ptr<Container> const& container);
    void set_transform(glm::mat4 const& in);
    void set_position(glm::vec2 const&);

    // Getters

    [[nodiscard]] std::vector<miral::Window> collect_all_windows() const;
    [[nodiscard]] Workspace* active() const;
    [[nodiscard]] std::vector<std::shared_ptr<Workspace>> const& get_workspaces() const { return workspaces; }
    [[nodiscard]] geom::Rectangle const& get_area() const { return area; }
    [[nodiscard]] std::vector<miral::Zone> const& get_app_zones() const { return application_zone_list; }
    [[nodiscard]] miral::Output const& get_output() const { return output; }
    [[nodiscard]] bool is_active() const;
    [[nodiscard]] glm::mat4 get_transform() const;
    /// Gets the relative position of the current rectangle (e.g. the active
    /// rectangle with be at position (0, 0))
    [[nodiscard]] geom::Rectangle get_workspace_rectangle(size_t i) const;
    [[nodiscard]] Workspace const* workspace(uint32_t id) const;
    [[nodiscard]] nlohmann::json to_json() const;

private:
    class WorkspaceAnimation : public Animation
    {
    public:
        WorkspaceAnimation(
            AnimationHandle handle,
            AnimationDefinition definition,
            mir::geometry::Rectangle const& from,
            mir::geometry::Rectangle const& to,
            mir::geometry::Rectangle const& current,
            std::shared_ptr<Workspace> const& to_workspace,
            std::shared_ptr<Workspace> const& from_workspace,
            Output* output);

        void on_tick(AnimationStepResult const&) override;

    private:
        std::shared_ptr<Workspace> to_workspace;
        std::shared_ptr<Workspace> from_workspace;
        Output* output;
    };

    void on_workspace_animation(
        AnimationStepResult const& result,
        std::shared_ptr<Workspace> const& to,
        std::shared_ptr<Workspace> const& from);

    miral::Output output;
    WorkspaceManager& workspace_manager;
    std::shared_ptr<MinimalWindowManager> floating_window_manager;
    CompositorState& state;
    geom::Rectangle area;
    std::shared_ptr<Config> config;
    WindowController& window_controller;
    Animator& animator;
    std::weak_ptr<Workspace> active_workspace;
    std::vector<std::shared_ptr<Workspace>> workspaces;
    std::vector<miral::Zone> application_zone_list;
    bool is_active_ = false;
    AnimationHandle handle;

    /// The position of the output for scrolling across workspaces
    glm::vec2 position_offset = glm::vec2(0.f);

    /// The transform applied to the workspace
    glm::mat4 transform = glm::mat4(1.f);

    /// A matrix resulting from combining position + transform
    glm::mat4 final_transform = glm::mat4(1.f);
};

}

#endif
