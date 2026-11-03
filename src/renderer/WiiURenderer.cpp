#include "cafe/renderer/WiiURenderer.hpp"
#include "cafe/renderer/Font.hpp"
#include "WiiUTextureShaderData.hpp"
#include "ColorFilter.hpp"

#ifdef CAFE_PLATFORM_WIIU

#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/clear.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <gx2r/resource.h>
#include <gx2r/surface.h>
#include <whb/gfx.h>

#include <algorithm>
#include <bit>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cafe {
namespace {

constexpr GX2RResourceFlags uploadFlags =
    GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;

constexpr GX2RResourceFlags attributeFlags = GX2R_RESOURCE_BIND_VERTEX_BUFFER |
    GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;

// 65k vertices cover the public 8192-sprite batch limit plus UI quads.
constexpr std::size_t maxDirectMeshVertices = 65536;

// Individual timing probes are useful in a diagnostic build, but hundreds of
// clock reads per frame are expensive on the Wii U's emulated PowerPC core.
using ProfileClock = std::chrono::steady_clock;
inline ProfileClock::time_point profileStamp() {
#ifdef CAFE_RENDER_PROFILE
    return ProfileClock::now();
#else
    return {};
#endif
}
inline double profileElapsedMs(ProfileClock::time_point start) {
#ifdef CAFE_RENDER_PROFILE
    return std::chrono::duration<double,std::milli>(ProfileClock::now()-start).count();
#else
    (void)start;
    return 0.0;
#endif
}

unsigned char hexNibble(char value) {
    return static_cast<unsigned char>(value <= '9' ? value - '0' : value - 'a' + 10);
}

std::vector<unsigned char> presentationShaderData() {
    constexpr std::size_t hexLength = sizeof(wiiUTextureShaderHex) - 1;
    static_assert(hexLength % 2 == 0);
    std::vector<unsigned char> bytes(hexLength / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>((hexNibble(wiiUTextureShaderHex[i * 2]) << 4) |
                                               hexNibble(wiiUTextureShaderHex[i * 2 + 1]));
    return bytes;
}

template<std::size_t N>
void createAttributeBuffer(GX2RBuffer& buffer, const float (&values)[N], std::size_t components) {
    buffer.flags = attributeFlags;
    buffer.elemSize = static_cast<std::uint32_t>(components * sizeof(float));
    buffer.elemCount = static_cast<std::uint32_t>(N / components);
    if (!GX2RCreateBuffer(&buffer)) throw std::runtime_error("Could not allocate GX2 vertex buffer");
    void* destination = GX2RLockBufferEx(&buffer, GX2R_RESOURCE_BIND_NONE);
    if (!destination) throw std::runtime_error("Could not lock GX2 vertex buffer");
    std::memcpy(destination, values, sizeof(values));
    GX2RUnlockBufferEx(&buffer, GX2R_RESOURCE_BIND_NONE);
    // Static presentation attributes are uploaded only once. Make the CPU
    // writes visible to GX2 before their first draw, just as for dynamic data.
    GX2RInvalidateBuffer(&buffer, GX2R_RESOURCE_BIND_VERTEX_BUFFER);
}

std::uint32_t rowPitchPixels(const GX2Surface& surface, int width) {
    std::uint32_t pitch = surface.pitch;
    if (pitch == 0) pitch = static_cast<std::uint32_t>(width);
    // GX2 pitch is normally texels. Only interpret it as bytes when the
    // texel interpretation would exceed the allocation returned by GX2.
    if (static_cast<std::uint64_t>(pitch) * surface.height * sizeof(Color) >
            surface.imageSize && pitch % sizeof(Color) == 0 &&
        pitch / sizeof(Color) >= static_cast<std::uint32_t>(width))
        pitch /= sizeof(Color);
    return pitch;
}

void createDynamicAttributeBuffer(GX2RBuffer& buffer, std::size_t components,
                                  std::size_t vertices) {
    buffer.flags = attributeFlags;
    buffer.elemSize = static_cast<std::uint32_t>(components * sizeof(float));
    buffer.elemCount = static_cast<std::uint32_t>(vertices);
    if (!GX2RCreateBuffer(&buffer)) throw std::runtime_error("Could not allocate GX2 mesh buffer");
}

bool uploadAttributeBuffer(GX2RBuffer& buffer, const std::vector<float>& values,
                           std::size_t components) {
    const std::size_t vertices=values.size()/components;
    if (values.empty() || vertices > maxDirectMeshVertices) return false;
    if (buffer.elemCount<vertices || !buffer.buffer) {
        std::size_t capacity=256;
        while (capacity<vertices) capacity*=2;
        if (buffer.buffer) {
            // Previous frames may still reference this buffer. Growth is rare;
            // wait before releasing it, then retain the larger capacity.
            GX2DrawDone();
            GX2RDestroyBufferEx(&buffer,GX2R_RESOURCE_BIND_NONE);
            buffer={};
        }
        createDynamicAttributeBuffer(buffer,components,capacity);
    }
    void* destination = GX2RLockBufferEx(&buffer, GX2R_RESOURCE_BIND_NONE);
    if (!destination) return false;
    std::memcpy(destination, values.data(), values.size() * sizeof(float));
    GX2RUnlockBufferEx(&buffer, GX2R_RESOURCE_BIND_NONE);
    GX2RInvalidateBuffer(&buffer, GX2R_RESOURCE_BIND_VERTEX_BUFFER);
    return true;
}

Vec3 rotateX(Vec3 value, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return {value.x, value.y*c-value.z*s, value.y*s+value.z*c};
}

Vec3 rotateY(Vec3 value, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return {value.x*c+value.z*s, value.y, -value.x*s+value.z*c};
}

Vec3 rotateZ(Vec3 value, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return {value.x*c-value.y*s, value.x*s+value.y*c, value.z};
}

Vec3 rotateObject(Vec3 value, Vec3 degrees) {
    constexpr double toRadians = 3.14159265358979323846 / 180.0;
    value = rotateX(value, degrees.x * toRadians);
    value = rotateY(value, degrees.y * toRadians);
    return rotateZ(value, degrees.z * toRadians);
}

Vec3 toCameraSpace(Vec3 value, Vec3 cameraPosition, Vec3 cameraRotation) {
    constexpr double toRadians = 3.14159265358979323846 / 180.0;
    value = value - cameraPosition;
    // Reverse both the angles and the operation order to invert the camera.
    value = rotateZ(value, -cameraRotation.z * toRadians);
    value = rotateY(value, -cameraRotation.y * toRadians);
    return rotateX(value, -cameraRotation.x * toRadians);
}

Color meshColor(const ComputedStyle& style) {
    if (!style.fill) return {95, 185, 255, 255};
    if (const auto* color = std::get_if<Color>(&*style.fill)) return *color;
    if (const auto* linear = std::get_if<LinearGradient>(&*style.fill)) {
        if (linear->stops.empty()) return {95, 185, 255, 255};
        if (linear->stops.size() == 1) return linear->stops.front().color;
        return Color::lerp(linear->stops.front().color, linear->stops.back().color, 0.5);
    }
    if (const auto* radial = std::get_if<RadialGradient>(&*style.fill)) {
        if (!radial->stops.empty()) return radial->stops.front().color;
    }
    if (const auto* pattern = std::get_if<Pattern>(&*style.fill)) return pattern->color;
    return {95, 185, 255, 255};
}

Color modulateParticleColor(Color paint, Color lifetime) {
    paint.r=static_cast<std::uint8_t>(paint.r*lifetime.r/255);
    paint.g=static_cast<std::uint8_t>(paint.g*lifetime.g/255);
    paint.b=static_cast<std::uint8_t>(paint.b*lifetime.b/255);
    paint.a=static_cast<std::uint8_t>(paint.a*lifetime.a/255);
    return paint;
}

std::pair<Color,Color> particleRadialColors(const ComputedStyle& style,Color lifetime) {
    Color center{255,255,255,255},edge{255,255,255,255};
    if(style.fill) {
        if(const auto* radial=std::get_if<RadialGradient>(&*style.fill);radial&&!radial->stops.empty()) {
            center=radial->stops.front().color;
            edge=radial->stops.back().color;
        } else center=edge=meshColor(style);
    }
    return {modulateParticleColor(center,lifetime),modulateParticleColor(edge,lifetime)};
}

std::vector<Vec3> clipDepth(std::vector<Vec3> polygon, double plane, bool keepGreater) {
    if (polygon.empty()) return {};
    std::vector<Vec3> output;
    Vec3 previous = polygon.back();
    bool previousInside = keepGreater ? previous.z >= plane : previous.z <= plane;
    for (Vec3 current : polygon) {
        const bool currentInside = keepGreater ? current.z >= plane : current.z <= plane;
        if (currentInside != previousInside) {
            const double amount = (plane - previous.z) / (current.z - previous.z);
            output.push_back(previous + (current - previous) * amount);
        }
        if (currentInside) output.push_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return output;
}

template<typename Key>
bool appendByte(Key& key,std::uint8_t value) {
    if (key.size>=key.data.size()) return false;
    key.data[key.size++]=value;
    return true;
}

template<typename Key>
bool appendNumber(Key& key,double value) {
    if (key.data.size()-key.size<sizeof(std::uint64_t)) return false;
    const auto bits=std::bit_cast<std::uint64_t>(value);
    for (unsigned i=0;i<8;++i)
        key.data[key.size++]=static_cast<std::uint8_t>(bits>>(i*8));
    return true;
}

template<typename Key>
bool appendColor(Key& key,Color value) {
    return appendByte(key,value.r) && appendByte(key,value.g) &&
           appendByte(key,value.b) && appendByte(key,value.a);
}

template<typename Key>
bool appendText(Key& key,std::string_view value) {
    if (!appendNumber(key,static_cast<double>(value.size())) ||
        value.size()>key.data.size()-key.size) return false;
    for (char c:value) key.data[key.size++]=static_cast<std::uint8_t>(c);
    return true;
}

template<typename Key>
bool appendPaint(Key& key,const Paint& paint) {
    if (!appendByte(key,static_cast<std::uint8_t>(paint.index()))) return false;
    if (const auto* color=std::get_if<Color>(&paint)) return appendColor(key,*color);
    else if (const auto* linear=std::get_if<LinearGradient>(&paint)) {
        if (!appendNumber(key,linear->angle) ||
            !appendNumber(key,static_cast<double>(linear->stops.size()))) return false;
        for (const ColorStop& stop:linear->stops) {
            if (!appendNumber(key,stop.position) ||
                !appendColor(key,stop.color)) return false;
        }
    } else if (const auto* radial=std::get_if<RadialGradient>(&paint)) {
        if (!appendNumber(key,static_cast<double>(radial->stops.size()))) return false;
        for (const ColorStop& stop:radial->stops) {
            if (!appendNumber(key,stop.position) ||
                !appendColor(key,stop.color)) return false;
        }
    } else return false; // Patterns depend on absolute screen coordinates.
    return true;
}

bool decodeUiText(std::string_view source,std::vector<int>& codepoints) {
    codepoints.clear();
    codepoints.reserve(source.size());
    for (std::size_t i=0;i<source.size();) {
        const unsigned char first=static_cast<unsigned char>(source[i]);
        if (first<127) {
            if (first<32 && first!='\n' && first!='\t') return false;
            codepoints.push_back(first);
            ++i;
            continue;
        }
        if (first==0xe2 && i+2<source.size() &&
            static_cast<unsigned char>(source[i+1])==0x80) {
            const unsigned char last=static_cast<unsigned char>(source[i+2]);
            if (last==0xa2 || last==0x94) {
                codepoints.push_back(last==0xa2?0x2022:0x2014);
                i+=3;
                continue;
            }
        }
        return false;
    }
    return true;
}

} // namespace

void WiiURenderer::createUploadSurface(UploadSurface& upload, int width, int height, bool flipY) {
    if (startupTrace_) {
        const std::string stage="surface-calc "+std::to_string(width)+"x"+
            std::to_string(height);
        startupTrace_(stage.c_str());
    }
    GX2Texture& texture = upload.texture;
    std::memset(&texture, 0, sizeof(texture));
    GX2Surface& surface = texture.surface;
    surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surface.width = static_cast<std::uint32_t>(width);
    surface.height = static_cast<std::uint32_t>(height);
    surface.depth = 1;
    surface.mipLevels = 1;
    surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    surface.aa = GX2_AA_MODE1X;
    surface.use = GX2_SURFACE_USE_TEXTURE;
    surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&surface);
    if (startupTrace_) {
        const std::string stage="surface-alloc bytes="+
            std::to_string(surface.imageSize)+" align="+
            std::to_string(surface.alignment)+" pitch="+
            std::to_string(surface.pitch);
        startupTrace_(stage.c_str());
    }

    if (!GX2RCreateSurface(&surface, uploadFlags))
        throw std::runtime_error("Could not allocate GX2 frame upload surface");
    if (startupTrace_) startupTrace_("surface-allocated");

    texture.viewNumMips = 1;
    texture.viewNumSlices = 1;
    texture.compMap = 0x00010203;
    GX2InitTextureRegs(&texture);

    upload.width = width;
    upload.height = height;
    upload.mapX.resize(static_cast<std::size_t>(width));
    upload.mapY.resize(static_cast<std::size_t>(height));
    for (int x = 0; x < width; ++x)
        upload.mapX[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(
            std::min(width_ - 1, x * width_ / width));
    for (int y = 0; y < height; ++y) {
        const int mappedY = flipY ? height - 1 - y : y;
        upload.mapY[static_cast<std::size_t>(y)] = static_cast<std::uint16_t>(
            std::min(height_ - 1, mappedY * height_ / height));
    }
}

void WiiURenderer::destroyUploadSurface(UploadSurface& upload) {
    if (upload.texture.surface.image)
        GX2RDestroySurfaceEx(&upload.texture.surface, GX2R_RESOURCE_BIND_NONE);
    upload = {};
}

void WiiURenderer::createPresentationShader() {
    auto shaderData = presentationShaderData();
    if (!WHBGfxLoadGFDShaderGroup(&shader_, 0, shaderData.data()))
        throw std::runtime_error("Could not load GX2 presentation shader");
    shaderReady_ = true;
    if (!WHBGfxInitShaderAttribute(&shader_, "aPosition", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32) ||
        !WHBGfxInitShaderAttribute(&shader_, "aTexCoord", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
        !WHBGfxInitShaderAttribute(&shader_, "aColour", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32) ||
        !WHBGfxInitFetchShader(&shader_))
        throw std::runtime_error("Could not initialize GX2 presentation shader");

    constexpr float textureCoordinates[] = {
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
    };
    constexpr float colors[] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    };
    createAttributeBuffer(textureCoordinateBuffer_, textureCoordinates, 2);
    createAttributeBuffer(colorBuffer_, colors, 4);
    GX2InitSampler(&sampler_, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
}

void WiiURenderer::createPositionBuffer(GX2RBuffer& buffer, const Rect& viewport,
                                        int outputWidth, int outputHeight) {
    const float left = static_cast<float>(viewport.x / outputWidth * 2.0 - 1.0);
    const float right = static_cast<float>((viewport.x + viewport.width) / outputWidth * 2.0 - 1.0);
    const float top = static_cast<float>(1.0 - viewport.y / outputHeight * 2.0);
    const float bottom = static_cast<float>(1.0 - (viewport.y + viewport.height) / outputHeight * 2.0);
    const float positions[] = {
        left, bottom, 0.0f, right, bottom, 0.0f,
        right, top, 0.0f, left, top, 0.0f,
    };
    if (!buffer.buffer) {
        createAttributeBuffer(buffer, positions, 3);
    } else {
        // A mode change is applied between frames after GX2DrawDone, so this
        // four-vertex presentation buffer can be updated without reallocating.
        void* destination=GX2RLockBufferEx(&buffer,GX2R_RESOURCE_BIND_NONE);
        if (!destination) throw std::runtime_error("Could not update GX2 viewport buffer");
        std::memcpy(destination,positions,sizeof(positions));
        GX2RUnlockBufferEx(&buffer,GX2R_RESOURCE_BIND_NONE);
        GX2RInvalidateBuffer(&buffer,GX2R_RESOURCE_BIND_VERTEX_BUFFER);
    }
}

void WiiURenderer::destroyPresentationShader() {
    if (tvPositionBuffer_.buffer)
        GX2RDestroyBufferEx(&tvPositionBuffer_, GX2R_RESOURCE_BIND_NONE);
    if (gamePadPositionBuffer_.buffer)
        GX2RDestroyBufferEx(&gamePadPositionBuffer_, GX2R_RESOURCE_BIND_NONE);
    if (textureCoordinateBuffer_.buffer)
        GX2RDestroyBufferEx(&textureCoordinateBuffer_, GX2R_RESOURCE_BIND_NONE);
    if (colorBuffer_.buffer) GX2RDestroyBufferEx(&colorBuffer_, GX2R_RESOURCE_BIND_NONE);
    tvPositionBuffer_ = {};
    gamePadPositionBuffer_ = {};
    textureCoordinateBuffer_ = {};
    colorBuffer_ = {};
    if (shaderReady_) WHBGfxFreeShaderGroup(&shader_);
    shader_ = {};
    shaderReady_ = false;
}

void WiiURenderer::destroyMeshBuffers(MeshBuffers& buffers) {
    if (buffers.positions.buffer)
        GX2RDestroyBufferEx(&buffers.positions, GX2R_RESOURCE_BIND_NONE);
    if (buffers.textureCoordinates.buffer)
        GX2RDestroyBufferEx(&buffers.textureCoordinates, GX2R_RESOURCE_BIND_NONE);
    if (buffers.colors.buffer)
        GX2RDestroyBufferEx(&buffers.colors, GX2R_RESOURCE_BIND_NONE);
    buffers = {};
}

WiiURenderer::WiiURenderer(int logicalWidth, int logicalHeight)
    : SoftwareRenderer(logicalWidth, logicalHeight, {}, 1.0) {
    if (!WHBGfxInit()) throw std::runtime_error("Could not initialize GX2");
    graphicsReady_ = true;

    try {
        createPresentationShader();
        // 2D geometry buffers are allocated on first use, sized to actual
        // frame geometry. Preallocating eight 65k-vertex sets here consumed
        // about 19 MiB before the Wii U could show its first game frame.
        createUploadSurface(whiteTexture_, 4, 4, false);
        {
            GX2Surface& surface = whiteTexture_.texture.surface;
            void* destination = GX2RLockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE);
            if (!destination) throw std::runtime_error("Could not initialize GX2 white texture");
            // GX2 aligns each row beyond the declared width. Populate the
            // entire allocation so filtered sampling of the white source
            // cannot pick up uninitialized transparent padding on hardware.
            std::memset(destination, 0xff, surface.imageSize);
            GX2RUnlockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE);
            GX2Invalidate(static_cast<GX2InvalidateMode>(GX2_INVALIDATE_MODE_CPU |
                                                        GX2_INVALIDATE_MODE_TEXTURE),
                          surface.image, surface.imageSize);
        }
        const GX2ColorBuffer* tvBuffer = WHBGfxGetTVColourBuffer();
        const GX2ColorBuffer* gamePadBuffer = WHBGfxGetDRCColourBuffer();
        if (!tvBuffer || !gamePadBuffer ||
            !tvBuffer->surface.image || !gamePadBuffer->surface.image)
            throw std::runtime_error("WHB did not provide TV/GamePad colour buffers");
        tvOutputWidth_ = static_cast<int>(tvBuffer->surface.width);
        tvOutputHeight_ = static_cast<int>(tvBuffer->surface.height);
        gamePadOutputWidth_ = static_cast<int>(gamePadBuffer->surface.width);
        gamePadOutputHeight_ = static_cast<int>(gamePadBuffer->surface.height);
        if (tvOutputWidth_ <= 0 || tvOutputHeight_ <= 0 ||
            gamePadOutputWidth_ <= 0 || gamePadOutputHeight_ <= 0 ||
            tvOutputWidth_ > 4096 || tvOutputHeight_ > 4096 ||
            gamePadOutputWidth_ > 4096 || gamePadOutputHeight_ > 4096)
            throw std::runtime_error("WHB returned invalid TV/GamePad dimensions");

        updateViewports();
        createPositionBuffer(tvPositionBuffer_,tvViewport_,
                             tvOutputWidth_,tvOutputHeight_);
        createPositionBuffer(gamePadPositionBuffer_,gamePadViewport_,
                             gamePadOutputWidth_,gamePadOutputHeight_);
        // Keep UI at the full logical resolution. Unchanged 2D frames remain
        // resident in these textures, while animated 3D and particles are
        // submitted separately as native-resolution GX2 geometry.
        createUploadSurface(tvUpload_, width_, height_, false);
        createUploadSurface(gamePadUpload_, width_, height_, false);
    } catch (...) {
        destroyUploadSurface(tvUpload_);
        destroyUploadSurface(gamePadUpload_);
        destroyUploadSurface(whiteTexture_);
        destroyMeshBuffers(tvMeshBuffers_);
        destroyMeshBuffers(gamePadMeshBuffers_);
        for (MeshBuffers& buffers:tv2DBuffers_) destroyMeshBuffers(buffers);
        for (MeshBuffers& buffers:gamePad2DBuffers_) destroyMeshBuffers(buffers);
        destroyPresentationShader();
        WHBGfxShutdown();
        graphicsReady_ = false;
        throw;
    }
}

void WiiURenderer::setStartupTrace(std::function<void(const char*)> trace) {
    startupTrace_=std::move(trace);
}

void WiiURenderer::beginDisplay(Display display, Color clear) {
    if (display == Display::Tv) { frameStats_ = {}; frameStart_ = std::chrono::steady_clock::now(); }
    if (gpu2DEnabled_) {
        currentDisplay_=display;
        currentPixels_=display==Display::GamePad?&gamePadPixels_:&tvPixels_;
    } else {
        SoftwareRenderer::beginDisplay(display,clear);
    }
    if (gpu2DEnabled_) reset2DFrame(current2D(), clear);
    MeshBatch& batch = display == Display::GamePad ? gamePadMeshes_ : tvMeshes_;
    batch.positions.clear();
    batch.textureCoordinates.clear();
    batch.colors.clear();
    batch.ranges.clear();
    if (display == Display::GamePad) {
        gamePadFrameValid_ = false;
        gamePadPixelsDirty_ = true;
    } else {
        tvFrameValid_ = false;
        tvPixelsDirty_ = true;
    }
}

void WiiURenderer::setMirrorDisplays(bool mirror) {
    // Reusing TV vertices on a 4:3 TV and a 16:9 GamePad would stretch them.
    if (expandedViewport_ &&
        std::abs(visibleBounds(Display::Tv).width-
                 visibleBounds(Display::GamePad).width)>1.0) mirror=false;
    if (mirrorDisplays_ == mirror) return;
    mirrorDisplays_ = mirror;
    // A private GamePad frame must be rebuilt when mirroring ends.
    gamePadFrameValid_ = false;
    gamePadPixelsDirty_ = true;
}

void WiiURenderer::setExpandedViewport(bool expanded) {
    if (expandedViewport_ == expanded) return;
    // The software fallback uses separate TV and GamePad presentation quads.
    // A project can choose expanded viewports after the renderer is created.
    expandedViewport_ = expanded;
    updateViewports();
    if (tvPositionBuffer_.buffer)
        createPositionBuffer(tvPositionBuffer_,tvViewport_,
                             tvOutputWidth_,tvOutputHeight_);
    if (gamePadPositionBuffer_.buffer)
        createPositionBuffer(gamePadPositionBuffer_,gamePadViewport_,
                             gamePadOutputWidth_,gamePadOutputHeight_);
    tvFrameValid_=gamePadFrameValid_=false;
}

void WiiURenderer::updateViewports() {
    // Expand the visible logical width at a uniform scale. This keeps eggs,
    // text, and hit targets round while revealing more of the scene at each
    // side of a wide TV or GamePad.
    tvViewport_ = expandedViewport_
        ? Rect{0,0,static_cast<double>(tvOutputWidth_),static_cast<double>(tvOutputHeight_)}
        : aspectFitViewport(logicalWidth_,logicalHeight_,tvOutputWidth_,tvOutputHeight_);
    gamePadViewport_ = expandedViewport_
        ? Rect{0,0,static_cast<double>(gamePadOutputWidth_),
               static_cast<double>(gamePadOutputHeight_)}
        : aspectFitViewport(logicalWidth_,logicalHeight_,gamePadOutputWidth_,
                            gamePadOutputHeight_);
    tvVisibleBounds_=expandedViewport_
        ? expandedLogicalBounds(logicalWidth_,logicalHeight_,
                                tvViewport_.width,tvViewport_.height)
        : SoftwareRenderer::visibleBounds(Display::Tv);
    gamePadVisibleBounds_=expandedViewport_
        ? expandedLogicalBounds(logicalWidth_,logicalHeight_,
                                gamePadViewport_.width,gamePadViewport_.height)
        : SoftwareRenderer::visibleBounds(Display::GamePad);
    uiTextureScale_ = std::max({1.0,tvViewport_.height/logicalHeight_,
                                gamePadViewport_.height/logicalHeight_});
}

void WiiURenderer::clearUiTextureCaches() {
    // Cached draw commands contain pointers into these atlases. Discard them
    // before releasing GPU surfaces, after the preceding frame has completed.
    reset2DFrame(tv2D_,{0,0,0,255});
    reset2DFrame(gamePad2D_,{0,0,0,255});
    auto releaseGpuTexture=[this](const Asset* asset) {
        const auto found=imageTextures_.find(asset);
        if (found==imageTextures_.end()) return;
        const std::size_t bytes=static_cast<std::size_t>(found->second->width)*
                                found->second->height*sizeof(Color);
        destroyUploadSurface(*found->second);
        imageTextures_.erase(found);
        imageTextureRevisions_.erase(asset);
        imageTextureBytes_-=bytes;
    };
    for (const auto& [key,asset]:shapeTextures_) releaseGpuTexture(asset.get());
    for (const auto& [key,atlas]:fontAtlases_) releaseGpuTexture(&atlas->asset);
    shapeTextures_.clear();
    fontAtlases_.clear();
    shapeTextureBytes_=0;
}

bool WiiURenderer::refreshOutputGeometry() {
    const GX2ColorBuffer* tv=WHBGfxGetTVColourBuffer();
    const GX2ColorBuffer* pad=WHBGfxGetDRCColourBuffer();
    if (!tv || !pad || !tv->surface.image || !pad->surface.image) return false;
    const int tvWidth=static_cast<int>(tv->surface.width);
    const int tvHeight=static_cast<int>(tv->surface.height);
    const int padWidth=static_cast<int>(pad->surface.width);
    const int padHeight=static_cast<int>(pad->surface.height);
    if (tvWidth<=0 || tvHeight<=0 || padWidth<=0 || padHeight<=0 ||
        tvWidth>4096 || tvHeight>4096 || padWidth>4096 || padHeight>4096)
        return false;
    if (tvWidth==tvOutputWidth_ && tvHeight==tvOutputHeight_ &&
        padWidth==gamePadOutputWidth_ && padHeight==gamePadOutputHeight_)
        return false;

    // WHBGfxFinishRender normally waits for completion, but do not rewrite
    // vertex buffers or free cached textures while GX2 may still sample them.
    GX2DrawDone();
    const double oldScale=uiTextureScale_;
    tvOutputWidth_=tvWidth;tvOutputHeight_=tvHeight;
    gamePadOutputWidth_=padWidth;gamePadOutputHeight_=padHeight;
    updateViewports();
    createPositionBuffer(tvPositionBuffer_,tvViewport_,tvWidth,tvHeight);
    createPositionBuffer(gamePadPositionBuffer_,gamePadViewport_,padWidth,padHeight);
    if (std::abs(oldScale-uiTextureScale_)>0.001) clearUiTextureCaches();
    tvFrameValid_=gamePadFrameValid_=false;
    tvPixelsDirty_=gamePadPixelsDirty_=true;
    return true;
}

Rect WiiURenderer::visibleBounds(Display display) const {
    return display==Display::GamePad?gamePadVisibleBounds_:tvVisibleBounds_;
}

void WiiURenderer::setGpu2DEnabled(bool enabled) {
    if (gpu2DEnabled_==enabled) return;
    gpu2DEnabled_=enabled;
    tvFrameValid_=gamePadFrameValid_=false;
    tvPixelsDirty_=gamePadPixelsDirty_=true;
}

bool WiiURenderer::beginDisplayFrame(Display display, Color clear,
                                     std::size_t signature) {
    if (startupTrace_)
        startupTrace_(display==Display::Tv?"render-tv-start":"render-gamepad-start");
    if (display == Display::Tv) { frameStats_ = {}; frameStart_ = std::chrono::steady_clock::now(); }
    MeshBatch& batch = display == Display::GamePad ? gamePadMeshes_ : tvMeshes_;
    batch.positions.clear();
    batch.textureCoordinates.clear();
    batch.colors.clear();
    batch.ranges.clear();
    currentDisplay_ = display;
    if (display == Display::GamePad && mirrorDisplays_) return false;
    currentPixels_ = display == Display::GamePad ? &gamePadPixels_ : &tvPixels_;
    bool& valid = display == Display::GamePad ? gamePadFrameValid_ : tvFrameValid_;
    bool& dirty = display == Display::GamePad ? gamePadPixelsDirty_ : tvPixelsDirty_;
    std::size_t& previous = display == Display::GamePad
        ? gamePadFrameSignature_ : tvFrameSignature_;
    if (valid && previous == signature) return false;
    if (gpu2DEnabled_) {
        currentDisplay_=display;
        currentPixels_=display==Display::GamePad?&gamePadPixels_:&tvPixels_;
    } else {
        SoftwareRenderer::beginDisplay(display,clear);
    }
    if (gpu2DEnabled_) reset2DFrame(current2D(), clear);
    previous = signature;
    valid = true;
    dirty = true;
    if (startupTrace_)
        startupTrace_(display==Display::Tv?"render-tv-ready":"render-gamepad-ready");
    return true;
}

WiiURenderer::Frame2D& WiiURenderer::current2D() {
    return currentDisplay_ == Display::GamePad ? gamePad2D_ : tv2D_;
}

void WiiURenderer::ensureSoftwareTarget() {
    Frame2D& frame=current2D();
    if (frame.softwareInitialized) return;
    SoftwareRenderer::beginDisplay(currentDisplay_,{0,0,0,0});
    frame.softwareInitialized=true;
}

void WiiURenderer::reset2DFrame(Frame2D& frame, Color clear) {
    frame.positions.clear();
    frame.textureCoordinates.clear();
    frame.colors.clear();
    frame.commands.clear();
    frame.layers.clear();
    frame.clear = clear;
    frame.softwarePending = false;
    frame.softwareInitialized = false;
    frame.revision = next2DRevision_++;
    frame.spriteCount = 0;
    frame.scanLeft=width_;frame.scanTop=height_;
    frame.scanRight=0;frame.scanBottom=0;
}

void WiiURenderer::markSoftwareBounds(Frame2D& frame,const Node& node,
                                      const ComputedStyle& style,bool text) {
    Rect bounds=node.bounds();
    double padding=8.0;
    for (const Shadow& shadow:style.shadows)
        padding=std::max(padding,std::abs(shadow.x)+std::abs(shadow.y)+
                                  shadow.blur*3.0+8.0);
    if (style.sawtooth) padding=std::max(padding,style.sawtooth->depth+8.0);
    if (text) {
        std::size_t longest=0,current=0,lines=1;
        for (char value:node.text) {
            if (value=='\n') { longest=std::max(longest,current);current=0;++lines; }
            else ++current;
        }
        longest=std::max(longest,current);
        bounds.width=std::max(bounds.width,static_cast<double>(longest)*style.fontSize*1.6);
        bounds.height=std::max(bounds.height,static_cast<double>(lines)*style.fontSize*1.6);
        padding=std::max(padding,style.fontSize*1.5+8.0);
    }
    const int left=std::clamp(static_cast<int>(std::floor(bounds.x-padding)),0,width_);
    const int top=std::clamp(static_cast<int>(std::floor(bounds.y-padding)),0,height_);
    const int right=std::clamp(static_cast<int>(std::ceil(bounds.x+bounds.width+padding)),0,width_);
    const int bottom=std::clamp(static_cast<int>(std::ceil(bounds.y+bounds.height+padding)),0,height_);
    frame.scanLeft=std::min(frame.scanLeft,left);
    frame.scanTop=std::min(frame.scanTop,top);
    frame.scanRight=std::max(frame.scanRight,right);
    frame.scanBottom=std::max(frame.scanBottom,bottom);
}

void WiiURenderer::queueQuad(Frame2D& frame, Rect rect, double rotation,
                             Color tint, double opacity, BlendMode blend,
                             const Asset* asset, std::size_t layer,
                             bool softwareLayer, double uMax, double vMax,
                             double uMin, double vMin) {
    const Rect visible=visibleBounds(currentDisplay_);
    if (expandedViewport_ && !softwareLayer && std::abs(rect.x)<0.001 &&
        std::abs(rect.y)<0.001 &&
        std::abs(rect.width-logicalWidth_)<0.001 &&
        std::abs(rect.height-logicalHeight_)<0.001)
        rect=visible; // Fullscreen gradients and modal scrims cover the output.
    if (rect.width <= 0 || rect.height <= 0 || opacity <= 0 ||
        frame.positions.size() / 3 + 6 > maxDirectMeshVertices) return;
    const double angle = rotation * 3.14159265358979323846 / 180.0;
    const double sine = std::sin(angle), cosine = std::cos(angle);
    const double centerX = rect.x + rect.width * 0.5;
    const double centerY = rect.y + rect.height * 0.5;
    const double halfX = rect.width * 0.5, halfY = rect.height * 0.5;
    const double localX[4]{-halfX, halfX, halfX, -halfX};
    const double localY[4]{-halfY, -halfY, halfY, halfY};
    const float u[4]{static_cast<float>(uMin), static_cast<float>(uMax),
                     static_cast<float>(uMax), static_cast<float>(uMin)};
    const float v[4]{static_cast<float>(vMin), static_cast<float>(vMin),
                     static_cast<float>(vMax), static_cast<float>(vMax)};
    const int indices[6]{0,1,2,0,2,3};
    const std::uint32_t first = static_cast<std::uint32_t>(frame.positions.size() / 3);
    const float alpha = static_cast<float>(std::clamp(opacity, 0.0, 1.0) * tint.a / 255.0);
    const float rgbScale = blend == BlendMode::Multiply || blend == BlendMode::Screen
        ? alpha : 1.0f;
    const float red = tint.r / 255.0f * rgbScale;
    const float green = tint.g / 255.0f * rgbScale;
    const float blue = tint.b / 255.0f * rgbScale;
    for (int index : indices) {
        const double x = centerX + localX[index] * cosine - localY[index] * sine;
        const double y = centerY + localX[index] * sine + localY[index] * cosine;
        frame.positions.insert(frame.positions.end(), {
            static_cast<float>((x-visible.x) / visible.width * 2.0 - 1.0),
            static_cast<float>(1.0 - (y-visible.y) / visible.height * 2.0), 0.0f});
        frame.textureCoordinates.insert(frame.textureCoordinates.end(), {u[index],v[index]});
        frame.colors.insert(frame.colors.end(), {red,green,blue,alpha});
    }
    if (!softwareLayer && !frame.commands.empty()) {
        Draw2DCommand& previous = frame.commands.back();
        if (!previous.softwareLayer && previous.asset == asset &&
            previous.blend == blend && previous.first + previous.count == first) {
            previous.count += 6;
            return;
        }
    }
    frame.commands.push_back({first,6,asset,layer,softwareLayer,blend});
}

void WiiURenderer::flushSoftwareLayer(Frame2D& frame) {
    if (!frame.softwarePending) return;
    const auto start = profileStamp();
    int left = width_, top = height_, right = 0, bottom = 0;
    for (int y = frame.scanTop; y < frame.scanBottom; ++y) {
        const Color* row = currentPixels_->data() + static_cast<std::size_t>(y * width_);
        for (int x = frame.scanLeft; x < frame.scanRight; ++x) if (row[x].a) {
            left = std::min(left,x); top = std::min(top,y);
            right = std::max(right,x+1); bottom = std::max(bottom,y+1);
        }
    }
    if (right > left && bottom > top) {
        SoftwareLayer layer;
        layer.width = right-left;
        layer.height = bottom-top;
        layer.pixels.resize(static_cast<std::size_t>(layer.width * layer.height));
        for (int y = top; y < bottom; ++y)
            std::memcpy(layer.pixels.data() + static_cast<std::size_t>((y-top)*layer.width),
                        currentPixels_->data() + static_cast<std::size_t>(y*width_+left),
                        static_cast<std::size_t>(layer.width) * sizeof(Color));
        const std::size_t index = frame.layers.size();
        frame.layers.push_back(std::move(layer));
        queueQuad(frame, {static_cast<double>(left),static_cast<double>(top),
                          static_cast<double>(right-left),static_cast<double>(bottom-top)},
                  0, {255,255,255,255}, 1, BlendMode::Normal, nullptr, index, true);
    }
    for (int y=frame.scanTop;y<frame.scanBottom;++y) {
        Color* row=currentPixels_->data()+static_cast<std::size_t>(y*width_);
        std::fill(row+frame.scanLeft,row+frame.scanRight,Color{0,0,0,0});
    }
    frame.softwarePending = false;
    frame.scanLeft=width_;frame.scanTop=height_;
    frame.scanRight=0;frame.scanBottom=0;
    frameStats_.geometryMs += profileElapsedMs(start);
}

bool WiiURenderer::uploadPixels(UploadSurface& upload, const Color* pixels,
                                int width, int height) {
    if (!pixels || width <= 0 || height <= 0 || width > upload.width ||
        height > upload.height) return false;
    GX2Surface& surface = upload.texture.surface;
    if (startupTrace_) startupTrace_("pixels-lock-begin");
    auto* destination = static_cast<std::uint8_t*>(
        GX2RLockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE));
    if (!destination) return false;
    if (startupTrace_) startupTrace_("pixels-lock-done");
    const std::size_t pitch = static_cast<std::size_t>(rowPitchPixels(surface, upload.width)) * sizeof(Color);
    if (pitch < static_cast<std::size_t>(width) * sizeof(Color) ||
        pitch * static_cast<std::size_t>(upload.height) > surface.imageSize) {
        GX2RUnlockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE);
        return false;
    }
    // A pooled surface may be wider than this layer. Clear the unused texels
    // so linear sampling at its edge cannot reveal an older layer.
    std::memset(destination, 0, surface.imageSize);
    if (startupTrace_) startupTrace_("pixels-cleared");
    for (int y = 0; y < height; ++y)
        std::memcpy(destination + static_cast<std::size_t>(y) * pitch,
                    pixels + static_cast<std::size_t>(y) * width,
                    static_cast<std::size_t>(width) * sizeof(Color));
    GX2RUnlockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE);
    if (startupTrace_) startupTrace_("pixels-unlocked");
    GX2Invalidate(static_cast<GX2InvalidateMode>(GX2_INVALIDATE_MODE_CPU |
                                                GX2_INVALIDATE_MODE_TEXTURE),
                  surface.image, surface.imageSize);
    if (startupTrace_) startupTrace_("pixels-invalidated");
    ++frameStats_.textureUploads;
    frameStats_.uploadedBytes += static_cast<std::size_t>(width)*height*sizeof(Color);
    return true;
}

