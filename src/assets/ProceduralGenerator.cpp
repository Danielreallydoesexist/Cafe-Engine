#include "ProceduralGenerator.hpp"
#include "CaveGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cafe {
namespace {

using Options=std::unordered_map<std::string,std::string>;

bool optionsFrom(std::string_view text,Options& out) {
    if (text.size()>2048) return false;
    while (!text.empty()) {
        const auto end=text.find(';');
        const auto field=text.substr(0,end);
        const auto equal=field.find('=');
        if (equal==std::string_view::npos || equal==0 || equal+1==field.size() ||
            out.size()>=32) return false;
        out[std::string(field.substr(0,equal))]=std::string(field.substr(equal+1));
        if (end==std::string_view::npos) break;
        text.remove_prefix(end+1);
        if (text.empty()) return false;
    }
    return true;
}

std::string_view option(const Options& o,std::string_view key,std::string_view fallback={}) {
    auto it=o.find(std::string(key));
    return it==o.end()?fallback:std::string_view(it->second);
}

bool numeric(const Options& o,std::string_view key,double& result,double fallback,
             double low,double high) {
    result=fallback;
    auto it=o.find(std::string(key));
    if (it==o.end()) return true;
    char* end=nullptr;
    const double value=std::strtod(it->second.c_str(),&end);
    if (end==it->second.c_str() || *end || !std::isfinite(value) ||
        value<low || value>high) return false;
    result=value;
    return true;
}

bool integer(const Options& o,std::string_view key,int& result,int fallback,
             int low,int high) {
    double value=0;
    if (!numeric(o,key,value,fallback,low,high) || std::floor(value)!=value) return false;
    result=static_cast<int>(value);
    return true;
}

bool boolean(const Options& o,std::string_view key,bool& result,bool fallback) {
    result=fallback;
    const auto value=option(o,key);
    if (value.empty()) return true;
    if (value=="true" || value=="1") { result=true;return true; }
    if (value=="false" || value=="0") { result=false;return true; }
    return false;
}

bool color(const Options& o,std::string_view key,Color& result,Color fallback) {
    result=fallback;
    const auto value=option(o,key);
    if (value.empty()) return true;
    const auto parsed=Color::parse(value);
    if (!parsed) return false;
    result=*parsed;
    return true;
}

std::uint32_t hash(std::uint32_t x) {
    x^=x>>16;
    x*=0x7feb352du;
    x^=x>>15;
    x*=0x846ca68bu;
    return x^(x>>16);
}

double gradient(int x,int y,std::uint32_t seed,double dx,double dy) {
    const unsigned direction=hash(hash(static_cast<std::uint32_t>(x)^seed)^
                                  static_cast<std::uint32_t>(y))&7u;
    static constexpr double inv=0.7071067811865476;
    static constexpr double gx[8]={1,-1,0,0,inv,-inv,inv,-inv};
    static constexpr double gy[8]={0,0,1,-1,inv,inv,-inv,-inv};
    return gx[direction]*dx+gy[direction]*dy;
}

// 2-D simplex lattice noise. It uses coordinate hashing, so adjacent tiles
// sample the same field without retaining a permutation table or bitmap.
double simplex(double x,double y,std::uint32_t seed) {
    constexpr double f=0.3660254037844386,g=0.2113248654051871;
    const double skew=(x+y)*f;
    const int i=static_cast<int>(std::floor(x+skew));
    const int j=static_cast<int>(std::floor(y+skew));
    const double unskew=(i+j)*g;
    const double x0=x-(i-unskew),y0=y-(j-unskew);
    const int i1=x0>y0?1:0,j1=x0>y0?0:1;
    const double x1=x0-i1+g,y1=y0-j1+g;
    const double x2=x0-1+2*g,y2=y0-1+2*g;
    const auto corner=[&](int cx,int cy,double dx,double dy) {
        double t=0.5-dx*dx-dy*dy;
        if (t<=0) return 0.0;
        t*=t;
        return t*t*gradient(cx,cy,seed,dx,dy);
    };
    return std::clamp(70.0*(corner(i,j,x0,y0)+corner(i+i1,j+j1,x1,y1)+
                            corner(i+1,j+1,x2,y2)),-1.0,1.0);
}

double fractal(double x,double y,std::uint32_t seed,int octaves) {
    double value=0,total=0,amplitude=1;
    for (int level=0;level<octaves;++level) {
        value+=simplex(x,y,seed+static_cast<std::uint32_t>(level)*1013u)*amplitude;
        total+=amplitude;
        amplitude*=0.5;
        x*=2;y*=2;
    }
    return value/total;
}

void edgeDecoration(Asset& asset,const Options& o,std::uint32_t seed) {
    const auto style=option(o,"edge","none");
    if (style=="none") return;
    Color ink{};
    if (!color(o,"edgeColor",ink,{238,233,217,255})) return;
    int width=2,spacing=12;
    if (!integer(o,"edgeWidth",width,2,1,12) ||
        !integer(o,"edgeSpacing",spacing,12,4,48)) return;
    const int w=asset.width,h=asset.height;
    const auto original=asset.pixels;
    auto opaque=[&](int x,int y) {
        return x>=0 && y>=0 && x<w && y<h &&
               original[static_cast<std::size_t>(y*w+x)].a>=128;
    };
    auto stamp=[&](int cx,int cy,int radius) {
        for (int dy=-radius;dy<=radius;++dy) for (int dx=-radius;dx<=radius;++dx) {
            const int x=cx+dx,y=cy+dy;
            if (dx*dx+dy*dy>radius*radius || !opaque(x,y)) continue;
            asset.pixels[static_cast<std::size_t>(y*w+x)]=ink;
        }
    };
    for (int y=1;y<h-1;++y) for (int x=1;x<w-1;++x) {
        if (!opaque(x,y)) continue;
        const bool top=!opaque(x,y-1),bottom=!opaque(x,y+1);
        const bool left=!opaque(x-1,y),right=!opaque(x+1,y);
        if (!(top||bottom||left||right)) continue;
        if (style=="outline") stamp(x,y,width-1);
        else if (style=="stones") {
            const std::uint32_t random=hash(seed^hash(static_cast<std::uint32_t>(x))^
                                            static_cast<std::uint32_t>(y)*15731u);
            if (random%static_cast<unsigned>(spacing*2)==0) stamp(x,y,width+2);
        } else if (style=="grass") {
            const std::uint32_t random=hash(seed^static_cast<std::uint32_t>(x*139+y*809));
            if (random%static_cast<unsigned>(spacing)==0)
                for (int step=0;step<width*2;++step)
                    stamp(x+(right?-step:left?step:0),
                          y+(bottom?-step:top?step:0),0);
        }
    }
}

bool makeImage(std::string_view kind,const Options& o,Asset& asset) {
    int w=512,h=512,seed=0,octaves=4,cornerRadius=0;
    bool roundedCorners=false;
    double scale=96,roughness=0.16,cutoff=0.5,surface=0.42;
    double caveScale=54,caveThreshold=0.22;
    Color low{},high{},solid{};
    if (!integer(o,"width",w,512,16,1024) ||
        !integer(o,"height",h,512,16,1024) ||
        !integer(o,"seed",seed,0,0,1000000000) ||
        !integer(o,"octaves",octaves,4,1,6) ||
        !integer(o,"cornerRadius",cornerRadius,0,0,512) ||
        !boolean(o,"roundedCorners",roundedCorners,false) ||
        !numeric(o,"scale",scale,96,4,2048) ||
        !numeric(o,"roughness",roughness,0.16,0,0.5) ||
        !numeric(o,"cutoff",cutoff,0.5,0,1) ||
        !numeric(o,"surface",surface,0.42,0,1) ||
        !numeric(o,"caveScale",caveScale,54,4,2048) ||
        !numeric(o,"caveThreshold",caveThreshold,0.22,-0.9,0.9) ||
        !color(o,"low",low,{43,60,100,255}) ||
        !color(o,"high",high,{184,218,194,255}) ||
        !color(o,"solid",solid,{53,52,75,255})) return false;
    const auto shape=option(o,"shape","ellipse");
    if (kind=="mask" && shape!="ellipse" && shape!="rectangle") return false;
    if (option(o,"cornerRadius").empty())
        cornerRadius=std::max(1,std::min(w,h)/8);
    asset.type=AssetType::Image;
    asset.available=true;
    asset.width=w;asset.height=h;
    asset.pixels.resize(static_cast<std::size_t>(w)*h);
    const std::uint32_t noiseSeed=static_cast<std::uint32_t>(seed);
    // Coordinates are absolute pixel coordinates. Callers can shift the field
    // with originX/Y to generate adjacent, seamless terrain chunks.
    int originX=0,originY=0;
    if (!integer(o,"originX",originX,0,-1000000,1000000) ||
        !integer(o,"originY",originY,0,-1000000,1000000)) return false;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        const double nx=static_cast<double>(x+originX)/scale;
        const double ny=static_cast<double>(y+originY)/scale;
        const double n=fractal(nx,ny,noiseSeed,octaves);
        Color pixel{};
        if (kind=="simplex") {
            pixel=Color::lerp(low,high,(n+1)*0.5);
            if (option(o,"cutoff")!="" && (n+1)*0.5<cutoff) pixel.a=0;
        } else if (kind=="terrain") {
            const double elevation=surface+roughness*fractal(nx,0.0,noiseSeed+7919u,octaves);
            const double cave=fractal(static_cast<double>(x+originX)/caveScale,
                                      static_cast<double>(y+originY)/caveScale,
                                      noiseSeed+3571u,octaves);
            const bool below=static_cast<double>(y+originY)/h>elevation;
            pixel=(below && cave<caveThreshold)?solid:Color{0,0,0,0};
        } else {
            const double fx=(x+0.5-w*0.5)/(w*0.5);
            const double fy=(y+0.5-h*0.5)/(h*0.5);
            if (shape=="rectangle" && roundedCorners) {
                const double margin=std::min(w,h)*0.1;
                const double halfW=w*0.5-margin,halfH=h*0.5-margin;
                const double radius=std::min<double>(cornerRadius,std::min(halfW,halfH));
                const double dx=std::abs(x+0.5-w*0.5)-(halfW-radius);
                const double dy=std::abs(y+0.5-h*0.5)-(halfH-radius);
                const double signedDistance=std::hypot(std::max(dx,0.0),std::max(dy,0.0))+
                    std::min(std::max(dx,dy),0.0)-radius;
                pixel=signedDistance<=roughness*n*std::min(w,h)*0.5
                    ?solid:Color{0,0,0,0};
            } else {
                const double distance=shape=="rectangle"?std::max(std::abs(fx),std::abs(fy)):
                                       std::sqrt(fx*fx+fy*fy);
                pixel=distance<=0.8+roughness*n?solid:Color{0,0,0,0};
            }
        }
        asset.pixels[static_cast<std::size_t>(y*w+x)]=pixel;
    }
    edgeDecoration(asset,o,noiseSeed);
    return true;
}

} // namespace

