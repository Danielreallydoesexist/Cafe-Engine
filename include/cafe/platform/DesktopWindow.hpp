#pragma once

#include "cafe/core/Types.hpp"
#include "cafe/input/Input.hpp"

#include <memory>
#include <vector>

namespace cafe {

class DesktopWindow {
public:
    DesktopWindow();
    ~DesktopWindow();
    DesktopWindow(const DesktopWindow&) = delete;
    DesktopWindow& operator=(const DesktopWindow&) = delete;

    bool open(int paneWidth, int paneHeight);
    bool poll(InputSystem& input);
    void present(const std::vector<Color>& tv, const std::vector<Color>& gamePad, int width, int height);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cafe
