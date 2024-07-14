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

#ifndef MIRACLEWM_WORKSPACE_CONTENT_H
#define MIRACLEWM_WORKSPACE_CONTENT_H

#include "animator.h"
#include "window_metadata.h"
#include "direction.h"
#include <glm/glm.hpp>
#include <memory>
#include <miral/minimal_window_manager.h>
#include <miral/window_manager_tools.h>

namespace miracle
{
class OutputContent;
class MiracleConfig;
class TilingWindowTree;
class WindowController;
class CompositorState;

class WorkspaceContent
{
public:
    WorkspaceContent(
        OutputContent* output,
        miral::WindowManagerTools const& tools,
        int workspace,
        std::shared_ptr<MiracleConfig> const& config,
        WindowController& window_controller,
        CompositorState const& state,
        miral::MinimalWindowManager& floating_window_manager);

    [[nodiscard]] int get_workspace() const;
    [[nodiscard]] std::shared_ptr<TilingWindowTree> const& get_tree() const;
    WindowType allocate_position(
        miral::ApplicationInfo const& app_info,
        miral::WindowSpecification& requested_specification,
        WindowType hint);
    std::shared_ptr<WindowMetadata> advise_new_window(
        miral::WindowInfo const& window_info, WindowType type);
    void show();
    void hide();
    void transfer_pinned_windows_to(std::shared_ptr<WorkspaceContent> const& other);
    void for_each_window(std::function<void(std::shared_ptr<WindowMetadata>)> const&);
    bool select_window_from_point(int x, int y);
    bool resize_active_window(miracle::Direction);
    bool select(miracle::Direction);
    void request_horizontal_layout();
    void request_vertical_layout();
    void toggle_layout();
    bool try_toggle_active_fullscreen();

    bool has_floating_window(miral::Window const&);
    void add_floating_window(miral::Window const&);
    void remove_floating_window(miral::Window const&);
    [[nodiscard]] std::vector<miral::Window> const& get_floating_windows() const;
    OutputContent* get_output();
    void trigger_rerender();
    [[nodiscard]] bool is_empty() const;
    static int workspace_to_number(int workspace);

private:
    OutputContent* output;
    miral::WindowManagerTools tools;
    std::shared_ptr<TilingWindowTree> tree;
    int workspace;
    std::vector<miral::Window> floating_windows;
    WindowController& window_controller;
    CompositorState const& state;
    std::shared_ptr<MiracleConfig> config;
    miral::MinimalWindowManager& floating_window_manager;
};

} // miracle

#endif // MIRACLEWM_WORKSPACE_CONTENT_H