bool generateProceduralImage(std::string_view source,Asset& asset) {
    if (!source.starts_with("procedural:") || source.size()>2200) return false;
    source.remove_prefix(11);
    const auto colon=source.find(':');
    const std::string_view kind=source.substr(0,colon);
    if (colon==std::string_view::npos) return false;
    source.remove_prefix(colon+1);
    if (kind=="cave") {
        const auto separator=source.find(';');
        const auto legacy=source.substr(0,separator);
        const std::string key="procedural:cave:"+std::string(legacy);
        Options o;
        if (separator!=std::string_view::npos) {
            if (!optionsFrom(source.substr(separator+1),o)) return false;
        }
        int width=768,height=768,seed=0;
        bool roundedCorners=false;
        const bool overrideCorners=!option(o,"roundedCorners").empty();
        if (!integer(o,"width",width,768,16,1024) ||
            !integer(o,"height",height,768,16,1024) ||
            !integer(o,"seed",seed,0,0,1000000000) ||
            !boolean(o,"roundedCorners",roundedCorners,false) ||
            !generateCaveImage(key,asset,width,height,
                               overrideCorners?(roundedCorners?15:0):-1)) return false;
        edgeDecoration(asset,o,static_cast<std::uint32_t>(seed));
        return true;
    }
    if (kind!="simplex" && kind!="terrain" && kind!="mask") return false;
    Options o;
    return optionsFrom(source,o) && makeImage(kind,o,asset);
}

