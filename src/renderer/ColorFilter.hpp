#pragma once

#include "cafe/core/Types.hpp"
#include <algorithm>
#include <cmath>

namespace cafe {

inline Color applyHueSaturation(Color source,double degrees,double saturation) {
    if (!std::isfinite(degrees) || !std::isfinite(saturation) ||
        (std::abs(degrees)<0.0001 && std::abs(saturation-1.0)<0.0001))
        return source;
    const double r=source.r/255.0,g=source.g/255.0,b=source.b/255.0;
    const double high=std::max({r,g,b}),low=std::min({r,g,b});
    const double delta=high-low;
    const double light=(high+low)*0.5;
    if (delta<1.0/255.0) return source;
    double hue=0;
    if (high==r) hue=60*std::fmod((g-b)/delta+6.0,6.0);
    else if (high==g) hue=60*((b-r)/delta+2.0);
    else hue=60*((r-g)/delta+4.0);
    hue=std::fmod(hue+degrees,360.0);
    if (hue<0) hue+=360.0;
    const double originalSaturation=delta/(1.0-std::abs(2.0*light-1.0));
    const double adjustedSaturation=std::clamp(originalSaturation*saturation,0.0,1.0);
    const double chroma=(1.0-std::abs(2.0*light-1.0))*adjustedSaturation;
    const double secondary=chroma*(1.0-std::abs(std::fmod(hue/60.0,2.0)-1.0));
    const double match=light-chroma*0.5;
    double rr=0,gg=0,bb=0;
    if (hue<60) { rr=chroma;gg=secondary; }
    else if (hue<120) { rr=secondary;gg=chroma; }
    else if (hue<180) { gg=chroma;bb=secondary; }
    else if (hue<240) { gg=secondary;bb=chroma; }
    else if (hue<300) { rr=secondary;bb=chroma; }
    else { rr=chroma;bb=secondary; }
    const auto channel=[&](double value) {
        return static_cast<std::uint8_t>(std::lround(
            std::clamp((value+match)*255.0,0.0,255.0)));
    };
    return {channel(rr),channel(gg),channel(bb),source.a};
}

} // namespace cafe