const WiiURenderer::UploadSurface* WiiURenderer::imageTexture(const Asset* asset) {
    if (startupTrace_) startupTrace_("texture-enter");
    if (!asset || !asset->available || asset->width <= 0 || asset->height <= 0 ||
        asset->width > 4096 || asset->height > 4096 ||
        asset->pixels.size() != static_cast<std::size_t>(asset->width)*asset->height)
        return nullptr;
    if (const auto found=imageTextures_.find(asset); found!=imageTextures_.end()) {
        if (imageTextureRevisions_[asset]!=asset->revision) {
            const auto start=profileStamp();
            // Procedural slots keep stable Asset addresses while pixels and
            // dimensions change. Resize rarely; wait before releasing a
            // surface that an earlier frame might still be sampling.
            if (found->second->width!=asset->width ||
                found->second->height!=asset->height) {
                const std::size_t oldBytes=static_cast<std::size_t>(
                    found->second->width)*found->second->height*sizeof(Color);
                const std::size_t newBytes=static_cast<std::size_t>(
                    asset->width)*asset->height*sizeof(Color);
                if (imageTextureBytes_-oldBytes+newBytes>128*1024*1024)
                    return nullptr;
                GX2DrawDone();
                destroyUploadSurface(*found->second);
                imageTextureBytes_-=oldBytes;
                try { createUploadSurface(*found->second,asset->width,asset->height,false); }
                catch (...) { return nullptr; }
                imageTextureBytes_+=newBytes;
            }
            // GX2RLockSurfaceEx synchronizes a surface still being read by
            // the GPU before its pixels are replaced.
            if (!uploadPixels(*found->second,asset->pixels.data(),
                              asset->width,asset->height)) return nullptr;
            imageTextureRevisions_[asset]=asset->revision;
            frameStats_.uploadMs+=profileElapsedMs(start);
        }
        return found->second.get();
    }
    const std::size_t bytes = static_cast<std::size_t>(asset->width)*asset->height*sizeof(Color);
    // Keep a bounded resident atlas of immutable decoded assets. AssetManager
    // owns the Asset objects for the renderer's entire lifetime.
    if (bytes > 128*1024*1024 || imageTextureBytes_ + bytes > 128*1024*1024)
        return nullptr;
    const auto start=profileStamp();
    auto texture=std::make_unique<UploadSurface>();
    if (startupTrace_) startupTrace_("texture-create-begin");
    try {
        createUploadSurface(*texture, asset->width, asset->height, false);
        if (startupTrace_) startupTrace_("texture-create-done");
        if (!uploadPixels(*texture, asset->pixels.data(),asset->width,asset->height)) {
            destroyUploadSurface(*texture);
            return nullptr;
        }
    } catch (...) {
        destroyUploadSurface(*texture);
        return nullptr;
    }
    imageTextureBytes_ += bytes;
    const UploadSurface* result=texture.get();
    imageTextures_.emplace(asset,std::move(texture));
    imageTextureRevisions_[asset]=asset->revision;
    if (startupTrace_) startupTrace_("texture-cached");
    frameStats_.uploadMs += profileElapsedMs(start);
    return result;
}

