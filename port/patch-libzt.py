#!/usr/bin/env python3
"""Widen select() bit sets in a libzt checkout.

Android aborts when FD_SET is used on a descriptor at or above 1024. A game
process can pass that. This gives the three socket loops a larger bit set
under its own names. It also shortens lwIP's TCP recovery timer and stops a
JNI wrapper from reporting a cleared errno as success.

The checkout is a build copy. Upstream libzt is not modified in git.
"""
import pathlib
import sys

HELPER = r'''
#ifndef HNS_WIDE_FD_MAX
#include <sys/select.h>
#include <string.h>
#define HNS_WIDE_FD_MAX 32768
struct hns_wide_fd { unsigned long word[HNS_WIDE_FD_MAX / (8 * sizeof(unsigned long))]; };
static inline int hns_wide_select(int n, struct hns_wide_fd *r, struct hns_wide_fd *w,
                                  struct hns_wide_fd *e, struct timeval *t)
{
    return select(n, (fd_set *)(void *)r, (fd_set *)(void *)w, (fd_set *)(void *)e, t);
}
static inline int hns_wide_ok(long fd) { return fd >= 0 && fd < HNS_WIDE_FD_MAX; }
#define HNS_WIDE_WORD(fd) ((unsigned long)(fd) / (8 * sizeof(unsigned long)))
#define HNS_WIDE_MASK(fd) (1UL << ((unsigned long)(fd) % (8 * sizeof(unsigned long))))
#define HNS_FD_SET(fd, set) do { if (hns_wide_ok((long)(fd))) (set)->word[HNS_WIDE_WORD(fd)] |= HNS_WIDE_MASK(fd); } while (0)
#define HNS_FD_CLR(fd, set) do { if (hns_wide_ok((long)(fd))) (set)->word[HNS_WIDE_WORD(fd)] &= ~HNS_WIDE_MASK(fd); } while (0)
#define HNS_FD_ISSET(fd, set) (hns_wide_ok((long)(fd)) && (((set)->word[HNS_WIDE_WORD(fd)] & HNS_WIDE_MASK(fd)) != 0))
#define HNS_FD_ZERO(set) memset((set), 0, sizeof(*(set)))
#endif
'''


def widen(text):
    text = text.replace("fd_set", "hns_wide_fd")
    for name in ("SET", "CLR", "ISSET", "ZERO"):
        text = text.replace("FD_%s(" % name, "HNS_FD_%s(" % name)
    text = text.replace("::select(", "hns_wide_select(")
    text = text.replace("select(FD_SETSIZE,", "hns_wide_select(natpmp.s + 1,")
    text = text.replace(" select(", " hns_wide_select(")
    text = text.replace("\tselect(", "\thns_wide_select(")
    return text


def patch_select(path):
    file = pathlib.Path(path)
    if not file.is_file():
        print("skip missing", path)
        return
    text = file.read_text(errors="replace")
    if "HNS_WIDE_FD_MAX" in text:
        return
    if "fd_set" not in text:
        print("skip no fd_set", path)
        return
    text = widen(text)
    anchor = "namespace ZeroTier {"
    if anchor not in text:
        anchor = text.splitlines()[0]
        text = HELPER + "\n" + text
    else:
        text = text.replace(anchor, HELPER + "\n" + anchor, 1)
    file.write_text(text)
    print("widened", path)


def patch_timers(path):
    file = pathlib.Path(path)
    text = file.read_text(errors="replace")
    old = "#define TCP_TMR_INTERVAL                250"
    if old in text:
        text = text.replace(old, "#define TCP_TMR_INTERVAL                25", 1)
    mss = "#define TCP_MSS                         (LWIP_MTU - 40)"
    if mss in text:
        text = text.replace(mss, "#define TCP_MSS                         1280", 1)
    file.write_text(text)
    print("timers", path)


def patch_jni(path):
    file = pathlib.Path(path)
    text = file.read_text(errors="replace")
    old = "return retval > -1 ? retval : -(zts_errno);"
    new = "return retval > -1 ? retval : -(zts_errno ? zts_errno : 11);"
    if old not in text:
        print("jni pattern absent", path)
        return
    file.write_text(text.replace(old, new))
    print("jni", path)


def main():
    root = pathlib.Path(sys.argv[1])
    patch_timers(root / "src" / "lwipopts.h")
    patch_jni(root / "src" / "bindings" / "java" / "JavaSockets.cxx")
    patch_select(root / "src" / "VirtualTap.cpp")
    patch_select(root / "ext" / "ZeroTierOne" / "osdep" / "Phy.hpp")
    patch_select(root / "ext" / "ZeroTierOne" / "osdep" / "PortMapper.cpp")


if __name__ == "__main__":
    main()
