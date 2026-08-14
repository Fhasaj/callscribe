#pragma once
#include <string>

namespace callscribe::util {

// A synthetic mouse, created through the kernel's uinput interface.
//
// Wayland deliberately gives ordinary clients no way to warp the pointer --
// there is no XWarpPointer equivalent, and a client cannot even ask where the
// cursor is. The one route left to an unprivileged app is to hand the kernel a
// virtual input device: events written to it travel back up through libinput
// and reach the compositor indistinguishable from a real mouse's, which is
// also what makes the compositor reset its idle timer -- the thing
// presence-tracking apps actually watch.
//
// Requires write access to /dev/uinput; open() reports the fix when it fails.
class VirtualPointer {
public:
    VirtualPointer() = default;
    ~VirtualPointer();
    VirtualPointer(const VirtualPointer&) = delete;
    VirtualPointer& operator=(const VirtualPointer&) = delete;

    // Returns false and fills `error` with a human-readable reason, including
    // the udev-rule hint for the common permission-denied case.
    bool open(std::string& error);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Relative move, in pixels. Returns false if the write failed.
    bool move(int dx, int dy);

private:
    int fd_ = -1;
};

}  // namespace callscribe::util
