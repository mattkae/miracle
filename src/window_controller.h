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

#ifndef MIRACLEWM_TILING_INTERFACE_H
#define MIRACLEWM_TILING_INTERFACE_H

#include <miral/window.h>
#include <miral/window_info.h>

namespace geom = mir::geometry;

namespace miracle
{
class Container;
class TilingWindowTree;
class AnimationStepResult;

/**
 * The sole interface for making changes to a window. This interface allows
 * all interactions with a window to be testable.
 */
class WindowController
{
public:
    virtual bool is_fullscreen(miral::Window const&) = 0;
    virtual void set_rectangle(miral::Window const&, geom::Rectangle const&, geom::Rectangle const&) = 0;
    virtual MirWindowState get_state(miral::Window const&) = 0;
    virtual void change_state(miral::Window const&, MirWindowState state) = 0;
    virtual void clip(miral::Window const&, geom::Rectangle const&) = 0;
    virtual void noclip(miral::Window const&) = 0;
    virtual void select_active_window(miral::Window const&) = 0;
    virtual std::shared_ptr<Container> get_container(miral::Window const&) = 0;
    virtual void raise(miral::Window const&) = 0;
    virtual void send_to_back(miral::Window const&) = 0;
    virtual void open(miral::Window const&) = 0;
    virtual void close(miral::Window const&) = 0;
    virtual void on_animation(miracle::AnimationStepResult const& result, std::shared_ptr<Container> const&) = 0;
    virtual void set_user_data(miral::Window const&, std::shared_ptr<void> const&) = 0;
    virtual void modify(miral::Window const&, miral::WindowSpecification const&) = 0;
    virtual miral::WindowInfo& info_for(miral::Window const&) = 0;
};

}

#endif