const Asset* WiiURenderer::filteredTexture(const Asset* source,double hue,
                                            double saturation) {
    if (!source || !source->available || !std::isfinite(hue) ||
        !std::isfinite(saturation)) return nullptr;
    const int degrees=static_cast<int>(std::lround(std::remainder(hue,360.0)));
    const int saturation100=static_cast<int>(std::lround(
        std::clamp(saturation,0.0,4.0)*100.0));
    if (degrees==0 && saturation100==100) return source;
    const auto key=std::make_tuple(source,degrees,saturation100);
    auto found=filteredAssets_.find(key);
    if (found==filteredAssets_.end()) {
        auto entry=std::make_unique<FilteredAsset>();
        entry->sourceRevision=std::numeric_limits<std::uint64_t>::max();
        found=filteredAssets_.emplace(key,std::move(entry)).first;
    }
    FilteredAsset& entry=*found->second;
    if (entry.sourceRevision!=source->revision ||
        entry.image.width!=source->width || entry.image.height!=source->height) {
        const std::size_t oldBytes=entry.image.pixels.size()*sizeof(Color);
        const std::size_t newBytes=source->pixels.size()*sizeof(Color);
        if (filteredAssetBytes_-oldBytes+newBytes>32*1024*1024) return nullptr;
        filteredAssetBytes_=filteredAssetBytes_-oldBytes+newBytes;
        entry.image.type=AssetType::Image;
        entry.image.available=true;
        entry.image.width=source->width;
        entry.image.height=source->height;
        entry.image.pixels.resize(source->pixels.size());
        for (std::size_t i=0;i<source->pixels.size();++i)
            entry.image.pixels[i]=applyHueSaturation(source->pixels[i],degrees,
                                                      saturation100/100.0);
        ++entry.image.revision;
        entry.sourceRevision=source->revision;
    }
    return &entry.image;
}

