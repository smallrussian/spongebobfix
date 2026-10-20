// grn_dump: extracts meshes and baked animation frames from the game's Granny 1.2b (.grn) files.
//
// Granny 1.x files are an undocumented, obfuscated format, so instead of parsing them we drive the
// game's own granny.dll (32-bit) exactly the way sb2.exe does:
//   open        sb2 0x40D2D0  GrannyOpenVersion("1.2b", "win32", "10-4-2000", copyright, &granny)
//                             GrannyAllowTextureFormat(granny, 0xB)
//   model       sb2 0x403810  GrannyOpenModel, GetToolCoordinateSystemForModel, RecommendTransform,
//                             TransformModel, OpenSequence
//   textures    sb2 0x403E10  GetNewTexturesLeft / LockNextNewTexture / UnlockNewTexture(&tex, user)
//   mesh        sb2 0x4039C0  LockSequenceForRendering(seq, 0x3B) -> rendering states, each one
//                             texture's worth of float3 positions / normals / UVs + u16 triangles
//   animation   sb2 0x404200  OpenAnimation, (same transform), TransformAnimation,
//                             AddControlledAnimation, AdvanceTime(absolute seconds)
// Handles (granny objects, models, sequences, animations, controls) are passed by value as 2-3 dwords.
//
// Usage: grn_dump <granny.dll> <model.grn> <out.bin> [--fps N] [anim.grn ...]
// Output (little-endian):
//   'GRD1' u32 texture_count, per texture: str material, str file   (str = u16 length + bytes)
//   u32 frame_set_count (1 + animations); per set: str name, f32 fps, u32 frame_count, frames
//   frame: u32 state_count; per state: u32 texture_index (or ~0), u32 vertex_count,
//          f32 pos[n*3], f32 normal[n*3], f32 uv[n*3], u32 triangle_count, u16 index[t*3]
//   Arrays a state does not provide are written as zeros.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct H2 { uint32_t a, b; };
struct H3 { uint32_t a, b, c; };

// LockNextRenderingState output, offsets observed in sb2 0x403A62..0x403CF1. Each array is described by
// a 0x18-byte block: +0 present, +0xC count, +0x14 pointer.
struct RenderState { uint8_t raw[0x400]; };
static uint32_t U32(const RenderState& s, int off) { uint32_t v; memcpy(&v, s.raw + off, 4); return v; }
static const void* PTR(const RenderState& s, int off) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(U32(s, off))); }
enum { RS_TEXTURE = 0x18, RS_POS = 0x2C, RS_UV = 0x74, RS_NORMAL = 0x8C, RS_TRI = 0xA4 };

struct Texture { uint8_t raw[0x100]; };

typedef int(__stdcall* OpenVersion_t)(const char*, const char*, const char*, const char*, uint32_t*);
typedef int(__stdcall* AllowTextureFormat_t)(uint32_t, int);
typedef int(__stdcall* OpenModel_t)(uint32_t, const char*, H2*);
typedef int(__stdcall* GetToolCS_t)(uint32_t, uint32_t, float*, float*);
typedef int(__stdcall* RecommendTransform_t)(uint32_t, const float*, const float*, const float*, const float*, float*, float*);
typedef int(__stdcall* TransformObj_t)(uint32_t, uint32_t, const float*, const float*);
typedef int(__stdcall* OpenSequence_t)(uint32_t, uint32_t, H2*);
typedef int(__stdcall* GetNewTexturesLeft_t)(uint32_t, uint32_t*);
typedef int(__stdcall* LockNextNewTexture_t)(uint32_t, Texture*);
typedef int(__stdcall* UnlockNewTexture_t)(Texture*, uint32_t);
typedef int(__stdcall* LockSequenceForRendering_t)(uint32_t, uint32_t, uint32_t, H2*);
typedef int(__stdcall* GetRenderingStatesLeft_t)(uint32_t, uint32_t, uint32_t*);
typedef int(__stdcall* LockNextRenderingState_t)(uint32_t, uint32_t, RenderState*);
typedef int(__stdcall* UnlockRenderingState_t)(RenderState*);
typedef int(__stdcall* UnlockRendering_t)(uint32_t, uint32_t);
typedef int(__stdcall* OpenAnimation_t)(uint32_t, const char*, H2*);
typedef int(__stdcall* AddControlledAnimation_t)(uint32_t, uint32_t, uint32_t, uint32_t, H3*);
typedef int(__stdcall* RemoveControlledAnimation_t)(uint32_t, uint32_t, uint32_t);
typedef int(__stdcall* SetAnimationParameter_t)(uint32_t, uint32_t, uint32_t, int, double);
typedef int(__stdcall* HasControlExpired_t)(uint32_t, uint32_t, uint32_t, int*);
typedef int(__stdcall* AdvanceTime_t)(uint32_t, double);
typedef int(__stdcall* CloseAnimation_t)(uint32_t, uint32_t);
typedef int(__stdcall* GetAnimationParameter_t)(uint32_t, uint32_t, uint32_t, int, double*);
typedef int(__stdcall* Close_t)(uint32_t);

