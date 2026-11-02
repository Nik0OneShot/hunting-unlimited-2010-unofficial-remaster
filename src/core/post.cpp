#include "longshot.h"

#include <cstdlib>

namespace longshot {

namespace {

constexpr unsigned kTexture2d = 0x0DE1;
constexpr unsigned kRgba = 0x1908;
constexpr unsigned kRgba8 = 0x8058;
constexpr unsigned kUnsignedByte = 0x1401;
constexpr unsigned kLinear = 0x2601;
constexpr unsigned kClampToEdge = 0x812F;
constexpr unsigned kTextureMagFilter = 0x2800;
constexpr unsigned kTextureMinFilter = 0x2801;
constexpr unsigned kTextureWrapS = 0x2802;
constexpr unsigned kTextureWrapT = 0x2803;
constexpr unsigned kTexture0 = 0x84C0;
constexpr unsigned kTextureBinding2d = 0x8069;
constexpr unsigned kActiveTexture = 0x84E0;
constexpr unsigned kAllAttribBits = 0xFFFFFFFF;
constexpr unsigned kViewport = 0x0BA2;
constexpr unsigned kBlend = 0x0BE2;
constexpr unsigned kDepthWritemask = 0x0B72;
constexpr unsigned kTriangles = 0x0004;
constexpr unsigned kBack = 0x0405;
constexpr unsigned kVertexShader = 0x8B31;
constexpr unsigned kFragmentShader = 0x8B30;
constexpr unsigned kCompileStatus = 0x8B81;
constexpr unsigned kLinkStatus = 0x8B82;
constexpr unsigned kCurrentProgram = 0x8B8D;
constexpr unsigned kFramebuffer = 0x8D40;
constexpr unsigned kFramebufferBinding = 0x8CA6;
constexpr unsigned kColorAttachment0 = 0x8CE0;
constexpr unsigned kFramebufferComplete = 0x8CD5;
constexpr unsigned kColorBufferBit = 0x4000;
constexpr unsigned kDepthBufferBit = 0x0100;
constexpr unsigned kDepthComponent = 0x1902;
constexpr unsigned kDepthComponent24 = 0x81A6;
constexpr unsigned kFloat = 0x1406;

// Everything that could get in the way of a full-screen pass. glPopAttrib puts these back.
constexpr unsigned kStatesOff[] = {
    0x0B71,  // depth test
    0x0B90,  // stencil test
    0x0C11,  // scissor test
    kBlend,
    0x0BC0,  // alpha test
    0x0B44,  // cull face
    0x0B60,  // fog
    0x0B50,  // lighting
    0x809E,  // sample alpha to coverage
    0x8620,  // ARB vertex programs
    0x8804,  // ARB fragment programs
};

constexpr int kBloomScale = 4;

const char kVertexSource[] =
    "#version 120\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    uv = gl_Vertex.xy * 0.5 + 0.5;\n"
    "    gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);\n"
    "}\n";

const char kBrightSource[] =
    "#version 120\n"
    "uniform sampler2D scene;\n"
    "uniform vec2 pixel;\n"
    "varying vec2 uv;\n"
    "vec3 bright(vec2 at) {\n"
    "    vec3 c = texture2D(scene, at).rgb;\n"
    "    return c * smoothstep(0.7, 1.0, max(c.r, max(c.g, c.b)));\n"
    "}\n"
    "void main() {\n"
    "    vec3 sum = bright(uv + pixel * vec2(-1.0, -1.0)) + bright(uv + pixel * vec2(1.0, -1.0))\n"
    "             + bright(uv + pixel * vec2(-1.0, 1.0)) + bright(uv + pixel * vec2(1.0, 1.0));\n"
    "    gl_FragColor = vec4(sum * 0.25, 1.0);\n"
    "}\n";

const char kBlurSource[] =
    "#version 120\n"
    "uniform sampler2D source;\n"
    "uniform vec2 direction;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec3 sum = texture2D(source, uv).rgb * 0.227;\n"
    "    sum += (texture2D(source, uv + direction * 1.385).rgb\n"
    "          + texture2D(source, uv - direction * 1.385).rgb) * 0.316;\n"
    "    sum += (texture2D(source, uv + direction * 3.231).rgb\n"
    "          + texture2D(source, uv - direction * 3.231).rgb) * 0.070;\n"
    "    gl_FragColor = vec4(sum, 1.0);\n"
    "}\n";

// Sharpening follows AMD's contrast adaptive sharpening, which holds back on edges that are
// already strong, so it doesn't leave halos around them.
const char kFinalSource[] =
    "#version 120\n"
    "uniform sampler2D scene;\n"
    "uniform sampler2D bloom;\n"
    "uniform vec2 pixel;\n"
    "uniform float sharpening;\n"
    "uniform float bloom_amount;\n"
    "uniform float vibrance;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec3 c = texture2D(scene, uv).rgb;\n"
    "    if (sharpening > 0.0) {\n"
    "        vec3 n = texture2D(scene, uv + vec2(0.0, pixel.y)).rgb;\n"
    "        vec3 s = texture2D(scene, uv - vec2(0.0, pixel.y)).rgb;\n"
    "        vec3 e = texture2D(scene, uv + vec2(pixel.x, 0.0)).rgb;\n"
    "        vec3 w = texture2D(scene, uv - vec2(pixel.x, 0.0)).rgb;\n"
    "        vec3 low = min(c, min(n, min(s, min(e, w))));\n"
    "        vec3 high = max(c, max(n, max(s, max(e, w))));\n"
    "        vec3 amount = sqrt(clamp(min(low, 1.0 - high) / max(high, vec3(0.001)), 0.0, 1.0));\n"
    "        vec3 weight = amount * (-1.0 / mix(8.0, 5.0, sharpening));\n"
    "        c = clamp(((n + s + e + w) * weight + c) / (1.0 + 4.0 * weight), 0.0, 1.0);\n"
    "    }\n"
    "    if (bloom_amount > 0.0) {\n"
    "        c += texture2D(bloom, uv).rgb * bloom_amount * (1.0 - c);\n"
    "    }\n"
    "    if (vibrance > 0.0) {\n"
    "        float grey = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "        float colourful = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));\n"
    "        c = mix(vec3(grey), c, 1.0 + vibrance * (1.0 - colourful));\n"
    "        c = (c - 0.5) * (1.0 + vibrance * 0.25) + 0.5;\n"
    "    }\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

