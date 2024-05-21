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

#ifndef MIRACLEWM_WINDOWTOOLSACCESSOR_H
#define MIRACLEWM_WINDOWTOOLSACCESSOR_H

#include <miral/window_manager_tools.h>

namespace miracle
{
/// Ugly hack to access the window manager tools outside of the context of the instance.
class WindowToolsAccessor
{
public:
    void set_tools(miral::WindowManagerTools const& tools);
    miral::WindowManagerTools const& get_tools();

    WindowToolsAccessor(WindowToolsAccessor& other) = delete;
    void operator=(const WindowToolsAccessor&) = delete;

    static WindowToolsAccessor& get_instance();

protected:
    WindowToolsAccessor();

private:
    miral::WindowManagerTools tools;
};

}

#endif // MIRACLEWM_WINDOWTOOLSACCESSOR_H
