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

#ifndef MIRACLE_WM_COMPOSITOR_STATE_H
#define MIRACLE_WM_COMPOSITOR_STATE_H

#include "container.h"
#include "render_data_manager.h"

#include <algorithm>
#include <memory>
#include <mir/geometry/point.h>
#include <vector>

namespace miracle
{
enum class WindowManagerMode
{
    normal = 0,

    /// While [resizing], only the window that was selected during
    /// resize can be selected. If that window closes, resize
    /// is completed.
    resizing,

    /// While [selecting], only [Container]s selected with the multi-select
    /// keybind/mousebind can be selected or deselected.
    selecting,

    dragging,

    moving
};

class CompositorState
{
public:
    CompositorState();
    mir::geometry::Point cursor_position;
    uint32_t modifiers = 0;
    bool has_clicked_floating_window = false;

    [[nodiscard]] std::shared_ptr<Container> focused_container() const;

    /// Focuses the provided container. If [is_anonymous] is true, the container
    /// will be focused even if it does not exist in the list.
    void focus_container(std::shared_ptr<Container> const&, bool is_anonymous = false);
    void unfocus_container(std::shared_ptr<Container> const& container);
    void add(std::shared_ptr<Container> const& container);
    void remove(std::shared_ptr<Container> const& container);
    [[nodiscard]] std::shared_ptr<Container> first_floating() const;
    [[nodiscard]] std::shared_ptr<Container> first_tiling() const;
    [[nodiscard]] std::vector<std::weak_ptr<Container>> const& containers() const { return focus_order; }
    WindowManagerMode mode() const;
    void mode(WindowManagerMode);
    RenderDataManager* render_data_manager() const;

private:
    std::weak_ptr<Container> focused;
    std::vector<std::weak_ptr<Container>> focus_order;
    WindowManagerMode mode_ = WindowManagerMode::normal;
    std::unique_ptr<RenderDataManager> render_data_manager_;
};
}

#endif // MIRACLE_WM_COMPOSITOR_STATE_H
