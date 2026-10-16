"""Send test pointer events to the observed Xiaocai X11 window."""

import ctypes as c
import sys
import time


class Button(c.Structure):
    _fields_ = [
        ("type", c.c_int), ("serial", c.c_ulong), ("send_event", c.c_int),
        ("display", c.c_void_p), ("window", c.c_ulong), ("root", c.c_ulong),
        ("subwindow", c.c_ulong), ("time", c.c_ulong), ("x", c.c_int),
        ("y", c.c_int), ("x_root", c.c_int), ("y_root", c.c_int),
        ("state", c.c_uint), ("button", c.c_uint), ("same_screen", c.c_int),
    ]


class Event(c.Union):
    _fields_ = [("button", Button), ("pad", c.c_long * 24)]


xlib = c.CDLL("libX11.so.6")
xlib.XOpenDisplay.argtypes = [c.c_char_p]
xlib.XOpenDisplay.restype = c.c_void_p
xlib.XDefaultRootWindow.argtypes = [c.c_void_p]
xlib.XDefaultRootWindow.restype = c.c_ulong
xlib.XQueryTree.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong),
                           c.POINTER(c.c_ulong), c.POINTER(c.POINTER(c.c_ulong)),
                           c.POINTER(c.c_uint)]
xlib.XFetchName.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_void_p)]
xlib.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long,
                           c.POINTER(Event)]
xlib.XFlush.argtypes = [c.c_void_p]
xlib.XFree.argtypes = [c.c_void_p]
xlib.XCloseDisplay.argtypes = [c.c_void_p]
display = xlib.XOpenDisplay(b":0")
if not display:
    raise RuntimeError("Cannot open board X11 display")
root = xlib.XDefaultRootWindow(display)
root_return, parent = c.c_ulong(), c.c_ulong()
children = c.POINTER(c.c_ulong)()
count = c.c_uint()
if not xlib.XQueryTree(display, root, c.byref(root_return), c.byref(parent),
                       c.byref(children), c.byref(count)):
    raise RuntimeError("Cannot inspect X11 windows")
window = None
for i in range(count.value):
    name = c.c_void_p()
    if xlib.XFetchName(display, children[i], c.byref(name)) and name.value:
        if c.string_at(name.value) == b"Xiaocai Display":
            window = children[i]
        xlib.XFree(name)
xlib.XFree(children)
if window is None:
    raise RuntimeError("Xiaocai Display window is not present")


def send(kind, x, y, state=0):
    event = Event()
    event.button = Button(kind, 0, 1, display, window, root, 0, 0,
                          x, y, x, y, state, 1, 1)
    mask = {4: 1 << 2, 5: 1 << 3, 6: 1 << 6}[kind]
    if not xlib.XSendEvent(display, window, 0, mask, c.byref(event)):
        raise RuntimeError("XSendEvent failed")
    xlib.XFlush(display)
    time.sleep(0.16)


mode, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
send(4, x, y)
if mode == "drag":
    y = int(sys.argv[4])
    send(6, x, y, 1 << 8)
send(5, x, y, 1 << 8)
xlib.XCloseDisplay(display)