WiiURenderer::FontAtlas* WiiURenderer::fontAtlas(const Asset* font,int size) {
    const auto key=std::make_pair(font,size);
    if (auto found=fontAtlases_.find(key);found!=fontAtlases_.end())
        return found->second.get();
    auto atlas=std::make_unique<FontAtlas>();
    if (font && font->available && !font->bytes.empty()) {
        atlas->face=std::make_unique<FontFace>(font->bytes);
        if (!atlas->face->ready()) atlas->face.reset();
    }
    EmbeddedFont& embedded=EmbeddedFont::instance();
    if (!atlas->face && !embedded.ready()) return nullptr;
    const int rasterSize=std::clamp(static_cast<int>(std::lround(size*uiTextureScale_)),6,96);
    atlas->scale=static_cast<double>(rasterSize)/size;
    constexpr int atlasWidth=1024;
    int x=1,y=1,rowHeight=0;
    std::vector<int> codepoints;
    for (int codepoint=32;codepoint<=126;++codepoint)
        codepoints.push_back(codepoint);
    codepoints.push_back(0x2022); // bullet in game hints
    codepoints.push_back(0x2014); // em dash in messages
    struct PackedGlyph { int codepoint,x,y; const GlyphBitmap* bitmap; };
    std::vector<PackedGlyph> packed;
    packed.reserve(codepoints.size());
    for (int codepoint:codepoints) {
        const GlyphBitmap& bitmap=atlas->face
            ?atlas->face->glyph(codepoint,rasterSize)
            :embedded.glyph(codepoint,rasterSize);
        if (bitmap.width+2>atlasWidth) return nullptr;
        if (x+bitmap.width+2>atlasWidth) { x=1;y+=rowHeight+2;rowHeight=0; }
        if (y+bitmap.height+2>1024) return nullptr;
        packed.push_back({codepoint,x,y,&bitmap});
        x+=bitmap.width+2;
        rowHeight=std::max(rowHeight,bitmap.height);
    }
    const int atlasHeight=std::max(32,(y+rowHeight+33)&~31);
    atlas->asset.type=AssetType::Image;
    atlas->asset.available=true;
    atlas->asset.width=atlasWidth;
    atlas->asset.height=atlasHeight;
    atlas->asset.pixels.assign(static_cast<std::size_t>(atlasWidth*atlasHeight),
                               Color{255,255,255,0});
    for (const PackedGlyph& entry:packed) {
        const GlyphBitmap& bitmap=*entry.bitmap;
        FontAtlas::Glyph& glyph=atlas->glyphs[entry.codepoint];
        glyph={entry.x,entry.y,bitmap.width,bitmap.height,bitmap.xoff,
               bitmap.yoff,bitmap.advance};
        for (int row=0;row<bitmap.height;++row)
            for (int column=0;column<bitmap.width;++column)
                atlas->asset.pixels[static_cast<std::size_t>(
                    (entry.y+row)*atlasWidth+entry.x+column)].a=
                    bitmap.coverage[static_cast<std::size_t>(row*bitmap.width+column)];
    }
    if (!imageTexture(&atlas->asset)) return nullptr;
    FontAtlas* result=atlas.get();
    fontAtlases_.emplace(key,std::move(atlas));
    return result;
}