static HMODULE g_dll;
template <class T> static T Fn(const char* name) {
  auto p = reinterpret_cast<T>(GetProcAddress(g_dll, name));
  if (!p) { fprintf(stderr, "granny.dll has no %s\n", name); exit(2); }
  return p;
}

static FILE* g_out;
static void W(const void* p, size_t n) { fwrite(p, 1, n, g_out); }
static void W32(uint32_t v) { W(&v, 4); }
static void WF(float v) { W(&v, 4); }
static void WS(const char* s) {
  uint16_t n = s ? static_cast<uint16_t>(strlen(s)) : 0;
  W(&n, 2);
  if (n) W(s, n);
}

struct Api {
  OpenVersion_t OpenVersion;
  AllowTextureFormat_t AllowTextureFormat;
  OpenModel_t OpenModel;
  GetToolCS_t GetToolCoordinateSystemForModel, GetToolCoordinateSystemForAnimation;
  RecommendTransform_t RecommendTransform;
  TransformObj_t TransformModel, TransformAnimation;
  OpenSequence_t OpenSequence;
  GetNewTexturesLeft_t GetNewTexturesLeft;
  LockNextNewTexture_t LockNextNewTexture;
  UnlockNewTexture_t UnlockNewTexture;
  LockSequenceForRendering_t LockSequenceForRendering;
  GetRenderingStatesLeft_t GetRenderingStatesLeft;
  LockNextRenderingState_t LockNextRenderingState;
  UnlockRenderingState_t UnlockRenderingState;
  UnlockRendering_t UnlockRendering;
  OpenAnimation_t OpenAnimation;
  AddControlledAnimation_t AddControlledAnimation;
  RemoveControlledAnimation_t RemoveControlledAnimation;
  SetAnimationParameter_t SetAnimationParameter;
  HasControlExpired_t HasControlExpired;
  AdvanceTime_t AdvanceTime;
  CloseAnimation_t CloseAnimation, CloseModel, CloseSequence;
  GetAnimationParameter_t GetAnimationParameter;
  Close_t Close;
} G;

static void LoadApi() {
  G.OpenVersion = Fn<OpenVersion_t>("_GrannyOpenVersion@20");
  G.AllowTextureFormat = Fn<AllowTextureFormat_t>("_GrannyAllowTextureFormat@8");
  G.OpenModel = Fn<OpenModel_t>("_GrannyOpenModel@12");
  G.GetToolCoordinateSystemForModel = Fn<GetToolCS_t>("_GrannyGetToolCoordinateSystemForModel@16");
  G.GetToolCoordinateSystemForAnimation = Fn<GetToolCS_t>("_GrannyGetToolCoordinateSystemForAnimation@16");
  G.RecommendTransform = Fn<RecommendTransform_t>("_GrannyRecommendTransform@28");
  G.TransformModel = Fn<TransformObj_t>("_GrannyTransformModel@16");
  G.TransformAnimation = Fn<TransformObj_t>("_GrannyTransformAnimation@16");
  G.OpenSequence = Fn<OpenSequence_t>("_GrannyOpenSequence@12");
  G.GetNewTexturesLeft = Fn<GetNewTexturesLeft_t>("_GrannyGetNewTexturesLeft@8");
  G.LockNextNewTexture = Fn<LockNextNewTexture_t>("_GrannyLockNextNewTexture@8");
  G.UnlockNewTexture = Fn<UnlockNewTexture_t>("_GrannyUnlockNewTexture@8");
  G.LockSequenceForRendering = Fn<LockSequenceForRendering_t>("_GrannyLockSequenceForRendering@16");
  G.GetRenderingStatesLeft = Fn<GetRenderingStatesLeft_t>("_GrannyGetRenderingStatesLeft@12");
  G.LockNextRenderingState = Fn<LockNextRenderingState_t>("_GrannyLockNextRenderingState@12");
  G.UnlockRenderingState = Fn<UnlockRenderingState_t>("_GrannyUnlockRenderingState@4");
  G.UnlockRendering = Fn<UnlockRendering_t>("_GrannyUnlockRendering@8");
  G.OpenAnimation = Fn<OpenAnimation_t>("_GrannyOpenAnimation@12");
  G.AddControlledAnimation = Fn<AddControlledAnimation_t>("_GrannyAddControlledAnimation@20");
  G.RemoveControlledAnimation = Fn<RemoveControlledAnimation_t>("_GrannyRemoveControlledAnimation@12");
  G.SetAnimationParameter = Fn<SetAnimationParameter_t>("_GrannySetAnimationParameter@24");
  G.HasControlExpired = Fn<HasControlExpired_t>("_GrannyHasControlExpired@16");
  G.AdvanceTime = Fn<AdvanceTime_t>("_GrannyAdvanceTime@12");
  G.CloseAnimation = Fn<CloseAnimation_t>("_GrannyCloseAnimation@8");
  G.CloseModel = Fn<CloseAnimation_t>("_GrannyCloseModel@8");
  G.CloseSequence = Fn<CloseAnimation_t>("_GrannyCloseSequence@8");
  G.GetAnimationParameter = Fn<GetAnimationParameter_t>("_GrannyGetAnimationParameter@20");
  G.Close = Fn<Close_t>("_GrannyClose@4");
}