// A flashlight or lantern, as a pass over the finished picture. The depth of the world says how
// far each pixel is, so this works on anything drawn, with no changes to the game's materials.
// A beam is a cone from the middle of the view, and a lantern a ball around it.
const char kLightSource[] =
    "#version 120\n"
    "uniform sampler2D scene;\n"
    "uniform sampler2D depth;\n"
    "uniform vec2 projection;\n"
    "uniform vec2 view_scale;\n"
    "uniform vec3 lift;\n"
    "uniform float beam;\n"
    "uniform float glow;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec3 c = texture2D(scene, uv).rgb;\n"
    "    float d = texture2D(depth, uv).r;\n"
    "    vec3 light = vec3(0.0);\n"
    "    if (d < 0.99999) {\n"
    "        float away = projection.y / (d * 2.0 - 1.0 + projection.x);\n"
    "        float angle = length((uv * 2.0 - 1.0) * view_scale);\n"
    "        float reach = away * sqrt(1.0 + angle * angle);\n"
    "        float cone = 1.0 - smoothstep(0.1, 0.42, angle);\n"
    "        float thrown = 1.6 / (1.0 + reach * reach / 500.0) * (1.0 - smoothstep(60.0, 110.0, reach));\n"
    "        float around = 1.8 / (1.0 + reach * reach / 36.0) * (1.0 - smoothstep(14.0, 26.0, reach));\n"
    "        light = beam * cone * thrown * vec3(1.0, 1.0, 0.95) + glow * around * vec3(1.0, 0.86, 0.62);\n"
    "    }\n"
    "    gl_FragColor = vec4(c * (vec3(1.0) + (lift - vec3(1.0)) * min(light, vec3(1.0))), 1.0);\n"
    "}\n";