const Asset* WiiURenderer::shapeTexture(const Node& node,
                                        const ComputedStyle& style,bool shape) {
    if (startupTrace_) startupTrace_("shape-enter");
    if (node.opacity!=1.0) {
        if (startupTrace_) startupTrace_("shape-skip-opacity");
        return nullptr;
    }
    if (style.blendMode!=BlendMode::Normal &&
        (!style.backgrounds.empty() || style.borderWidth>0 ||
         !style.shadows.empty() || style.sawtooth)) {
        if (startupTrace_) startupTrace_("shape-skip-blend");
        return nullptr;
    }
    const Rect bounds=node.bounds();
    auto integral=[](double value) {return std::abs(value-std::round(value))<0.001;};
    if (!integral(bounds.x) || !integral(bounds.y) ||
        !integral(bounds.width) || !integral(bounds.height) ||
        bounds.width<=0 || bounds.height<=0) {
        if (startupTrace_) startupTrace_("shape-skip-bounds");
        return nullptr;
    }
    const std::string kind=shape && node.attributes.contains("kind")
        ?node.attributes.at("kind"):"rectangle";
    if (kind!="rectangle" && kind!="rect" && kind!="shape" &&
        kind!="circle" && kind!="ellipse") return nullptr;
    double padding=2;
    for (const Shadow& shadow:style.shadows)
        padding=std::max(padding,std::abs(shadow.x)+std::abs(shadow.y)+
                                 shadow.blur*3.0+4.0);
    if (style.sawtooth) padding=std::max(padding,style.sawtooth->depth+4.0);
    const int margin=static_cast<int>(std::ceil(padding));
    const int textureWidth=static_cast<int>(bounds.width)+margin*2;
    const int textureHeight=static_cast<int>(bounds.height)+margin*2;
    const int rasterWidth=static_cast<int>(std::lround(textureWidth*uiTextureScale_));
    const int rasterHeight=static_cast<int>(std::lround(textureHeight*uiTextureScale_));
    if (startupTrace_) {
        const std::string stage="shape-size "+std::to_string(rasterWidth)+
            "x"+std::to_string(rasterHeight);
        startupTrace_(stage.c_str());
    }
    if (rasterWidth>2048 || rasterHeight>2048) return nullptr;
    const std::size_t bytes=static_cast<std::size_t>(rasterWidth)*rasterHeight*sizeof(Color);
    if (startupTrace_) {
        const std::string stage="shape-style fill="+
            std::to_string(style.fill.has_value())+" backgrounds="+
            std::to_string(style.backgrounds.size())+" shadows="+
            std::to_string(style.shadows.size())+" bytes="+
            std::to_string(bytes);
        startupTrace_(stage.c_str());
    }
    ShapeKey key;
    if (startupTrace_) startupTrace_("shape-key-ready");
    if (!appendText(key,kind)) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-kind");
    if (!appendNumber(key,bounds.width)) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-width");
    if (!appendNumber(key,bounds.height)) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-height");
    if (!appendNumber(key,static_cast<double>(margin))) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-bounds");
    if (!appendByte(key,style.fill?1:0)) return nullptr;
    if (style.fill && !appendPaint(key,*style.fill)) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-fill");
    if (!appendNumber(key,static_cast<double>(style.backgrounds.size()))) return nullptr;
    for (const Paint& paint:style.backgrounds)
        if (!appendPaint(key,paint)) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-backgrounds");
    if (!appendNumber(key,style.borderWidth) ||
        !appendColor(key,style.borderColor) ||
        !appendNumber(key,style.borderRadius)) return nullptr;
    if (startupTrace_) startupTrace_("shape-key-border");
    if (!appendNumber(key,static_cast<double>(style.shadows.size()))) return nullptr;
    for (const Shadow& shadow:style.shadows) {
        if (!appendNumber(key,shadow.x) || !appendNumber(key,shadow.y) ||
            !appendNumber(key,shadow.blur) ||
            !appendColor(key,shadow.color) ||
            !appendByte(key,shadow.inset?1:0)) return nullptr;
    }
    if (startupTrace_) startupTrace_("shape-key-shadows");
    if (!appendByte(key,style.sawtooth?1:0)) return nullptr;
    if (style.sawtooth) {
        if (!appendText(key,style.sawtooth->side) ||
            !appendNumber(key,style.sawtooth->tooth) ||
            !appendNumber(key,style.sawtooth->depth)) return nullptr;
    }
    if (startupTrace_) startupTrace_("shape-cache-lookup");
    if (auto found=shapeTextures_.find(key);found!=shapeTextures_.end())
        return found->second.get();
    if (startupTrace_) startupTrace_("shape-cache-miss");
    if (shapeTextureBytes_+bytes>48*1024*1024) return nullptr;
    if (startupTrace_) startupTrace_("shape-raster-begin");
    const auto rasterStart=profileStamp();
    SoftwareRenderer raster(textureWidth,textureHeight,{},uiTextureScale_);
    raster.beginDisplay(Display::Tv,{0,0,0,0});
    if (startupTrace_) startupTrace_("shape-raster-ready");
    Node local;
    local.type=shape?NodeType::Shape:NodeType::Panel;
    local.transform.position={static_cast<double>(margin),static_cast<double>(margin)};
    local.width=bounds.width;local.height=bounds.height;
    if (shape) local.attributes["kind"]=kind;
    ComputedStyle rasterStyle=style;
    rasterStyle.blendMode=BlendMode::Normal;
    if (shape) raster.drawShape(local,rasterStyle);
    else raster.drawRectangle(local,rasterStyle);
    if (startupTrace_) startupTrace_("shape-raster-drawn");
    auto asset=std::make_unique<Asset>();
    asset->type=AssetType::Image;
    asset->available=true;
    asset->width=raster.width();asset->height=raster.height();
    asset->pixels=raster.pixels(Display::Tv);
    frameStats_.rasterMs+=profileElapsedMs(rasterStart);
    if (startupTrace_) startupTrace_("shape-upload-begin");
    if (!imageTexture(asset.get())) return nullptr;
    if (startupTrace_) startupTrace_("shape-upload-done");
    const Asset* result=asset.get();
    shapeTextures_.emplace(std::move(key),std::move(asset));
    shapeTextureBytes_+=bytes;
    return result;
}

void WiiURenderer::drawSprite(const Node& node, const ComputedStyle& style,
                              const Asset* texture) {
    bool visibleFill=false;
    if (style.fill) {
        if (const auto* solid=std::get_if<Color>(&*style.fill)) visibleFill=solid->a!=0;
        else visibleFill=true;
    }
    const bool decorated=visibleFill || !style.backgrounds.empty() ||
        style.borderWidth>0 || !style.shadows.empty() || style.sawtooth ||
        style.borderRadius>0;
    const auto start=profileStamp();
    const Asset* actualTexture=gpu2DEnabled_ && !decorated
        ?filteredTexture(texture,node.hue,node.saturation):nullptr;
    const UploadSurface* gpuTexture=actualTexture?imageTexture(actualTexture):nullptr;
    if (gpuTexture && current2D().positions.size()/3+12<=maxDirectMeshVertices) {
        Frame2D& frame=current2D();
        flushSoftwareLayer(frame);
        const auto geometryStart=profileStamp();
        queueQuad(frame,node.bounds(),node.worldTransform().rotation,node.tint,
                  node.opacity,style.blendMode,actualTexture,0,false);
        ++frame.spriteCount;
        frameStats_.geometryMs += profileElapsedMs(geometryStart);
        return;
    }
    if (gpu2DEnabled_) {
        ensureSoftwareTarget();
        current2D().softwarePending=true;
        markSoftwareBounds(current2D(),node,style);
    }
    SoftwareRenderer::drawSprite(node,style,texture);
    frameStats_.rasterMs += profileElapsedMs(start);
}

void WiiURenderer::drawSpriteBatch(const Node& node,const ComputedStyle& style,
                                   const Asset* texture) {
    const auto start=profileStamp();
    std::size_t visible=0;
    for (const SpriteInstance& instance:node.batch.sprites)
        visible += instance.visible && instance.opacity>0;
    if (!visible) return;
    const Asset* actualTexture=gpu2DEnabled_
        ?filteredTexture(texture,node.hue,node.saturation):nullptr;
    const UploadSurface* gpuTexture=actualTexture?imageTexture(actualTexture):nullptr;
    Frame2D& frame=current2D();
    if (gpuTexture && frame.positions.size()/3+visible*6+6<=maxDirectMeshVertices) {
        flushSoftwareLayer(frame);
        const auto geometryStart=profileStamp();
        const Rect visibleArea=visibleBounds(currentDisplay_);
        const Transform transform=node.worldTransform();
        const double angle=transform.rotation*3.14159265358979323846/180.0;
        const double cosine=std::cos(angle),sine=std::sin(angle);
        const double defaultWidth=node.width>0?node.width:texture->width;
        const double defaultHeight=node.height>0?node.height:texture->height;
        for (const SpriteInstance& instance:node.batch.sprites) {
            if (!instance.visible || instance.opacity<=0) continue;
            const double localX=instance.x*transform.scale.x;
            const double localY=instance.y*transform.scale.y;
            const double left=transform.position.x+localX*cosine-localY*sine;
            const double top=transform.position.y+localX*sine+localY*cosine;
            const double width=std::abs((instance.width>0?instance.width:defaultWidth)*
                                        transform.scale.x*instance.scaleX);
            const double height=std::abs((instance.height>0?instance.height:defaultHeight)*
                                         transform.scale.y*instance.scaleY);
            if (width<0.01 || height<0.01) continue;
            const double rotation=transform.rotation+instance.rotation;
            const double radians=rotation*3.14159265358979323846/180.0;
            const double extentX=std::abs(std::cos(radians))*width*0.5+
                                 std::abs(std::sin(radians))*height*0.5;
            const double extentY=std::abs(std::sin(radians))*width*0.5+
                                 std::abs(std::cos(radians))*height*0.5;
            const double centerX=left+width*0.5,centerY=top+height*0.5;
            if (centerX+extentX<visibleArea.x || centerY+extentY<visibleArea.y ||
                centerX-extentX>visibleArea.x+visibleArea.width ||
                centerY-extentY>visibleArea.y+visibleArea.height)
                continue;
            Color tint{
                static_cast<std::uint8_t>(node.tint.r*instance.tint.r/255),
                static_cast<std::uint8_t>(node.tint.g*instance.tint.g/255),
                static_cast<std::uint8_t>(node.tint.b*instance.tint.b/255),
                static_cast<std::uint8_t>(node.tint.a*instance.tint.a/255)};
            queueQuad(frame,{left,top,width,height},rotation,tint,
                      node.opacity*instance.opacity,style.blendMode,actualTexture,0,false);
            ++frame.spriteCount;
        }
        frameStats_.geometryMs += profileElapsedMs(geometryStart);
        return;
    }
    if (gpu2DEnabled_) {
        ensureSoftwareTarget();
        frame.softwarePending=true;
        frame.scanLeft=0;frame.scanTop=0;
        frame.scanRight=width_;frame.scanBottom=height_;
    }
    SoftwareRenderer::drawSpriteBatch(node,style,texture);
    frameStats_.rasterMs += profileElapsedMs(start);
}

void WiiURenderer::drawText(const Node& node,const ComputedStyle& style,
                            const Asset* font) {
    const auto start=profileStamp();
    std::vector<int> codepoints;
    if (gpu2DEnabled_ && !node.text.empty() &&
        current2D().positions.size()/3+node.text.size()*6+12<=maxDirectMeshVertices) {
        const bool supported=decodeUiText(node.text,codepoints);
        const int size=std::max(4,static_cast<int>(std::lround(style.fontSize)));
        FontAtlas* atlas=supported?fontAtlas(font,size):nullptr;
        if (atlas) {
            Frame2D& frame=current2D();
            flushSoftwareLayer(frame);
            const auto geometryStart=profileStamp();
            const Transform transform=node.worldTransform();
            const float pixelHeight=static_cast<float>(std::max(4.0,style.fontSize));
            const double scale=atlas->scale;
            const float rasterHeight=static_cast<float>(pixelHeight*scale);
            const float lineHeight=pixelHeight*1.2f;
            float lineTop=static_cast<float>(transform.position.y);
            EmbeddedFont& embedded=EmbeddedFont::instance();
            const float ascent=(atlas->face?atlas->face->ascent(rasterHeight):
                                            embedded.ascent(rasterHeight))/scale;
            float baseline=lineTop+ascent;
            float cursor=static_cast<float>(transform.position.x);
            int previous=0;
            for (int raw:codepoints) {
                if (raw=='\n') {
                    cursor=static_cast<float>(transform.position.x);
                    lineTop+=lineHeight;
                    baseline=lineTop+ascent;
                    previous=0;
                    continue;
                }
                const int codepoint=raw=='\t'?' ':raw;
                const float kerning=(atlas->face
                    ?atlas->face->kerning(previous,codepoint,rasterHeight)
                    :embedded.kerning(previous,codepoint,rasterHeight))/scale;
                cursor+=kerning;
                const FontAtlas::Glyph& glyph=atlas->glyphs.at(codepoint);
                if (glyph.width>0 && glyph.height>0) {
                    const double left=std::floor(cursor*scale)/scale+glyph.xoff/scale;
                    const double top=std::floor(baseline*scale)/scale+glyph.yoff/scale;
                    queueQuad(frame,{left,top,glyph.width/scale,
                                      glyph.height/scale},0,
                              style.textColor,node.opacity,style.blendMode,&atlas->asset,
                              0,false,
                              static_cast<double>(glyph.x+glyph.width)/atlas->asset.width,
                              static_cast<double>(glyph.y+glyph.height)/atlas->asset.height,
                              static_cast<double>(glyph.x)/atlas->asset.width,
                              static_cast<double>(glyph.y)/atlas->asset.height);
                }
                cursor+=glyph.advance/scale;
                previous=codepoint;
            }
            frameStats_.geometryMs+=profileElapsedMs(geometryStart);
            return;
        }
    }
    if (gpu2DEnabled_) {
        ensureSoftwareTarget();
        current2D().softwarePending=true;
        markSoftwareBounds(current2D(),node,style,true);
    }
    SoftwareRenderer::drawText(node,style,font);
    frameStats_.rasterMs += profileElapsedMs(start);
}

