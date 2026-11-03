#include "CaveGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace cafe {
namespace {

constexpr double logicalSide=500.0;
constexpr double tunnelHalf=76.0;

struct Route {
    std::array<Vec2,8> points{};
    int count{0};
};
struct Segment {
    Vec2 a{},b{};
    int mouth{0};
};

bool hasExit(int mask,int bit) { return (mask&bit)!=0; }

Route route(std::initializer_list<double> coordinates) {
    Route result;
    auto it=coordinates.begin();
    while (it!=coordinates.end() && result.count<8) {
        const double x=*it++;
        if (it==coordinates.end()) break;
        result.points[static_cast<std::size_t>(result.count++)]={x,*it++};
    }
    return result;
}

Vec2 pointOn(const Route& curve,double t) {
    std::array<Vec2,8> points=curve.points;
    for (int count=curve.count;count>1;--count)
        for (int i=0;i<count-1;++i) {
            points[static_cast<std::size_t>(i)].x=
                points[static_cast<std::size_t>(i)].x*(1-t)+
                points[static_cast<std::size_t>(i+1)].x*t;
            points[static_cast<std::size_t>(i)].y=
                points[static_cast<std::size_t>(i)].y*(1-t)+
                points[static_cast<std::size_t>(i+1)].y*t;
        }
    return points[0];
}

std::vector<Route> routes(double bendH,double bendV,int exits,int style) {
    const double centerX=250+bendV*38,centerY=250+bendH*38;
    if (style==1 && exits==5) {
        const double swing=(bendV<0?-1:1)*(85+std::abs(bendV)*25)*0.55;
        return {route({250,0,250,70,250+swing,150,250+swing,220,
                       250-swing,280,250-swing,350,250,430,250,500})};
    }
    if (style==1 && exits==10) {
        const double swing=(bendH<0?-1:1)*(85+std::abs(bendH)*25)*0.55;
        return {route({0,250,70,250,150,250+swing,220,250+swing,
                       280,250-swing,350,250-swing,430,250,500,250})};
    }
    if (exits==5) return {route({250,0,250,85,250+bendV*280,250,250,415,250,500})};
    if (exits==10) return {route({0,250,85,250,250,250+bendH*280,415,250,500,250})};
    if (exits==3) return {route({250,0,250,150+bendH*45,350+bendV*45,250,500,250})};
    if (exits==6) return {route({500,250,350+bendV*45,250,250,350+bendH*45,250,500})};
    if (exits==12) return {route({250,500,250,350+bendH*45,150+bendV*45,250,0,250})};
    if (exits==9) return {route({0,250,150+bendV*45,250,250,150+bendH*45,250,0})};
    std::vector<Route> result;
    if (style==1) {
        const double swingV=(bendV<0?-1:1)*(65+std::abs(bendV)*25);
        const double swingH=(bendH<0?-1:1)*(65+std::abs(bendH)*25);
        if (hasExit(exits,1)) result.push_back(route({centerX,centerY,centerX+swingV,centerY-85,250,105,250,0}));
        if (hasExit(exits,2)) result.push_back(route({centerX,centerY,centerX+85,centerY+swingH,395,250,500,250}));
        if (hasExit(exits,4)) result.push_back(route({centerX,centerY,centerX-swingV,centerY+85,250,395,250,500}));
        if (hasExit(exits,8)) result.push_back(route({centerX,centerY,centerX-85,centerY-swingH,105,250,0,250}));
    } else {
        if (hasExit(exits,1)) result.push_back(route({centerX,centerY,250,120,250,0}));
        if (hasExit(exits,2)) result.push_back(route({centerX,centerY,380,250,500,250}));
        if (hasExit(exits,4)) result.push_back(route({centerX,centerY,250,380,250,500}));
        if (hasExit(exits,8)) result.push_back(route({centerX,centerY,120,250,0,250}));
    }
    return result;
}

int routeMouth(const Route& curve,int mouths) {
    const Vec2 a=curve.points[0],b=curve.points[static_cast<std::size_t>(curve.count-1)];
    if (hasExit(mouths,1) && (a.y==0 || b.y==0)) return 1;
    if (hasExit(mouths,2) && (a.x==logicalSide || b.x==logicalSide)) return 2;
    if (hasExit(mouths,4) && (a.y==logicalSide || b.y==logicalSide)) return 4;
    if (hasExit(mouths,8) && (a.x==0 || b.x==0)) return 8;
    return 0;
}

double mouthWidth(double t) {
    const double edge=std::clamp((t-0.8)/0.2,0.0,1.0);
    return tunnelHalf+30*(1-std::sqrt(std::max(0.0,1-edge*edge)));
}

void cutCorner(Asset& asset,int corner,double pixelScaleX,double pixelScaleY) {
    const int width=asset.width,height=asset.height;
    const bool right=corner==2 || corner==4;
    const bool bottom=corner==4 || corner==8;
    const int edgeX=static_cast<int>(std::ceil(108*pixelScaleX));
    const int edgeY=static_cast<int>(std::ceil(108*pixelScaleY));
    const int x0=right?width-edgeX:0,y0=bottom?height-edgeY:0;
    const double centerX=right?392.0:108.0;
    const double centerY=bottom?392.0:108.0;
    const double feather=0.5/std::min(pixelScaleX,pixelScaleY);
    for (int y=y0;y<std::min(height,y0+edgeY);++y)
        for(int x=x0;x<std::min(width,x0+edgeX);++x) {
        const double dx=(x+0.5)/pixelScaleX-centerX;
        const double dy=(y+0.5)/pixelScaleY-centerY;
        const double distance=std::sqrt(dx*dx+dy*dy);
        const double alpha=std::clamp((108.0+feather-distance)/(2*feather),0.0,1.0);
        Color& pixel=asset.pixels[static_cast<std::size_t>(y*width+x)];
        pixel.a=static_cast<std::uint8_t>(std::min<double>(pixel.a,alpha*255));
    }
}

} // namespace