struct Gl {
    void(WINAPI* GenTextures)(int, unsigned*);
    void(WINAPI* BindTexture)(unsigned, unsigned);
    void(WINAPI* TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void(WINAPI* TexParameteri)(unsigned, unsigned, int);
    void(WINAPI* CopyTexSubImage2D)(unsigned, int, int, int, int, int, int, int);
    void(WINAPI* Viewport)(int, int, int, int);
    void(WINAPI* Enable)(unsigned);
    void(WINAPI* Disable)(unsigned);
    void(WINAPI* GetIntegerv)(unsigned, int*);
    void(WINAPI* Begin)(unsigned);
    void(WINAPI* End)();
    void(WINAPI* Vertex2f)(float, float);
    void(WINAPI* PushAttrib)(unsigned);
    void(WINAPI* PopAttrib)();
    void(WINAPI* DepthMask)(unsigned char);
    void(WINAPI* ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void(WINAPI* ReadBuffer)(unsigned);
    void(WINAPI* DrawBuffer)(unsigned);
    void(WINAPI* ReadPixels)(int, int, int, int, unsigned, unsigned, void*);
    void(WINAPI* ClearColor)(float, float, float, float);
    void(WINAPI* ClearDepth)(double);
    void(WINAPI* Clear)(unsigned);
    void(WINAPI* Scissor)(int, int, int, int);
    void(APIENTRY* ActiveTexture)(unsigned);
    unsigned(APIENTRY* CreateShader)(unsigned);
    void(APIENTRY* ShaderSource)(unsigned, int, const char* const*, const int*);
    void(APIENTRY* CompileShader)(unsigned);
    void(APIENTRY* GetShaderiv)(unsigned, unsigned, int*);
    void(APIENTRY* GetShaderInfoLog)(unsigned, int, int*, char*);
    unsigned(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(unsigned, unsigned);
    void(APIENTRY* LinkProgram)(unsigned);
    void(APIENTRY* GetProgramiv)(unsigned, unsigned, int*);
    void(APIENTRY* UseProgram)(unsigned);
    int(APIENTRY* GetUniformLocation)(unsigned, const char*);
    void(APIENTRY* Uniform1i)(int, int);
    void(APIENTRY* Uniform1f)(int, float);
    void(APIENTRY* Uniform3f)(int, float, float, float);
    void(APIENTRY* Uniform2f)(int, float, float);
    void(APIENTRY* GenFramebuffers)(int, unsigned*);
    void(APIENTRY* BindFramebuffer)(unsigned, unsigned);
    void(APIENTRY* FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned(APIENTRY* CheckFramebufferStatus)(unsigned);
};

Gl g_gl;
void* g_context = nullptr;
bool g_ready = false;
bool g_failed = false;
unsigned g_bright = 0, g_blur = 0, g_final = 0, g_light = 0;
unsigned g_scene_texture = 0, g_depth_texture = 0, g_bloom_textures[2] = {};
bool g_have_depth = false;
float g_view_scale[2] = {};
float g_depth_a = 0.0f, g_depth_b = 0.0f;
unsigned g_bloom_buffers[2] = {};
int g_width = 0, g_height = 0;

using WglCurrentContextFn = void*(WINAPI*)();
using WglGetProcFn = PROC(WINAPI*)(const char*);

template <typename T>
bool load_core(T* out, HMODULE opengl, const char* name) {
    *out = reinterpret_cast<T>(GetProcAddress(opengl, name));
    return *out != nullptr;
}

template <typename T>
bool load_extension(T* out, WglGetProcFn get, const char* name) {
    *out = reinterpret_cast<T>(get(name));
    return *out != nullptr;
}

bool load_functions() {
    HMODULE opengl = LoadLibraryW(L"opengl32.dll");
    if (!opengl) return false;
    auto get = reinterpret_cast<WglGetProcFn>(GetProcAddress(opengl, "wglGetProcAddress"));
    if (!get) return false;

    Gl& gl = g_gl;
    return load_core(&gl.GenTextures, opengl, "glGenTextures") &&
           load_core(&gl.BindTexture, opengl, "glBindTexture") &&
           load_core(&gl.TexImage2D, opengl, "glTexImage2D") &&
           load_core(&gl.TexParameteri, opengl, "glTexParameteri") &&
           load_core(&gl.CopyTexSubImage2D, opengl, "glCopyTexSubImage2D") &&
           load_core(&gl.Viewport, opengl, "glViewport") &&
           load_core(&gl.Enable, opengl, "glEnable") &&
           load_core(&gl.Disable, opengl, "glDisable") &&
           load_core(&gl.GetIntegerv, opengl, "glGetIntegerv") &&
           load_core(&gl.Begin, opengl, "glBegin") &&
           load_core(&gl.End, opengl, "glEnd") &&
           load_core(&gl.Vertex2f, opengl, "glVertex2f") &&
           load_core(&gl.PushAttrib, opengl, "glPushAttrib") &&
           load_core(&gl.PopAttrib, opengl, "glPopAttrib") &&
           load_core(&gl.DepthMask, opengl, "glDepthMask") &&
           load_core(&gl.ColorMask, opengl, "glColorMask") &&
           load_core(&gl.ReadBuffer, opengl, "glReadBuffer") &&
           load_core(&gl.DrawBuffer, opengl, "glDrawBuffer") &&
           load_core(&gl.ReadPixels, opengl, "glReadPixels") &&
           load_core(&gl.ClearColor, opengl, "glClearColor") &&
           load_core(&gl.ClearDepth, opengl, "glClearDepth") &&
           load_core(&gl.Clear, opengl, "glClear") &&
           load_core(&gl.Scissor, opengl, "glScissor") &&
           load_extension(&gl.ActiveTexture, get, "glActiveTexture") &&
           load_extension(&gl.CreateShader, get, "glCreateShader") &&
           load_extension(&gl.ShaderSource, get, "glShaderSource") &&
           load_extension(&gl.CompileShader, get, "glCompileShader") &&
           load_extension(&gl.GetShaderiv, get, "glGetShaderiv") &&
           load_extension(&gl.GetShaderInfoLog, get, "glGetShaderInfoLog") &&
           load_extension(&gl.CreateProgram, get, "glCreateProgram") &&
           load_extension(&gl.AttachShader, get, "glAttachShader") &&
           load_extension(&gl.LinkProgram, get, "glLinkProgram") &&
           load_extension(&gl.GetProgramiv, get, "glGetProgramiv") &&
           load_extension(&gl.UseProgram, get, "glUseProgram") &&
           load_extension(&gl.GetUniformLocation, get, "glGetUniformLocation") &&
           load_extension(&gl.Uniform1i, get, "glUniform1i") &&
           load_extension(&gl.Uniform1f, get, "glUniform1f") &&
           load_extension(&gl.Uniform3f, get, "glUniform3f") &&
           load_extension(&gl.Uniform2f, get, "glUniform2f") &&
           load_extension(&gl.GenFramebuffers, get, "glGenFramebuffers") &&
           load_extension(&gl.BindFramebuffer, get, "glBindFramebuffer") &&
           load_extension(&gl.FramebufferTexture2D, get, "glFramebufferTexture2D") &&
           load_extension(&gl.CheckFramebufferStatus, get, "glCheckFramebufferStatus");
}

unsigned compile(unsigned type, const char* source) {
    const unsigned shader = g_gl.CreateShader(type);
    g_gl.ShaderSource(shader, 1, &source, nullptr);
    g_gl.CompileShader(shader);
    int ok = 0;
    g_gl.GetShaderiv(shader, kCompileStatus, &ok);
    if (ok) return shader;

    char text[512] = {};
    g_gl.GetShaderInfoLog(shader, sizeof(text), nullptr, text);
    logf("post: shader failed to compile: %s", text);
    return 0;
}

unsigned link(const char* fragment_source) {
    const unsigned vertex = compile(kVertexShader, kVertexSource);
    const unsigned fragment = compile(kFragmentShader, fragment_source);
    if (!vertex || !fragment) return 0;
    const unsigned program = g_gl.CreateProgram();
    g_gl.AttachShader(program, vertex);
    g_gl.AttachShader(program, fragment);
    g_gl.LinkProgram(program);
    int ok = 0;
    g_gl.GetProgramiv(program, kLinkStatus, &ok);
    return ok ? program : 0;
}

void set_uniform(unsigned program, const char* name, int value) {
    g_gl.Uniform1i(g_gl.GetUniformLocation(program, name), value);
}

void set_uniform(unsigned program, const char* name, float value) {
    g_gl.Uniform1f(g_gl.GetUniformLocation(program, name), value);
}

void set_uniform(unsigned program, const char* name, float x, float y, float z) {
    g_gl.Uniform3f(g_gl.GetUniformLocation(program, name), x, y, z);
}

void set_uniform(unsigned program, const char* name, float x, float y) {
    g_gl.Uniform2f(g_gl.GetUniformLocation(program, name), x, y);
}

void make_texture(unsigned texture, int width, int height) {
    g_gl.BindTexture(kTexture2d, texture);
    g_gl.TexParameteri(kTexture2d, kTextureMinFilter, kLinear);
    g_gl.TexParameteri(kTexture2d, kTextureMagFilter, kLinear);
    g_gl.TexParameteri(kTexture2d, kTextureWrapS, kClampToEdge);
    g_gl.TexParameteri(kTexture2d, kTextureWrapT, kClampToEdge);
    g_gl.TexImage2D(kTexture2d, 0, kRgba8, width, height, 0, kRgba, kUnsignedByte, nullptr);
}

bool make_targets(int width, int height) {
    if (!g_scene_texture) {
        g_gl.GenTextures(1, &g_scene_texture);
        g_gl.GenTextures(1, &g_depth_texture);
        g_gl.GenTextures(2, g_bloom_textures);
        g_gl.GenFramebuffers(2, g_bloom_buffers);
    }
    make_texture(g_scene_texture, width, height);
    g_gl.BindTexture(kTexture2d, g_depth_texture);
    g_gl.TexParameteri(kTexture2d, kTextureMagFilter, kLinear);
    g_gl.TexParameteri(kTexture2d, kTextureMinFilter, kLinear);
    g_gl.TexParameteri(kTexture2d, kTextureWrapS, kClampToEdge);
    g_gl.TexParameteri(kTexture2d, kTextureWrapT, kClampToEdge);
    g_gl.TexImage2D(kTexture2d, 0, kDepthComponent24, width, height, 0, kDepthComponent, kFloat, nullptr);
    bool complete = true;
    for (int i = 0; i < 2; ++i) {
        make_texture(g_bloom_textures[i], width / kBloomScale, height / kBloomScale);
        g_gl.BindFramebuffer(kFramebuffer, g_bloom_buffers[i]);
        g_gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2d, g_bloom_textures[i], 0);
        complete = complete && g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    }
    g_width = width;
    g_height = height;
    return complete;
}

// The game can restart its video, and a new context starts with none of these objects.
bool ready() {
    static const auto current = reinterpret_cast<WglCurrentContextFn>(
        GetProcAddress(LoadLibraryW(L"opengl32.dll"), "wglGetCurrentContext"));
    void* context = current ? current() : nullptr;
    if (!context) return false;
    if (context != g_context) {
        g_context = context;
        g_ready = false;
        g_failed = false;
        g_scene_texture = 0;
        g_depth_texture = 0;
        g_have_depth = false;
        g_width = g_height = 0;
    }
    if (g_ready) return true;
    if (g_failed) return false;

    g_failed = true;
    if (!load_functions()) {
        logf("post: this driver lacks what post-processing needs");
        return false;
    }
    g_bright = link(kBrightSource);
    g_blur = link(kBlurSource);
    g_final = link(kFinalSource);
    g_light = link(kLightSource);
    if (!g_bright || !g_blur || !g_final || !g_light) return false;

    g_failed = false;
    g_ready = true;
    logf("post: ready");
    return true;
}

void draw_pass() {
    g_gl.Begin(kTriangles);
    g_gl.Vertex2f(-1.0f, -1.0f);
    g_gl.Vertex2f(3.0f, -1.0f);
    g_gl.Vertex2f(-1.0f, 3.0f);
    g_gl.End();
}

void bloom_pass(unsigned buffer, unsigned program, unsigned source, float x, float y) {
    g_gl.BindFramebuffer(kFramebuffer, buffer);
    g_gl.UseProgram(program);
    g_gl.BindTexture(kTexture2d, source);
    set_uniform(program, program == g_bright ? "scene" : "source", 0);
    set_uniform(program, program == g_bright ? "pixel" : "direction", x, y);
    draw_pass();
}

// Reads the back buffer, and writes the result over it.
bool run_chain(int width, int height) {
    if ((width != g_width || height != g_height) && !make_targets(width, height)) return false;
    g_gl.BindFramebuffer(kFramebuffer, 0);

    for (const unsigned state : kStatesOff) g_gl.Disable(state);
    g_gl.DepthMask(0);
    g_gl.ColorMask(1, 1, 1, 1);
    g_gl.ReadBuffer(kBack);
    g_gl.DrawBuffer(kBack);

    // Copying from the back buffer also resolves it, when it's multisampled.
    g_gl.ActiveTexture(kTexture0);
    g_gl.BindTexture(kTexture2d, g_scene_texture);
    g_gl.CopyTexSubImage2D(kTexture2d, 0, 0, 0, 0, 0, width, height);

    const float bloom = g_cfg.bloom;
    if (bloom > 0.0f) {
        const int small_width = width / kBloomScale;
        const int small_height = height / kBloomScale;
        g_gl.Viewport(0, 0, small_width, small_height);
        bloom_pass(g_bloom_buffers[0], g_bright, g_scene_texture, 1.0f / width, 1.0f / height);
        bloom_pass(g_bloom_buffers[1], g_blur, g_bloom_textures[0], 1.0f / small_width, 0.0f);
        bloom_pass(g_bloom_buffers[0], g_blur, g_bloom_textures[1], 0.0f, 1.0f / small_height);
    }

    g_gl.BindFramebuffer(kFramebuffer, 0);
    g_gl.Viewport(0, 0, width, height);
    g_gl.UseProgram(g_final);
    g_gl.ActiveTexture(kTexture0 + 1);
    g_gl.BindTexture(kTexture2d, g_bloom_textures[0]);
    g_gl.ActiveTexture(kTexture0);
    g_gl.BindTexture(kTexture2d, g_scene_texture);
    set_uniform(g_final, "scene", 0);
    set_uniform(g_final, "bloom", 1);
    set_uniform(g_final, "pixel", 1.0f / width, 1.0f / height);
    set_uniform(g_final, "sharpening", g_cfg.sharpening);
    set_uniform(g_final, "bloom_amount", bloom);
    set_uniform(g_final, "vibrance", g_cfg.vibrance);
    draw_pass();
    return true;
}

bool any_effect() { return g_cfg.sharpening > 0.0f || g_cfg.bloom > 0.0f || g_cfg.vibrance > 0.0f; }

// Has to come straight after the world, as whatever's drawn next clears the depth.
void copy_world_depth(int width, int height, const float* projection) {
    g_gl.ActiveTexture(kTexture0);
    g_gl.BindTexture(kTexture2d, g_depth_texture);
    g_gl.ReadBuffer(kBack);
    g_gl.CopyTexSubImage2D(kTexture2d, 0, 0, 0, 0, 0, width, height);
    g_view_scale[0] = 1.0f / projection[0];
    g_view_scale[1] = 1.0f / projection[5];
    g_depth_a = projection[10];
    g_depth_b = projection[11];
    g_have_depth = true;
}

bool run_lights(int width, int height, const PostLight& light) {
    if ((width != g_width || height != g_height) && !make_targets(width, height)) return false;
    g_gl.BindFramebuffer(kFramebuffer, 0);
    for (const unsigned state : kStatesOff) g_gl.Disable(state);
    g_gl.DepthMask(0);
    g_gl.ColorMask(1, 1, 1, 1);
    g_gl.ReadBuffer(kBack);
    g_gl.DrawBuffer(kBack);
    g_gl.Viewport(0, 0, width, height);

    g_gl.ActiveTexture(kTexture0 + 1);
    g_gl.BindTexture(kTexture2d, g_depth_texture);
    g_gl.ActiveTexture(kTexture0);
    g_gl.BindTexture(kTexture2d, g_scene_texture);
    g_gl.CopyTexSubImage2D(kTexture2d, 0, 0, 0, 0, 0, width, height);

    g_gl.UseProgram(g_light);
    set_uniform(g_light, "scene", 0);
    set_uniform(g_light, "depth", 1);
    set_uniform(g_light, "projection", g_depth_a, g_depth_b);
    set_uniform(g_light, "view_scale", g_view_scale[0], g_view_scale[1]);
    set_uniform(g_light, "lift", light.lift[0], light.lift[1], light.lift[2]);
    set_uniform(g_light, "beam", light.beam);
    set_uniform(g_light, "glow", light.glow);
    draw_pass();
    return true;
}

// ---- Self-test, on the real driver --------------------------------------------------------

constexpr int kTestSize = 64;

struct Pixel {
    int r, g, b;
};

Pixel read_pixel(int x, int y) {
    unsigned char rgba[4] = {};
    g_gl.ReadPixels(x, y, 1, 1, kRgba, kUnsignedByte, rgba);
    return {rgba[0], rgba[1], rgba[2]};
}

void clear_area(int x, int width, float r, float g, float b) {
    g_gl.Enable(0x0C11);  // scissor test
    g_gl.Scissor(x, 0, width, kTestSize);
    g_gl.ClearColor(r, g, b, 1.0f);
    g_gl.Clear(kColorBufferBit);
    g_gl.Disable(0x0C11);
}

// Two halves, with an edge down the middle for the effects to act on.
void draw_test_picture(float left) {
    clear_area(0, kTestSize / 2, left, left, left);
    clear_area(kTestSize / 2, kTestSize / 2, 0.3f, 0.2f, 0.2f);
}

void set_effects(float sharpening, float bloom, float vibrance) {
    g_cfg.sharpening = sharpening;
    g_cfg.bloom = bloom;
    g_cfg.vibrance = vibrance;
}

int test_effects() {
    const auto run = [](int x, float left, float sharpening, float bloom, float vibrance) {
        draw_test_picture(left);
        set_effects(sharpening, bloom, vibrance);
        run_chain(kTestSize, kTestSize);
        return read_pixel(x, kTestSize / 2);
    };

    constexpr int kFarDark = kTestSize - 4;
    constexpr int kNextToEdge = kTestSize / 2 + 2;
    constexpr int kOnEdge = kTestSize / 2;
    // Sharpening holds back beside pure white, on purpose, so its edge has to be grey.
    constexpr float kGrey = 0.6f;
    constexpr float kWhite = 1.0f;

    const Pixel plain = run(kFarDark, kWhite, 0.0f, 0.0f, 0.0f);
    if (abs(plain.r - 77) > 2 || abs(plain.g - 51) > 2) return 3;  // nothing changes with all off

    const Pixel plain_edge = run(kOnEdge, kGrey, 0.0f, 0.0f, 0.0f);
    const Pixel sharp_edge = run(kOnEdge, kGrey, 1.0f, 0.0f, 0.0f);
    if (sharp_edge.r >= plain_edge.r - 4) return 4;  // the dark side of an edge gets darker

    const Pixel plain_near = run(kNextToEdge, kWhite, 0.0f, 0.0f, 0.0f);
    const Pixel glow = run(kNextToEdge, kWhite, 0.0f, 0.5f, 0.0f);
    if (glow.r <= plain_near.r + 4) return 5;  // the bright side glows onto the dark side

    const Pixel vivid = run(kFarDark, kWhite, 0.0f, 0.0f, 1.0f);
    if (vivid.r - vivid.g <= plain.r - plain.g) return 6;  // colours move further apart
    return 0;
}

void clear_depth_area(int x, int width, double depth) {
    g_gl.Enable(0x0C11);  // scissor test
    g_gl.Scissor(x, 0, width, kTestSize);
    g_gl.DepthMask(1);
    g_gl.ClearDepth(depth);
    g_gl.Clear(kDepthBufferBit);
    g_gl.Disable(0x0C11);
}

// Dim ground on the left and open sky on the right, seen by a light from the middle of the view.
int test_lights() {
    constexpr float kNear = 0.5f, kFar = 1000.0f;
    const float depth_a = (kNear + kFar) / (kNear - kFar);
    const float depth_b = 2.0f * kNear * kFar / (kNear - kFar);
    const float camera[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, depth_a, depth_b, 0.0f, 0.0f, -1.0f, 0.0f};
    const auto buffer_depth = [&](float away) { return ((depth_b / away - depth_a) + 1.0) * 0.5; };

    constexpr int kOnGround = kTestSize / 2 - 3;
    constexpr int kOnSky = kTestSize / 2 + 3;
    constexpr int kFarSide = 3;
    const auto lit = [&](int x, float away, float beam, float glow) {
        if ((kTestSize != g_width) && !make_targets(kTestSize, kTestSize)) return Pixel{-1, -1, -1};
        clear_area(0, kTestSize, 0.1f, 0.1f, 0.1f);
        clear_depth_area(0, kTestSize / 2, buffer_depth(away));
        clear_depth_area(kTestSize / 2, kTestSize / 2, 1.0);
        copy_world_depth(kTestSize, kTestSize, camera);
        const PostLight light = {beam, glow, {6.0f, 6.0f, 6.0f}};
        run_lights(kTestSize, kTestSize, light);
        return read_pixel(x, kTestSize / 2);
    };

    constexpr int kDim = 26;
    if (lit(kOnGround, 5.0f, 0.0f, 0.0f).r > kDim + 2) return 20;        // switched off, nothing changes
    if (lit(kOnGround, 5.0f, 1.0f, 0.0f).r < kDim * 3) return 21;         // a beam lights what's in front
    if (lit(kOnSky, 5.0f, 1.0f, 0.0f).r > kDim + 2) return 22;            // but not the sky
    if (lit(kOnGround, 500.0f, 1.0f, 0.0f).r > kDim + 2) return 23;       // or what's too far off
    if (lit(kFarSide, 5.0f, 1.0f, 0.0f).r > lit(kOnGround, 5.0f, 1.0f, 0.0f).r - 20) return 24;  // and it's a cone
    if (lit(kFarSide, 5.0f, 0.0f, 1.0f).r < kDim * 3) return 25;          // a lantern reaches the sides
    if (lit(kFarSide, 40.0f, 0.0f, 1.0f).r > kDim + 2) return 26;         // and not far
    const Pixel warm = lit(kOnGround, 5.0f, 0.0f, 1.0f);
    if (warm.b >= warm.r) return 27;                                      // and it's warm
    return 0;
}

// The game keeps track of its own state, so anything left changed here would break its drawing.
int test_state_kept() {
    unsigned texture = 0;
    g_gl.GenTextures(1, &texture);
    g_gl.ActiveTexture(kTexture0 + 2);
    g_gl.BindTexture(kTexture2d, texture);
    g_gl.UseProgram(g_blur);
    g_gl.Viewport(1, 2, 3, 4);
    g_gl.Enable(kBlend);
    g_gl.DepthMask(1);

    set_effects(1.0f, 0.5f, 0.5f);
    post_render();
    if (!g_ready) return 7;

    int viewport[4] = {}, active = 0, bound = 0, program = 0, blend = 0, depth_mask = 0;
    g_gl.GetIntegerv(kViewport, viewport);
    g_gl.GetIntegerv(kActiveTexture, &active);
    g_gl.GetIntegerv(kTextureBinding2d, &bound);
    g_gl.GetIntegerv(kCurrentProgram, &program);
    g_gl.GetIntegerv(kBlend, &blend);
    g_gl.GetIntegerv(kDepthWritemask, &depth_mask);
    g_gl.UseProgram(0);
    g_gl.Disable(kBlend);

    const bool viewport_kept =
        viewport[0] == 1 && viewport[1] == 2 && viewport[2] == 3 && viewport[3] == 4;
    if (!viewport_kept) return 8;
    if (active != static_cast<int>(kTexture0 + 2) || bound != static_cast<int>(texture)) return 9;
    if (program != static_cast<int>(g_blur)) return 10;
    if (!blend || !depth_mask) return 11;
    return 0;
}

int test_chain() {
    if (!ready()) return 1;
    g_gl.Viewport(0, 0, kTestSize, kTestSize);
    const int effects = test_effects();
    if (effects) return effects;
    const int lights = test_lights();
    return lights ? lights : test_state_kept();
}

}  // namespace

void post_capture_world(const float* projection) {
    g_have_depth = false;
    if (!lights_wanted() || !ready()) return;
    if ((g_mode.width != g_width || g_mode.height != g_height) && !make_targets(g_mode.width, g_mode.height)) return;
    ProfileTimer timer(ProfileSection::Post);
    g_gl.PushAttrib(kAllAttribBits);
    copy_world_depth(g_mode.width, g_mode.height, projection);
    g_gl.PopAttrib();
}

void post_render() {
    PostLight light = {};
    const bool lit = g_have_depth && lights_this_frame(&light);
    g_have_depth = false;
    if ((!any_effect() && !lit) || !ready()) return;
    ProfileTimer timer(ProfileSection::Post);

    int program = 0, framebuffer = 0;
    g_gl.GetIntegerv(kCurrentProgram, &program);
    g_gl.GetIntegerv(kFramebufferBinding, &framebuffer);
    g_gl.PushAttrib(kAllAttribBits);

    const bool ok = (!any_effect() || run_chain(g_mode.width, g_mode.height)) && (!lit || run_lights(g_mode.width, g_mode.height, light));

    g_gl.UseProgram(program);
    g_gl.BindFramebuffer(kFramebuffer, framebuffer);
    g_gl.PopAttrib();

    if (ok) return;
    g_ready = false;
    g_failed = true;
    logf("post: the driver refused the bloom buffers, so post-processing is off");
}

int post_test() {
    const Config saved_config = g_cfg;
    const DisplayMode saved_mode = g_mode;
    g_mode.width = g_mode.height = kTestSize;

    // The game's back buffer can be multisampled, which changes how it has to be read.
    int result = -1;
    for (const int samples : {0, 4}) {
        result = -1;
        graphics_in_context(kTestSize, samples, [&result]() { result = test_chain(); });
        if (result != 0) break;
    }

    g_cfg = saved_config;
    g_mode = saved_mode;
    g_ready = false;
    g_failed = false;
    g_context = nullptr;
    return result;
}

}  // namespace longshot