void WiiURenderer::drawRectangle(const Node& node,const ComputedStyle& style) {
    if (startupTrace_) startupTrace_("rectangle-enter");
    const auto start=profileStamp();
    if (gpu2DEnabled_ && style.fill &&
        std::holds_alternative<Color>(*style.fill) &&
        style.backgrounds.empty() && style.borderWidth<=0 &&
        style.borderRadius<=0 && style.shadows.empty() && !style.sawtooth &&
        current2D().positions.size()/3+12<=maxDirectMeshVertices) {
        Frame2D& frame=current2D();
        flushSoftwareLayer(frame);
        const auto geometryStart=profileStamp();
        queueQuad(frame,node.bounds(),0,std::get<Color>(*style.fill),
                  node.opacity,style.blendMode,nullptr,0,false);
        frameStats_.geometryMs+=profileElapsedMs(geometryStart);
        return;
    }
    if (gpu2DEnabled_ && current2D().positions.size()/3+12<=maxDirectMeshVertices) {
        if (startupTrace_) startupTrace_("rectangle-shape-check");
        if (const Asset* cached=shapeTexture(node,style,false)) {
            Frame2D& frame=current2D();
            if (startupTrace_) startupTrace_("panel-flush-begin");
            flushSoftwareLayer(frame);
            if (startupTrace_) startupTrace_("panel-flush-done");
            const auto geometryStart=profileStamp();
            const Rect bounds=node.bounds();
            const double logicalWidth=std::round(cached->width/uiTextureScale_);
            const double logicalHeight=std::round(cached->height/uiTextureScale_);
            const double margin=(logicalWidth-bounds.width)*0.5;
            queueQuad(frame,{bounds.x-margin,bounds.y-margin,
                             logicalWidth,logicalHeight},0,
                      {255,255,255,255},1,style.blendMode,cached,0,false);
            if (startupTrace_) startupTrace_("panel-queued");
            frameStats_.geometryMs+=profileElapsedMs(geometryStart);
            return;
        }
    }
    if (gpu2DEnabled_) {
        if (startupTrace_) startupTrace_("rectangle-software-setup");
        ensureSoftwareTarget();
        current2D().softwarePending=true;
        markSoftwareBounds(current2D(),node,style);
    }
    if (startupTrace_) startupTrace_("rectangle-software-draw");
    SoftwareRenderer::drawRectangle(node,style);
    frameStats_.rasterMs += profileElapsedMs(start);
}

void WiiURenderer::drawPanel(const Node& node,const ComputedStyle& style) {
    drawRectangle(node,style);
}

void WiiURenderer::drawShape(const Node& node,const ComputedStyle& style) {
    const auto start=profileStamp();
    if (gpu2DEnabled_ && current2D().positions.size()/3+12<=maxDirectMeshVertices) {
        if (const Asset* cached=shapeTexture(node,style,true)) {
            Frame2D& frame=current2D();
            flushSoftwareLayer(frame);
            const auto geometryStart=profileStamp();
            const Rect bounds=node.bounds();
            const double logicalWidth=std::round(cached->width/uiTextureScale_);
            const double logicalHeight=std::round(cached->height/uiTextureScale_);
            const double margin=(logicalWidth-bounds.width)*0.5;
            queueQuad(frame,{bounds.x-margin,bounds.y-margin,
                             logicalWidth,logicalHeight},0,
                      {255,255,255,255},1,style.blendMode,cached,0,false);
            frameStats_.geometryMs+=profileElapsedMs(geometryStart);
            return;
        }
    }
    if (gpu2DEnabled_) {
        ensureSoftwareTarget();
        current2D().softwarePending=true;
        markSoftwareBounds(current2D(),node,style);
    }
    SoftwareRenderer::drawShape(node,style);
    frameStats_.rasterMs += profileElapsedMs(start);
}

void WiiURenderer::endDisplay(Display display) {
    if (!gpu2DEnabled_) return;
    currentDisplay_=display;
    currentPixels_=display==Display::GamePad?&gamePadPixels_:&tvPixels_;
    flushSoftwareLayer(current2D());
    if (startupTrace_)
        startupTrace_(display==Display::Tv?"render-tv-done":"render-gamepad-done");
}

void WiiURenderer::drawMesh3D(const Node& node, const ComputedStyle& style,
                              const Asset* mesh, const Node* camera) {
    static const std::vector<MeshVertex> cubeVertices = {
        {{-0.5,-0.5,-0.5}}, {{0.5,-0.5,-0.5}}, {{0.5,0.5,-0.5}}, {{-0.5,0.5,-0.5}},
        {{-0.5,-0.5, 0.5}}, {{0.5,-0.5, 0.5}}, {{0.5,0.5, 0.5}}, {{-0.5,0.5, 0.5}}
    };
    static const std::vector<std::uint32_t> cubeIndices = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
        3,7,6, 3,6,2, 1,2,6, 1,6,5, 0,4,7, 0,7,3
    };
    const auto& vertices = mesh && !mesh->vertices.empty() ? mesh->vertices : cubeVertices;
    const auto& indices = mesh && mesh->indices.size() >= 3 ? mesh->indices : cubeIndices;
    MeshBatch& batch = currentDisplay_ == Display::GamePad ? gamePadMeshes_ : tvMeshes_;
    const Rect& targetViewport = viewport(currentDisplay_);
    const int targetWidth = outputWidth(currentDisplay_);
    const int targetHeight = outputHeight(currentDisplay_);
    if (targetWidth <= 0 || targetHeight <= 0 || targetViewport.height <= 0) return;

    const Vec3 cameraPosition = camera ? camera->transform3D.position : Vec3{0, 0, -5};
    const Vec3 cameraRotation = camera ? camera->transform3D.rotation : Vec3{};
    const double nearClip = camera ? std::max(0.01, camera->nearClip) : 0.1;
    const double farClip = camera ? std::max(nearClip + 1.0, camera->farClip) : 1000.0;
    const double fieldOfView = camera ? std::clamp(camera->fieldOfView, 10.0, 150.0) : 60.0;
    const double tangent = std::tan(fieldOfView * 3.14159265358979323846 / 360.0);
    const double aspect = targetViewport.width / targetViewport.height;
    const double scaleX = node.width > 0 ? node.width : node.transform3D.scale.x;
    const double scaleY = node.height > 0 ? node.height : node.transform3D.scale.y;
    const double scaleZ = node.depth > 1.0 ? node.depth : node.transform3D.scale.z;
    const Color base = meshColor(style);
    const Vec3 lightDirection = normalized(Vec3{-0.4, 0.7, -0.6});
    const std::uint32_t firstVertex = static_cast<std::uint32_t>(batch.positions.size()/3);
    batch.ranges.push_back({firstVertex, 0, style.blendMode,
                            style.blendMode == BlendMode::Normal && node.opacity >= 0.999});
    auto& range = batch.ranges.back();
    bool capacityReached = false;

    auto worldPosition = [&](std::uint32_t index) {
        const Vec3 source = vertices[index].position;
        return rotateObject({source.x*scaleX, source.y*scaleY, source.z*scaleZ},
                            node.transform3D.rotation) + node.transform3D.position;
    };
    auto smoothLight = [&](std::uint32_t index) {
        Vec3 normal = vertices[index].normal;
        if (dot(normal,normal)<0.000001) normal=normalized(vertices[index].position);
        normal=normalized(rotateObject(normal,node.transform3D.rotation));
        return std::clamp(0.2+std::max(0.0,dot(normal,lightDirection))*0.8,0.2,1.0);
    };
    struct Projected { float x, y, z; };
    auto project = [&](Vec3 view) {
        const double xNdc = view.x / (view.z * tangent * aspect);
        const double yNdc = view.y / (view.z * tangent);
        const double screenX = targetViewport.x + (xNdc * 0.5 + 0.5) * targetViewport.width;
        const double screenY = targetViewport.y + (0.5 - yNdc * 0.5) * targetViewport.height;
        const double outputX = screenX / targetWidth * 2.0 - 1.0;
        const double outputY = 1.0 - screenY / targetHeight * 2.0;
        const double outputZ = farClip / (farClip-nearClip) -
                               farClip*nearClip / ((farClip-nearClip)*view.z);
        return Projected{static_cast<float>(outputX), static_cast<float>(outputY),
                         static_cast<float>(std::clamp(outputZ, 0.0, 1.0))};
    };

    for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3) {
        const std::uint32_t ia=indices[triangle], ib=indices[triangle+1], ic=indices[triangle+2];
        if (ia>=vertices.size() || ib>=vertices.size() || ic>=vertices.size()) continue;
        const Vec3 wa=worldPosition(ia), wb=worldPosition(ib), wc=worldPosition(ic);
        const Vec3 originalView[3]{toCameraSpace(wa,cameraPosition,cameraRotation),
                                   toCameraSpace(wb,cameraPosition,cameraRotation),
                                   toCameraSpace(wc,cameraPosition,cameraRotation)};
        const bool unclipped=originalView[0].z>=nearClip&&originalView[0].z<=farClip&&
                             originalView[1].z>=nearClip&&originalView[1].z<=farClip&&
                             originalView[2].z>=nearClip&&originalView[2].z<=farClip;
        std::vector<Vec3> polygon{originalView[0],originalView[1],originalView[2]};
        polygon = clipDepth(std::move(polygon), nearClip, true);
        polygon = clipDepth(std::move(polygon), farClip, false);
        if (polygon.size() < 3) continue;
        const Vec3 normal = normalized(cross(wb-wa,wc-wa));
        const double light = style.shadingMode == ShadingMode::Unlit ? 1.0 :
            std::clamp(0.2 + std::max(0.0, dot(normal,lightDirection))*0.8, 0.2, 1.0);
        const float alpha=static_cast<float>(base.a/255.0*node.opacity);
        const double vertexLights[3]{smoothLight(ia),smoothLight(ib),smoothLight(ic)};
        for (std::size_t fan=1; fan+1<polygon.size(); ++fan) {
            const Projected points[3]{project(polygon[0]),project(polygon[fan]),project(polygon[fan+1])};
            const double area=(points[1].x-points[0].x)*(points[2].y-points[0].y)-
                              (points[1].y-points[0].y)*(points[2].x-points[0].x);
            if (area >= -0.000001) continue;
            if (batch.positions.size()/3+3 > maxDirectMeshVertices) {
                capacityReached = true;
                break;
            }
            for (int pointIndex=0;pointIndex<3;++pointIndex) {
                const Projected& point=points[pointIndex];
                const double pointLight=style.shadingMode==ShadingMode::Unlit?1.0:
                    (style.shadingMode==ShadingMode::Gouraud&&unclipped&&polygon.size()==3
                        ?vertexLights[pointIndex]:light);
                float red=static_cast<float>(base.r/255.0*pointLight);
                float green=static_cast<float>(base.g/255.0*pointLight);
                float blue=static_cast<float>(base.b/255.0*pointLight);
                if(style.blendMode==BlendMode::Multiply||style.blendMode==BlendMode::Screen){red*=alpha;green*=alpha;blue*=alpha;}
                batch.positions.insert(batch.positions.end(),{point.x,point.y,point.z});
                batch.textureCoordinates.insert(batch.textureCoordinates.end(),{0.5f,0.5f});
                batch.colors.insert(batch.colors.end(),{red,green,blue,alpha});
            }
            range.count += 3;
        }
        if (capacityReached) break;
    }
    if (range.count == 0) batch.ranges.pop_back();
}

