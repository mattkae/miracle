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

#ifndef MIRACLEWM_LEAF_NODE_H
#define MIRACLEWM_LEAF_NODE_H

#include "container.h"
#include "node_common.h"
#include "window_controller.h"
#include <miral/window.h>
#include <miral/window_manager_tools.h>
#include <optional>

namespace geom = mir::geometry;

namespace miracle
{

class MiracleConfig;
class TilingWindowTree;

/// A leaf container contains one or many windows
/// (in the event that windows are stacked or tabbed)
class LeafContainer : public Container
{
public:
    LeafContainer(
        WindowController& node_interface,
        geom::Rectangle area,
        std::shared_ptr<MiracleConfig> const& config,
        TilingWindowTree* tree,
        std::shared_ptr<ParentContainer> const& parent);

    void associate_to_window(miral::Window const&);
    [[nodiscard]] geom::Rectangle get_logical_area() const override;
    [[nodiscard]] geom::Rectangle get_visible_area() const override;
    void set_logical_area(geom::Rectangle const& target_rect) override;
    void set_parent(std::shared_ptr<ParentContainer> const&) override;
    void set_state(MirWindowState state);
    void show();
    void hide();
    bool is_fullscreen() const;
    void constrain() override;
    size_t get_min_width() const override;
    size_t get_min_height() const override;
    void handle_ready() override;
    void handle_modify(miral::WindowSpecification const&) override;
    void handle_raise() override;
    bool resize(Direction direction) override;
    bool toggle_fullscreen() override;
    mir::geometry::Rectangle confirm_placement(
        MirWindowState, mir::geometry::Rectangle const&) override;
    void on_open() override;
    void on_focus_gained() override;
    void on_focus_lost() override;
    void on_move_to(geom::Point const&) override;
    void handle_request_move(MirInputEvent const *input_event) override;
    void handle_request_resize(MirInputEvent const *input_event, MirResizeEdge edge) override;
    void request_horizontal_layout() override;
    void request_vertical_layout() override;
    void toggle_layout() override;

    [[nodiscard]] TilingWindowTree* get_tree() const { return tree; }
    [[nodiscard]] miral::Window& get_window() { return window; }
    void commit_changes() override;

    void restore_state(MirWindowState state) override;
    std::optional<MirWindowState> restore_state() override;
    Workspace *get_workspace() const override;
    Output *get_output() const override;
    glm::mat4 get_transform() const override;
    void set_transform(glm::mat4 transform) override;
    glm::mat4 get_workspace_transform() const override;
    glm::mat4 get_output_transform() const override;
    uint32_t animation_handle() const override;
    void animation_handle(uint32_t uint_32) override;
    bool is_focused() const override;

private:
    WindowController& node_interface;
    geom::Rectangle logical_area;
    std::optional<geom::Rectangle> next_logical_area;
    std::shared_ptr<MiracleConfig> config;
    TilingWindowTree* tree;
    miral::Window window;
    std::optional<MirWindowState> before_shown_state;
    std::optional<MirWindowState> next_state;
    NodeLayoutDirection tentative_direction = NodeLayoutDirection::none;
    std::optional<MirWindowState> restore_state_;
    glm::mat4 transform = glm::mat4(1.f);
    uint32_t animation_handle_ = 0;
};

} // miracle

#endif // MIRACLEWM_LEAF_NODE_H