bool generateCaveImage(std::string_view source,Asset& asset,int width,int height,
                       int cornersOverride) {
    if (width<16 || width>1024 || height<16 || height>1024) return false;
    const double pixelScaleX=width/logicalSide;
    const double pixelScaleY=height/logicalSide;
    const std::string key(source);
    double bendH=0,bendV=0;
    int exits=0,mouths=0,corners=0,style=0,consumed=0;
    if (std::sscanf(key.c_str(),"procedural:cave:%lf:%lf:%d:%d:%d:%d%n",
                    &bendH,&bendV,&exits,&mouths,&corners,&style,&consumed)!=6 ||
        consumed!=static_cast<int>(key.size()) ||
        !std::isfinite(bendH) || !std::isfinite(bendV) ||
        std::abs(bendH)>1.01 || std::abs(bendV)>1.01 ||
        exits<0 || exits>15 || mouths<0 || mouths>15 ||
        corners<0 || corners>15 || (style!=0 && style!=1)) return false;
    if (cornersOverride>=0) corners=cornersOverride;
    asset.type=AssetType::Image;
    asset.available=true;
    asset.width=width;asset.height=height;
    asset.pixels.assign(static_cast<std::size_t>(width)*height,Color{0,0,0,255});
    const std::vector<Route> curves=routes(bendH,bendV,exits,style);
    std::vector<Segment> segments;
    segments.reserve(48);
    for (const Route& curve:curves) {
        const int mouth=routeMouth(curve,mouths);
        const int steps=curve.count==8?12:curve.count==5?10:6;
        Vec2 previous=pointOn(curve,0);
        for (int i=1;i<=steps;++i) {
            const Vec2 next=pointOn(curve,static_cast<double>(i)/steps);
            segments.push_back({previous,next,mouth});
            previous=next;
        }
    }
    // A cave image has many short segments but only width+height sample
    // coordinates. Reuse pixel centers and curved mouth widths across every
    // segment instead of evaluating square roots for every covered texel.
    std::vector<double> sampleX(static_cast<std::size_t>(width));
    std::vector<double> sampleY(static_cast<std::size_t>(height));
    std::vector<double> mouthEast(static_cast<std::size_t>(width));
    std::vector<double> mouthWest(static_cast<std::size_t>(width));
    std::vector<double> mouthNorth(static_cast<std::size_t>(height));
    std::vector<double> mouthSouth(static_cast<std::size_t>(height));
    for (int x=0;x<width;++x) {
        const double logicalX=(x+0.5)/pixelScaleX;
        sampleX[static_cast<std::size_t>(x)]=logicalX;
        mouthEast[static_cast<std::size_t>(x)]=mouthWidth((logicalX-200.0)/300.0);
        mouthWest[static_cast<std::size_t>(x)]=mouthWidth((300.0-logicalX)/300.0);
    }
    for (int y=0;y<height;++y) {
        const double logicalY=(y+0.5)/pixelScaleY;
        sampleY[static_cast<std::size_t>(y)]=logicalY;
        mouthNorth[static_cast<std::size_t>(y)]=mouthWidth((300.0-logicalY)/300.0);
        mouthSouth[static_cast<std::size_t>(y)]=mouthWidth((logicalY-200.0)/300.0);
    }
    const double feather=0.5/std::min(pixelScaleX,pixelScaleY);
    for (const Segment& segment:segments) {
        const double dx=segment.b.x-segment.a.x;
        const double dy=segment.b.y-segment.a.y;
        const double length=dx*dx+dy*dy;
        const double extent=segment.mouth?107.0:77.0;
        const int left=std::max(0,static_cast<int>(std::floor(
            (std::min(segment.a.x,segment.b.x)-extent)*pixelScaleX)));
        const int right=std::min(width,static_cast<int>(std::ceil(
            (std::max(segment.a.x,segment.b.x)+extent)*pixelScaleX)));
        const int top=std::max(0,static_cast<int>(std::floor(
            (std::min(segment.a.y,segment.b.y)-extent)*pixelScaleY)));
        const int bottom=std::min(height,static_cast<int>(std::ceil(
            (std::max(segment.a.y,segment.b.y)+extent)*pixelScaleY)));
        for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
            Color& pixel=asset.pixels[static_cast<std::size_t>(y*width+x)];
            if (pixel.a==0) continue;
            const double logicalX=sampleX[static_cast<std::size_t>(x)];
            const double logicalY=sampleY[static_cast<std::size_t>(y)];
            double halfWidth=tunnelHalf;
            if (segment.mouth==1) halfWidth=mouthNorth[static_cast<std::size_t>(y)];
            else if (segment.mouth==2) halfWidth=mouthEast[static_cast<std::size_t>(x)];
            else if (segment.mouth==4) halfWidth=mouthSouth[static_cast<std::size_t>(y)];
            else if (segment.mouth==8) halfWidth=mouthWest[static_cast<std::size_t>(x)];
            const double relativeX=logicalX-segment.a.x;
            const double relativeY=logicalY-segment.a.y;
            const double t=length>0?std::clamp((relativeX*dx+relativeY*dy)/length,0.0,1.0):0.0;
            const double ex=logicalX-(segment.a.x+dx*t);
            const double ey=logicalY-(segment.a.y+dy*t);
            const double d2=ex*ex+ey*ey;
            if (d2<=(halfWidth-feather)*(halfWidth-feather)) pixel.a=0;
            else if (d2<(halfWidth+feather)*(halfWidth+feather)) {
                const double alpha=std::clamp((std::sqrt(d2)-halfWidth+feather)/(2*feather),0.0,1.0);
                pixel.a=static_cast<std::uint8_t>(std::min<double>(pixel.a,alpha*255));
            }
        }
    }
    if (hasExit(corners,1)) cutCorner(asset,1,pixelScaleX,pixelScaleY);
    if (hasExit(corners,2)) cutCorner(asset,2,pixelScaleX,pixelScaleY);
    if (hasExit(corners,4)) cutCorner(asset,4,pixelScaleX,pixelScaleY);
    if (hasExit(corners,8)) cutCorner(asset,8,pixelScaleX,pixelScaleY);
    return true;
}

} // namespace cafe