void WiiURenderer::drawParticles(const Node& node, const ComputedStyle& style,
                                 const Asset*) {
    frameStats_.particles += node.emitter.particles.size();
    MeshBatch& batch = currentDisplay_ == Display::GamePad ? gamePadMeshes_ : tvMeshes_;
    const Rect& targetViewport = viewport(currentDisplay_);
    const Rect visible = visibleBounds(currentDisplay_);
    const int targetWidth = outputWidth(currentDisplay_);
    const int targetHeight = outputHeight(currentDisplay_);
    if (targetWidth <= 0 || targetHeight <= 0) return;
    constexpr double tau = 6.28318530717958647692;
    static const std::array<Vec2,17> unitCircle = [] {
        std::array<Vec2,17> points{};
        for (int i = 0; i <= 16; ++i) {
            const double angle = tau * i / 16;
            points[static_cast<std::size_t>(i)] = {std::cos(angle), std::sin(angle)};
        }
        return points;
    }();
    auto clipPoint = [&](double logicalX, double logicalY) {
        const double screenX = targetViewport.x +
            (logicalX-visible.x) / visible.width * targetViewport.width;
        const double screenY = targetViewport.y +
            (logicalY-visible.y) / visible.height * targetViewport.height;
        return Vec2{screenX / targetWidth * 2.0 - 1.0,
                    1.0 - screenY / targetHeight * 2.0};
    };
    const std::uint32_t firstVertex = static_cast<std::uint32_t>(batch.positions.size()/3);
    batch.ranges.push_back({firstVertex, 0, style.blendMode, false, false});
    auto& range = batch.ranges.back();
    const Transform emitterTransform = node.worldTransform();
    auto appendVertex = [&](Vec2 point, float red, float green, float blue, float alpha) {
        batch.positions.insert(batch.positions.end(),
                               {static_cast<float>(point.x), static_cast<float>(point.y), 0.0f});
        batch.textureCoordinates.insert(batch.textureCoordinates.end(), {0.5f, 0.5f});
        batch.colors.insert(batch.colors.end(), {red, green, blue, alpha});
        ++range.count;
    };
    for (const Particle& particle : node.emitter.particles) {
        const int segments = std::max(particle.size, particle.endSize) <= 8 ? 8 : 16;
        const std::size_t required = particle.shape == ParticleShape::Rectangle ? 6 : segments*3;
        if (batch.positions.size()/3+required > maxDirectMeshVertices) break;
        const double progress = std::clamp(particle.age /
            std::max(0.001, particle.lifetime), 0.0, 1.0);
        constexpr double parentRadians = 3.14159265358979323846 / 180.0;
        const double parentCos = std::cos(emitterTransform.rotation * parentRadians);
        const double parentSin = std::sin(emitterTransform.rotation * parentRadians);
        const double localX = particle.position.x * emitterTransform.scale.x;
        const double localY = particle.position.y * emitterTransform.scale.y;
        const double centerX = particle.camera
            ? emitterTransform.position.x + localX * parentCos - localY * parentSin
            : particle.position.x;
        const double centerY = particle.camera
            ? emitterTransform.position.y + localX * parentSin + localY * parentCos
            : particle.position.y;
        const double size = particle.size + (particle.endSize-particle.size)*progress;
        const double radius = size * 0.5;
        if (centerX + radius < visible.x || centerY + radius < visible.y ||
            centerX - radius > visible.x+visible.width ||
            centerY - radius > visible.y+visible.height) continue;
        const Vec2 center = clipPoint(centerX, centerY);
        const Color lifetime=Color::lerp(particle.color,particle.endColor,progress);
        const auto [centerColor,edgeColor]=particleRadialColors(style,lifetime);
        auto channels=[&](Color color,float& red,float& green,float& blue,float& alpha){
            red=color.r/255.0f;green=color.g/255.0f;blue=color.b/255.0f;
            alpha=static_cast<float>(color.a/255.0*node.opacity);
            if(style.blendMode==BlendMode::Multiply||style.blendMode==BlendMode::Screen){red*=alpha;green*=alpha;blue*=alpha;}
        };
        float centerRed,centerGreen,centerBlue,centerAlpha;
        float edgeRed,edgeGreen,edgeBlue,edgeAlpha;
        channels(centerColor,centerRed,centerGreen,centerBlue,centerAlpha);
        channels(edgeColor,edgeRed,edgeGreen,edgeBlue,edgeAlpha);
        if (particle.shape == ParticleShape::Rectangle) {
            constexpr double radians = 3.14159265358979323846/180.0;
            const double rotation=particle.rotation+(particle.camera?emitterTransform.rotation:0);
            const double c=std::cos(rotation*radians);
            const double s=std::sin(rotation*radians);
            const double halfWidth=radius*particle.aspect, halfHeight=radius;
            const Vec2 local[4]{{-halfWidth,-halfHeight},{halfWidth,-halfHeight},
                                {halfWidth,halfHeight},{-halfWidth,halfHeight}};
            Vec2 corners[4];
            for(int i=0;i<4;++i)corners[i]=clipPoint(centerX+local[i].x*c-local[i].y*s,
                                                     centerY+local[i].x*s+local[i].y*c);
            const int order[6]{0,1,2,0,2,3};
            for(int index:order)appendVertex(corners[index],centerRed,centerGreen,centerBlue,centerAlpha);
        } else {
            for (int segment = 0; segment < segments; ++segment) {
                const int step = 16 / segments;
                const Vec2 p0 = unitCircle[static_cast<std::size_t>(segment * step)];
                const Vec2 p1 = unitCircle[static_cast<std::size_t>((segment + 1) * step)];
                const Vec2 edge0 = clipPoint(centerX + p0.x * radius,
                                             centerY + p0.y * radius);
                const Vec2 edge1 = clipPoint(centerX + p1.x * radius,
                                             centerY + p1.y * radius);
                appendVertex(center,centerRed,centerGreen,centerBlue,centerAlpha);
                appendVertex(edge0,edgeRed,edgeGreen,edgeBlue,edgeAlpha);
                appendVertex(edge1,edgeRed,edgeGreen,edgeBlue,edgeAlpha);
            }
        }
    }
    if (range.count == 0) batch.ranges.pop_back();
}

int WiiURenderer::outputWidth(Display display) const {
    return display == Display::GamePad ? gamePadOutputWidth_ : tvOutputWidth_;
}

int WiiURenderer::outputHeight(Display display) const {
    return display == Display::GamePad ? gamePadOutputHeight_ : tvOutputHeight_;
}

const Rect& WiiURenderer::viewport(Display display) const {
    return display == Display::GamePad ? gamePadViewport_ : tvViewport_;
}

std::optional<Vec2> WiiURenderer::outputToLogical(Display display,
                                                   double x, double y) const {
    const Rect logical=visibleBounds(display);
    const auto point=outputToVirtual(viewport(display),logical.width,logical.height,x,y);
    if (!point) return std::nullopt;
    return Vec2{point->x+logical.x,point->y+logical.y};
}

WiiURenderer::~WiiURenderer() {
    for (auto& [asset, texture] : imageTextures_) destroyUploadSurface(*texture);
    for (auto& texture : tv2D_.uploadPool) destroyUploadSurface(*texture);
    for (auto& texture : gamePad2D_.uploadPool) destroyUploadSurface(*texture);
    destroyUploadSurface(tvUpload_);
    destroyUploadSurface(gamePadUpload_);
    destroyUploadSurface(whiteTexture_);
    destroyMeshBuffers(tvMeshBuffers_);
    destroyMeshBuffers(gamePadMeshBuffers_);
    for (MeshBuffers& buffers:tv2DBuffers_) destroyMeshBuffers(buffers);
    for (MeshBuffers& buffers:gamePad2DBuffers_) destroyMeshBuffers(buffers);
    destroyPresentationShader();
    if (graphicsReady_) WHBGfxShutdown();
}

bool WiiURenderer::upload(UploadSurface& upload, const std::vector<Color>& source) {
    if (source.empty() || upload.width <= 0 || upload.height <= 0) return false;
    GX2Surface& surface = upload.texture.surface;
    auto* destination = static_cast<std::uint32_t*>(
        GX2RLockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE));
    if (!destination) return false;

    const std::uint32_t pitch = rowPitchPixels(surface, upload.width);
    if (pitch == static_cast<std::uint32_t>(width_) && upload.width == width_ &&
        upload.height == height_ && !upload.mapY.empty() && upload.mapY.front() == 0 &&
        upload.mapY.back() == static_cast<std::uint16_t>(height_ - 1)) {
        std::memcpy(destination, source.data(), source.size() * sizeof(Color));
        GX2RUnlockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE);
        GX2Invalidate(static_cast<GX2InvalidateMode>(GX2_INVALIDATE_MODE_CPU |
                                                    GX2_INVALIDATE_MODE_TEXTURE),
                      surface.image, surface.imageSize);
        return true;
    }
    const int integerScale = upload.width % width_ == 0 ? upload.width / width_ : 0;
    int previousSourceY = -1;
    for (int y = 0; y < upload.height; ++y) {
        const int sourceY = upload.mapY[static_cast<std::size_t>(y)];
        const Color* sourceRow = source.data() + static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(width_);
        auto* row = destination + static_cast<std::size_t>(y) * pitch;

        if (sourceY == previousSourceY) {
            std::memcpy(row, row - pitch, static_cast<std::size_t>(upload.width) * sizeof(std::uint32_t));
            continue;
        }
        previousSourceY = sourceY;

        if (integerScale == 1) {
            std::memcpy(row, sourceRow, static_cast<std::size_t>(width_) * sizeof(Color));
        } else if (integerScale >= 2 && integerScale <= 4) {
            for (int x = 0; x < width_; ++x) {
                std::uint32_t value;
                std::memcpy(&value, sourceRow + x, sizeof(value));
                const int outputX = x * integerScale;
                for (int copy = 0; copy < integerScale; ++copy) row[outputX + copy] = value;
            }
        } else {
            for (int x = 0; x < upload.width; ++x) {
                std::memcpy(row + x, sourceRow + upload.mapX[static_cast<std::size_t>(x)],
                            sizeof(std::uint32_t));
            }
        }
    }

    GX2RUnlockSurfaceEx(&surface, 0, GX2R_RESOURCE_BIND_NONE);
    GX2Invalidate(static_cast<GX2InvalidateMode>(GX2_INVALIDATE_MODE_CPU |
                                                GX2_INVALIDATE_MODE_TEXTURE),
                  surface.image, surface.imageSize);
    return true;
}

void WiiURenderer::drawUpload(const UploadSurface& upload,
                              GX2RBuffer& positionBuffer) {
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xff, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA,
                       GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, TRUE,
                       GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetFetchShader(&shader_.fetchShader);
    GX2SetVertexShader(shader_.vertexShader);
    GX2SetPixelShader(shader_.pixelShader);
    GX2RSetAttributeBuffer(&positionBuffer, 0, positionBuffer.elemSize, 0);
    GX2RSetAttributeBuffer(&textureCoordinateBuffer_, 1, textureCoordinateBuffer_.elemSize, 0);
    GX2RSetAttributeBuffer(&colorBuffer_, 2, colorBuffer_.elemSize, 0);
    GX2SetPixelTexture(&upload.texture, 0);
    GX2SetPixelSampler(&sampler_, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
}

void WiiURenderer::drawMeshes(const MeshBatch& batch, MeshBuffers& buffers,
                              Display display) {
    const auto uploadStart=profileStamp();
    const std::size_t vertexCount = batch.positions.size() / 3;
    if (vertexCount == 0 ||
        !uploadAttributeBuffer(buffers.positions, batch.positions, 3) ||
        !uploadAttributeBuffer(buffers.textureCoordinates, batch.textureCoordinates, 2) ||
        !uploadAttributeBuffer(buffers.colors, batch.colors, 4)) return;
    frameStats_.uploadMs += profileElapsedMs(uploadStart);
    frameStats_.uploadedBytes += (batch.positions.size()+
        batch.textureCoordinates.size()+batch.colors.size())*sizeof(float);

    const bool needsDepth=std::any_of(batch.ranges.begin(),batch.ranges.end(),
                                     [](const MeshBatch::Range& range) {
                                         return range.depthTest;
                                     });
    GX2DepthBuffer* depth = needsDepth
        ? (display == Display::GamePad
            ? WHBGfxGetDRCDepthBuffer() : WHBGfxGetTVDepthBuffer())
        : nullptr;
    if (depth) {
        GX2SetDepthBuffer(depth);
        GX2ClearDepthStencilEx(depth, 1.0f, 0, GX2_CLEAR_FLAGS_DEPTH);
    }
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    // LEQUAL lets coplanar translucent particle triangles blend with each
    // other while still placing the 2D particle layer in front of 3D meshes.
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xff, FALSE, TRUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetFetchShader(&shader_.fetchShader);
    GX2SetVertexShader(shader_.vertexShader);
    GX2SetPixelShader(shader_.pixelShader);
    GX2RSetAttributeBuffer(&buffers.positions, 0, buffers.positions.elemSize, 0);
    GX2RSetAttributeBuffer(&buffers.textureCoordinates, 1,
                           buffers.textureCoordinates.elemSize, 0);
    GX2RSetAttributeBuffer(&buffers.colors, 2, buffers.colors.elemSize, 0);
    GX2SetPixelTexture(&whiteTexture_.texture, 0);
    GX2SetPixelSampler(&sampler_, 0);
    for (const MeshBatch::Range& range : batch.ranges) {
        GX2SetDepthOnlyControl(depth != nullptr && range.depthTest,
                               depth != nullptr && range.depthWrite,
                               GX2_COMPARE_FUNC_LEQUAL);
        GX2BlendMode source = GX2_BLEND_MODE_SRC_ALPHA;
        GX2BlendMode destination = GX2_BLEND_MODE_INV_SRC_ALPHA;
        GX2BlendCombineMode combine = GX2_BLEND_COMBINE_MODE_ADD;
        if (range.blendMode == BlendMode::Additive) destination = GX2_BLEND_MODE_ONE;
        else if (range.blendMode == BlendMode::Multiply) {
            source=GX2_BLEND_MODE_DST_COLOR; destination=GX2_BLEND_MODE_INV_SRC_ALPHA;
        } else if (range.blendMode == BlendMode::Screen) {
            source=GX2_BLEND_MODE_ONE; destination=GX2_BLEND_MODE_INV_SRC_COLOR;
        } else if (range.blendMode == BlendMode::Subtract) {
            destination=GX2_BLEND_MODE_ONE; combine=GX2_BLEND_COMBINE_MODE_REV_SUB;
        }
        GX2SetBlendControl(GX2_RENDER_TARGET_0, source, destination, combine, TRUE,
                           GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                           GX2_BLEND_COMBINE_MODE_ADD);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, range.count, range.first, 1);
        ++frameStats_.drawCalls;
    }
}

