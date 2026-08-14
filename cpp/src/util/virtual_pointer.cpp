#include "util/virtual_pointer.hpp"

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace callscribe::util {

namespace {
bool emit(int fd, unsigned short type, unsigned short code, int value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ::write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}
}  // namespace

VirtualPointer::~VirtualPointer() { close(); }

bool VirtualPointer::open(std::string& error) {
    if (fd_ >= 0) return true;

    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            error =
                "No permission to open /dev/uinput. Grant it with:  sudo usermod -aG input $USER  "
                "plus a udev rule /etc/udev/rules.d/99-uinput.rules containing  "
                "KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"  -- then log out and back in.";
        } else if (errno == ENOENT) {
            error = "/dev/uinput is missing -- load the module with:  sudo modprobe uinput";
        } else {
            error = std::string("Cannot open /dev/uinput: ") + std::strerror(errno);
        }
        return false;
    }

    // libinput only classifies a device as a pointer if it has relative X/Y
    // *and* at least one button; a button-less REL device is taken for
    // something else (a dial, a jog wheel) and never moves the cursor.
    bool ok = ioctl(fd, UI_SET_EVBIT, EV_REL) == 0 &&    //
              ioctl(fd, UI_SET_RELBIT, REL_X) == 0 &&    //
              ioctl(fd, UI_SET_RELBIT, REL_Y) == 0 &&    //
              ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 &&    //
              ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) == 0;

    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1209;  // pid.codes, the community USB vendor id
    setup.id.product = 0xca11;
    setup.id.version = 1;
    std::strncpy(setup.name, "CallScribe Virtual Pointer", sizeof(setup.name) - 1);

    ok = ok && ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
    if (!ok) {
        error = std::string("Could not create the virtual pointer: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    fd_ = fd;
    // udev has to enumerate the new device and the compositor has to open it
    // before anything written here goes anywhere -- without this pause the
    // first move or two are silently dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
}

void VirtualPointer::close() {
    if (fd_ < 0) return;
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
    fd_ = -1;
}

bool VirtualPointer::move(int dx, int dy) {
    if (fd_ < 0) return false;
    if (dx == 0 && dy == 0) return true;
    bool ok = true;
    if (dx != 0) ok = emit(fd_, EV_REL, REL_X, dx) && ok;
    if (dy != 0) ok = emit(fd_, EV_REL, REL_Y, dy) && ok;
    return emit(fd_, EV_SYN, SYN_REPORT, 0) && ok;
}

}  // namespace callscribe::util