// The game converts every model/animation from its tool's coordinate system into its own:
// origin 0, axes rows (-B[0],0,0), (0,0,B[5]), (0,B[7],0) where B is the tool axis block.
struct Transform { float offset[16]; float linear[32]; };  // game uses 3 and 9 floats; padded
static Transform Recommend(uint32_t granny, const float tool_origin[3], const float tool_axes[9]) {
  float origin[3] = {0, 0, 0};
  float axes[9] = {-tool_axes[0], 0, 0, 0, 0, tool_axes[5], 0, tool_axes[7], 0};
  Transform t{};
  G.RecommendTransform(granny, tool_origin, tool_axes, origin, axes, t.offset, t.linear);
  return t;
}

// One frame of the sequence's current pose.
static bool DumpFrame(H2 seq) {
  H2 r{};
  if (G.LockSequenceForRendering(seq.a, seq.b, 0x3B, &r) != 0) return false;
  uint32_t n = 0;
  G.GetRenderingStatesLeft(r.a, r.b, &n);
  std::vector<std::vector<uint8_t>> states;
  for (uint32_t i = 0; i < n; ++i) {
    RenderState s{};
    if (G.LockNextRenderingState(r.a, r.b, &s) != 0) continue;
    uint32_t nv = U32(s, RS_POS) ? U32(s, RS_POS + 0xC) : 0;
    uint32_t nt = U32(s, RS_TRI) ? U32(s, RS_TRI + 0xC) : 0;
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t len) { auto c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + len); };
    uint32_t tex = U32(s, RS_TEXTURE);
    put(&tex, 4);
    put(&nv, 4);
    std::vector<float> zero(nv * 3, 0.0f);
    for (int block : {RS_POS, RS_NORMAL, RS_UV}) {
      bool ok = U32(s, block) && U32(s, block + 0xC) == nv && PTR(s, block + 0x14);
      put(ok ? PTR(s, block + 0x14) : zero.data(), nv * 12);
    }
    put(&nt, 4);
    if (nt) put(PTR(s, RS_TRI + 0x14), nt * 6);
    states.push_back(std::move(b));
    G.UnlockRenderingState(&s);
  }
  G.UnlockRendering(r.a, r.b);
  W32(static_cast<uint32_t>(states.size()));
  for (auto& b : states) W(b.data(), b.size());
  return true;
}