void WiiURenderer::setBlendMode(BlendMode mode) {
    GX2BlendMode source=GX2_BLEND_MODE_SRC_ALPHA;
    GX2BlendMode destination=GX2_BLEND_MODE_INV_SRC_ALPHA;
    GX2BlendCombineMode combine=GX2_BLEND_COMBINE_MODE_ADD;
    if (mode==BlendMode::Additive) destination=GX2_BLEND_MODE_ONE;
    else if (mode==BlendMode::Multiply) {
        source=GX2_BLEND_MODE_DST_COLOR;
        destination=GX2_BLEND_MODE_INV_SRC_ALPHA;
    } else if (mode==BlendMode::Screen) {
        source=GX2_BLEND_MODE_ONE;
        destination=GX2_BLEND_MODE_INV_SRC_COLOR;
    } else if (mode==BlendMode::Subtract) {
        destination=GX2_BLEND_MODE_ONE;
        combine=GX2_BLEND_COMBINE_MODE_REV_SUB;
    }
    GX2SetBlendControl(GX2_RENDER_TARGET_0,source,destination,combine,TRUE,
                       GX2_BLEND_MODE_SRC_ALPHA,GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD);
}

bool WiiURenderer::prepare2D(Frame2D& frame,MeshBuffers& buffers,
                              std::size_t& bufferRevision,Display) {
    const auto start=profileStamp();
    std::unordered_set<UploadSurface*> used;
    for (const Draw2DCommand& command:frame.commands)
        if (command.softwareLayer && frame.layers[command.layer].upload)
            used.insert(frame.layers[command.layer].upload);
    for (Draw2DCommand& command:frame.commands) {
        if (!command.softwareLayer) continue;
        SoftwareLayer& layer=frame.layers[command.layer];
        if (layer.upload) continue;
        UploadSurface* selected=nullptr;
        std::size_t bestArea=std::numeric_limits<std::size_t>::max();
        for (const auto& candidate:frame.uploadPool) {
            if (used.contains(candidate.get()) || candidate->width<layer.width ||
                candidate->height<layer.height) continue;
            const std::size_t area=static_cast<std::size_t>(candidate->width)*candidate->height;
            if (area<bestArea) { selected=candidate.get();bestArea=area; }
        }
        if (!selected) {
            auto surface=std::make_unique<UploadSurface>();
            const int allocatedWidth=std::min(width_,(layer.width+31)&~31);
            const int allocatedHeight=std::min(height_,(layer.height+31)&~31);
            try { createUploadSurface(*surface,allocatedWidth,allocatedHeight,false); }
            catch (...) { destroyUploadSurface(*surface); return false; }
            selected=surface.get();
            frame.uploadPool.push_back(std::move(surface));
        }
        if (!uploadPixels(*selected,layer.pixels.data(),layer.width,layer.height))
            return false;
        layer.upload=selected;
        used.insert(selected);
        const float uMax=static_cast<float>(layer.width)/selected->width;
        const float vMax=static_cast<float>(layer.height)/selected->height;
        const float u[6]{0,uMax,uMax,0,uMax,0};
        const float v[6]{0,0,vMax,0,vMax,vMax};
        for (std::uint32_t i=0;i<6;++i) {
            frame.textureCoordinates[static_cast<std::size_t>(command.first+i)*2]=u[i];
            frame.textureCoordinates[static_cast<std::size_t>(command.first+i)*2+1]=v[i];
        }
    }
    if (bufferRevision!=frame.revision && !frame.positions.empty()) {
        if (!uploadAttributeBuffer(buffers.positions,frame.positions,3) ||
            !uploadAttributeBuffer(buffers.textureCoordinates,frame.textureCoordinates,2) ||
            !uploadAttributeBuffer(buffers.colors,frame.colors,4)) return false;
        frameStats_.uploadedBytes += (frame.positions.size()+
            frame.textureCoordinates.size()+frame.colors.size())*sizeof(float);
        bufferRevision=frame.revision;
    }
    frameStats_.uploadMs += profileElapsedMs(start);
    return true;
}

void WiiURenderer::draw2D(Frame2D& frame,MeshBuffers& buffers,Display display) {
    if (frame.commands.empty()) return;
    const Rect& target=viewport(display);
    GX2SetViewport(static_cast<float>(target.x),static_cast<float>(target.y),
                   static_cast<float>(target.width),static_cast<float>(target.height),
                   0.0f,1.0f);
    GX2SetScissor(static_cast<std::uint32_t>(target.x),
                  static_cast<std::uint32_t>(target.y),
                  static_cast<std::uint32_t>(target.width),
                  static_cast<std::uint32_t>(target.height));
    GX2SetAlphaTest(FALSE,GX2_COMPARE_FUNC_ALWAYS,0.0f);
    GX2SetDepthOnlyControl(FALSE,FALSE,GX2_COMPARE_FUNC_ALWAYS);
    GX2SetColorControl(GX2_LOGIC_OP_COPY,0xff,FALSE,TRUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW,FALSE,FALSE);
    GX2SetFetchShader(&shader_.fetchShader);
    GX2SetVertexShader(shader_.vertexShader);
    GX2SetPixelShader(shader_.pixelShader);
    GX2RSetAttributeBuffer(&buffers.positions,0,buffers.positions.elemSize,0);
    GX2RSetAttributeBuffer(&buffers.textureCoordinates,1,
                           buffers.textureCoordinates.elemSize,0);
    GX2RSetAttributeBuffer(&buffers.colors,2,buffers.colors.elemSize,0);
    GX2SetPixelSampler(&sampler_,0);
    const GX2Texture* lastTexture=nullptr;
    BlendMode lastBlend=BlendMode::Normal;
    bool blendValid=false;
    for (const Draw2DCommand& command:frame.commands) {
        const UploadSurface* surface=command.softwareLayer
            ? frame.layers[command.layer].upload
            : (command.asset?imageTexture(command.asset):&whiteTexture_);
        if (!surface) continue;
        if (&surface->texture!=lastTexture) {
            GX2SetPixelTexture(&surface->texture,0);
            lastTexture=&surface->texture;
        }
        if (!blendValid || command.blend!=lastBlend) {
            setBlendMode(command.blend);
            lastBlend=command.blend;
            blendValid=true;
        }
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES,command.count,command.first,1);
        ++frameStats_.drawCalls;
    }
    // 3D mesh positions are already in output-space NDC.
    GX2SetViewport(0,0,static_cast<float>(outputWidth(display)),
                   static_cast<float>(outputHeight(display)),0.0f,1.0f);
    GX2SetScissor(0,0,static_cast<std::uint32_t>(outputWidth(display)),
                  static_cast<std::uint32_t>(outputHeight(display)));
}

void WiiURenderer::present() {
    if (gpu2DEnabled_) {
        if (startupTrace_) startupTrace_("prepare-tv");
        const std::size_t slot=presentCounter_%tv2DBuffers_.size();
        if (!prepare2D(tv2D_,tv2DBuffers_[slot],tv2DBufferRevision_[slot],Display::Tv))
            throw std::runtime_error("Could not prepare TV GX2 frame");
        Frame2D& gamePadFrame=mirrorDisplays_?tv2D_:gamePad2D_;
        if (startupTrace_) startupTrace_("prepare-gamepad");
        if (!prepare2D(gamePadFrame,gamePad2DBuffers_[slot],gamePad2DBufferRevision_[slot],
                       Display::GamePad))
            throw std::runtime_error("Could not prepare GamePad GX2 frame");
        frameStats_.sprites=tv2D_.spriteCount+gamePadFrame.spriteCount;
        const auto presentStart=std::chrono::steady_clock::now();
        if (startupTrace_) startupTrace_("gx2-begin-render");
        WHBGfxBeginRender();
        if (startupTrace_) startupTrace_("gx2-tv");
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(tv2D_.clear.r/255.0f,tv2D_.clear.g/255.0f,
                         tv2D_.clear.b/255.0f,tv2D_.clear.a/255.0f);
        draw2D(tv2D_,tv2DBuffers_[slot],Display::Tv);
        drawMeshes(tvMeshes_,tvMeshBuffers_,Display::Tv);
        WHBGfxFinishRenderTV();
        if (startupTrace_) startupTrace_("gx2-gamepad");
        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(gamePadFrame.clear.r/255.0f,gamePadFrame.clear.g/255.0f,
                         gamePadFrame.clear.b/255.0f,gamePadFrame.clear.a/255.0f);
        draw2D(gamePadFrame,gamePad2DBuffers_[slot],Display::GamePad);
        drawMeshes(gamePadMeshes_,gamePadMeshBuffers_,Display::GamePad);
        WHBGfxFinishRenderDRC();
        if (startupTrace_) startupTrace_("gx2-finish-render");
        WHBGfxFinishRender();
        ++presentCounter_;
        const auto end=std::chrono::steady_clock::now();
        frameStats_.presentMs=std::chrono::duration<double,std::milli>(end-presentStart).count();
        frameStats_.cpuFrameMs=std::chrono::duration<double,std::milli>(end-frameStart_).count();
        if (previousPresent_.time_since_epoch().count()!=0) {
            frameStats_.frameMs=std::chrono::duration<double,std::milli>(end-previousPresent_).count();
            if (frameStats_.frameMs>0) frameStats_.fps=1000.0/frameStats_.frameMs;
        }
        previousPresent_=end;
        lastStats_=frameStats_;
        return;
    }
    if (tvPixelsDirty_) {
        if (!upload(tvUpload_, tvPixels_))
            throw std::runtime_error("Could not upload TV GX2 frame");
        tvPixelsDirty_ = false;
    }
    if (!mirrorDisplays_ && gamePadPixelsDirty_) {
        if (!upload(gamePadUpload_, gamePadPixels_))
            throw std::runtime_error("Could not upload GamePad GX2 frame");
        gamePadPixelsDirty_ = false;
    }

    WHBGfxBeginRender();

    WHBGfxBeginRenderTV();
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    drawUpload(tvUpload_, tvPositionBuffer_);
    drawMeshes(tvMeshes_, tvMeshBuffers_, Display::Tv);
    WHBGfxFinishRenderTV();

    WHBGfxBeginRenderDRC();
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    drawUpload(mirrorDisplays_ ? tvUpload_ : gamePadUpload_, gamePadPositionBuffer_);
    drawMeshes(gamePadMeshes_, gamePadMeshBuffers_, Display::GamePad);
    WHBGfxFinishRenderDRC();

    WHBGfxFinishRender();
    const auto end=std::chrono::steady_clock::now();
    frameStats_.cpuFrameMs=std::chrono::duration<double,std::milli>(end-frameStart_).count();
    if (previousPresent_.time_since_epoch().count()!=0) {
        frameStats_.frameMs=std::chrono::duration<double,std::milli>(end-previousPresent_).count();
        if (frameStats_.frameMs>0) frameStats_.fps=1000.0/frameStats_.frameMs;
    }
    previousPresent_=end;
    lastStats_=frameStats_;
}

} // namespace cafe

#endif
