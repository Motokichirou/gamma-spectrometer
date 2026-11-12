// theme_tokens.cpp — см. theme_tokens.h
#include "theme_tokens.h"
#include <cmath>

namespace theme {

const Tok tok = {
    0x15171a,   // bg
    0x1e2126,   // panel
    0x282c33,   // panel2
    0x3a3f47,   // border
    0xe8eaed,   // text
    0xaab1bb,   // text2
    0x6ea8ff,   // accent
    0x10141a,   // on_accent
    0x2a3a55,   // sel_bg
    0x5fd38a,   // ok
    0xf2c14e,   // warn
    0xff7b72,   // danger
    0xffa04d,   // hv
    0x3a2a1a,   // hv_bg
    0x33301f,   // warn_bg
};

static double lin(unsigned c8)
{
    double c = c8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(unsigned rgb)
{
    return 0.2126 * lin((rgb >> 16) & 0xFF) + 0.7152 * lin((rgb >> 8) & 0xFF) + 0.0722 * lin(rgb & 0xFF);
}

double contrast(unsigned a, unsigned b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

} // namespace theme