std::vector<MaskEdgePoint> extractMaskEdgePoints(const Asset& asset,
                                                int spacing,double threshold) {
    std::vector<MaskEdgePoint> result;
    const int width=asset.width,height=asset.height;
    if (width<=0 || height<=0 ||
        asset.pixels.size()!=static_cast<std::size_t>(width)*height) return result;
    spacing=std::clamp(spacing,4,128);
    const int cellsX=(width+spacing-1)/spacing;
    const int cellsY=(height+spacing-1)/spacing;
    std::vector<std::uint8_t> occupied(static_cast<std::size_t>(cellsX)*cellsY,0);
    const auto solid=[&](int x,int y) {
        return x>=0 && y>=0 && x<width && y<height &&
            asset.pixels[static_cast<std::size_t>(y*width+x)].a/255.0>=threshold;
    };
    result.reserve(std::min<std::size_t>(2048,occupied.size()));
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        if (!solid(x,y)) continue;
        const int left=!solid(x-1,y),right=!solid(x+1,y);
        const int up=!solid(x,y-1),down=!solid(x,y+1);
        if (!(left||right||up||down)) continue;
        auto& used=occupied[static_cast<std::size_t>((y/spacing)*cellsX+x/spacing)];
        if (used) continue;
        used=1;
        double nx=right-left,ny=down-up;
        if (nx==0 && ny==0) {
            if (right) nx=1;
            else if (left) nx=-1;
            else if (down) ny=1;
            else ny=-1;
        }
        const double length=std::hypot(nx,ny);
        nx/=length;ny/=length;
        result.push_back({(x+0.5)/width,(y+0.5)/height,nx,ny});
        if (result.size()==2048) return result;
    }
    return result;
}

} // namespace cafe
