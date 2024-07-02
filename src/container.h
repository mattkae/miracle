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

#ifndef NODE_H
#define NODE_H

#include "direction.h"
#include <functional>
#include <memory>
#include <mir/geometry/rectangle.h>
#include <miral/window.h>
#include <miral/window_manager_tools.h>
#include <vector>

namespace geom = mir::geometry;

namespace miracle
{
class MiracleConfig;
class TilingWindowTree;
class LeafContainer;
class ParentContainer;

/// A node in the tree is either a single window or a lane.
class Container : public std::enable_shared_from_this<Container>
{
public:
    explicit Container(std::shared_ptr<ParentContainer> const& parent);

    /// Commits any changes made to this node to the screen. This must
    /// be call for changes to be pushed to the scene. Additionally,
    /// it is advised that this method is only called once all changes have
    /// been made for a particular operation.
    virtual void commit_changes() = 0;

    [[nodiscard]] virtual geom::Rectangle get_logical_area() const = 0;
    virtual void set_logical_area(geom::Rectangle const&) = 0;
    virtual void constrain() = 0;
    virtual void set_parent(std::shared_ptr<ParentContainer> const&) = 0;
    virtual size_t get_min_height() const = 0;
    virtual size_t get_min_width() const = 0;
    bool is_leaf();
    bool is_lane();
    [[nodiscard]] std::weak_ptr<ParentContainer> get_parent() const;

    static std::shared_ptr<LeafContainer> as_leaf(std::shared_ptr<Container> const&);
    static std::shared_ptr<ParentContainer> as_lane(std::shared_ptr<Container> const&);

protected:
    std::weak_ptr<ParentContainer> parent;
    [[nodiscard]] std::array<bool, (size_t)Direction::MAX> get_neighbors() const;
};
}

#endif // MIRCOMPOSITOR_NODE_H