// A Granny instance with the model loaded, transformed like the game does, and one sequence (instance).
struct Session {
  uint32_t granny = 0;
  H2 model{}, seq{};
  bool Open(const char* path) {
    if (G.OpenVersion("1.2b", "win32", "10-4-2000",
                      "(C) Copyright 1999-2000 RAD Game Tools, Inc.  All Rights Reserved.", &granny) != 0) {
      fprintf(stderr, "GrannyOpenVersion failed\n");
      return false;
    }
    G.AllowTextureFormat(granny, 0xB);
    if (G.OpenModel(granny, path, &model) != 0) { fprintf(stderr, "cannot open model %s\n", path); return false; }
    float tool_origin[3] = {}, tool_axes[9] = {};
    G.GetToolCoordinateSystemForModel(model.a, model.b, tool_origin, tool_axes);
    Transform t = Recommend(granny, tool_origin, tool_axes);
    G.TransformModel(model.a, model.b, t.offset, t.linear);
    if (G.OpenSequence(model.a, model.b, &seq) != 0) { fprintf(stderr, "cannot open sequence\n"); return false; }
    return true;
  }
  void Close() {
    if (seq.a) G.CloseSequence(seq.a, seq.b);
    if (model.a) G.CloseModel(model.a, model.b);
    if (granny) G.Close(granny);
    granny = 0;
    model = seq = H2{};
  }
};

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: grn_dump <granny.dll> <model.grn> <out.bin> [--fps N] [anim.grn ...]\n");
    return 1;
  }
  g_dll = LoadLibraryA(argv[1]);
  if (!g_dll) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
  LoadApi();
  double fps = 30.0;
  std::vector<const char*> anims;
  for (int i = 4; i < argc; ++i) {
    if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
    else anims.push_back(argv[i]);
  }

  g_out = fopen(argv[3], "wb");
  if (!g_out) { fprintf(stderr, "cannot write %s\n", argv[3]); return 6; }
  W("GRD1", 4);

  // Rest pose and textures.
  Session rest;
  if (!rest.Open(argv[2])) return 4;
  // Textures: the game looks each one up by name among the model's .omt 3D materials.
  uint32_t ntex = 0;
  G.GetNewTexturesLeft(rest.granny, &ntex);
  W32(ntex);
  for (uint32_t i = 0; i < ntex; ++i) {
    Texture t{};
    if (G.LockNextNewTexture(rest.granny, &t) != 0) { WS(""); WS(""); continue; }
    WS(*reinterpret_cast<const char* const*>(t.raw + 4));    // material name
    WS(*reinterpret_cast<const char* const*>(t.raw + 0xC));  // source file
    G.UnlockNewTexture(&t, i);  // the user value comes back in each rendering state
  }
  W32(1 + static_cast<uint32_t>(anims.size()));
  G.AdvanceTime(rest.granny, 0.0);
  WS("rest");
  WF(0.0f);
  W32(1);
  DumpFrame(rest.seq);
  rest.Close();

  int failures = 0;
  for (const char* path : anims) {
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    WS(base);
    WF(static_cast<float>(fps));
    // Granny integrates pose state per AdvanceTime step and misbehaves after large clock jumps, so
    // every animation gets a fresh session whose clock starts at 0 and moves in 1/fps steps.
    Session s;
    H2 anim{};
    H3 ctrl{};
    if (!s.Open(argv[2]) || G.OpenAnimation(s.granny, path, &anim) != 0) {
      fprintf(stderr, "cannot open animation %s\n", path);
      W32(0);
      ++failures;
      s.Close();
      continue;
    }
    float ao[3] = {}, aa[9] = {};
    G.GetToolCoordinateSystemForAnimation(anim.a, anim.b, ao, aa);
    Transform at = Recommend(s.granny, ao, aa);
    G.TransformAnimation(anim.a, anim.b, at.offset, at.linear);
    G.AdvanceTime(s.granny, 0.0);
    G.AddControlledAnimation(s.seq.a, s.seq.b, anim.a, anim.b, &ctrl);
    // Parameters (GrannyGetAnimationParameter): 0/1 = start/end of one play in seconds, 2 = speed,
    // 3 = loop count (the game passes FLT_MAX for "forever"), 4 = weight, 5..8 = ease in/out.
    double start = 0, stop = 0;
    G.GetAnimationParameter(ctrl.a, ctrl.b, ctrl.c, 0, &start);
    G.GetAnimationParameter(ctrl.a, ctrl.b, ctrl.c, 1, &stop);
    G.SetAnimationParameter(ctrl.a, ctrl.b, ctrl.c, 3, 3.4e38);
    double duration = stop - start;
    uint32_t frames = duration > 0 ? static_cast<uint32_t>(duration * fps + 0.5) + 1 : 1;
    W32(frames);
    for (uint32_t f = 0; f < frames; ++f) {
      G.AdvanceTime(s.granny, start + (f + 1 == frames ? duration : f / fps));
      DumpFrame(s.seq);
    }
    G.RemoveControlledAnimation(ctrl.a, ctrl.b, ctrl.c);
    G.CloseAnimation(anim.a, anim.b);
    s.Close();
    if (getenv("GRN_VERBOSE")) fprintf(stderr, "%s: %.3f s, %u frames\n", base, duration, frames);
  }
  fclose(g_out);
  return failures ? 7 : 0;
}
