#include "anymix/Bridge.h"
#include "anymix/Devices.h"
#include "anymix/Engine.h"
#include "anymix/NoiseGate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

constexpr DWORD kMainStyle = WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;

void ApplyWindowFrame(HWND window) {
  if (window == nullptr) {
    return;
  }
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  const LONG_PTR bare = style & ~(WS_CAPTION | WS_BORDER | WS_DLGFRAME);
  if (bare != style) {
    SetWindowLongPtrW(window, GWL_STYLE, bare);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
  const LONG_PTR bareEx = ex & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
  if (bareEx != ex) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, bareEx);
  }
  const MARGINS none{0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(window, &none);
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
  const int corner = 2;
  DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
  const COLORREF border = 0xFFFFFFFE;
  DwmSetWindowAttribute(window, 34, &border, sizeof(border));
  static bool themeReady = false;
  static HRESULT(WINAPI* setTheme)(HWND, LPCWSTR, LPCWSTR) = nullptr;
  if (!themeReady) {
    if (HMODULE theme = LoadLibraryW(L"uxtheme.dll")) {
      setTheme = reinterpret_cast<HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR)>(GetProcAddress(theme, "SetWindowTheme"));
    }
    themeReady = true;
  }
  if (setTheme != nullptr) {
    setTheme(window, L"", L"");
  }
}

namespace {

enum Page { kMixer = 0, kRouting, kDevices, kApps, kRecord, kAsio, kSettings, kVst, kPageCount };

enum Part {
  kFader = 1, kMute, kSolo, kMono, kBass, kTreble, kPan, kInsert = 8, kGate = 9, kRoute0 = 10,
  kLocal = 20,
  kGateLevel = 21, kGateLed = 26,
  kGateDepth = 28, kName = 30
};

struct Hot {
  RECT r{};
  int id = 0;
  const wchar_t* tip = L"";
};

struct StripUi {
  float gain = 0.f;
  float bass = 0.f;
  float treble = 0.f;
  bool mute = false;
  bool solo = false;
  bool mono = false;
  float pan = 0.f;
  bool insert = false;
  bool local = true;
  bool gate = false;
  float gateDepth = 1.f;
  bool route[anymix::kBuses]{};
};

struct MicGateUi {
  float level = 0.f;
};

struct Palette {
  Gdiplus::Color bg, bar, surface, strip, text, dim, line, accent, mute, solo, ok, warn, clip, meter;
};

wchar_t g_label[anymix::kStrips][32]{};
HWND g_edit = nullptr;
int g_editStrip = -1;
int g_editGroup = -1;
WNDPROC g_editProc = nullptr;
anymix::Engine* g_engine = nullptr;
bool g_engineOn = false;
bool g_dark = true;
bool g_compact = false;
bool g_anim = true;
bool g_closeToTray = false;
bool g_startWithWindows = true;
bool g_forceQuit = false;
bool g_trayAdded = false;
Page g_page = kMixer;
int g_vstStrip = anymix::kMic1;
int g_vstScroll = 0;
int g_vstVisible = 12;
bool g_vstScanned = false;
wchar_t g_vstStatus[160] = L"Plug-ins run on the strip before the external ASIO insert.";
struct VstListItem {
  wchar_t name[96];
  wchar_t path[520];
};
VstListItem g_vstFiles[128]{};
int g_vstFileCount = 0;
struct VstGroupUi {
  bool used = false;
  bool bypass = false;
  wchar_t name[32]{};
};
VstGroupUi g_vstGroup[anymix::kVstGroups]{};
int8_t g_vstSlotGroup[anymix::kVstSlots]{};
RECT g_vstGroupRect[anymix::kVstGroups]{};
bool g_vstGroupRectOn[anymix::kVstGroups]{};
RECT g_vstLooseRect{};
bool g_vstLooseOn = false;
bool g_vstPending = false;
bool g_vstDrag = false;
int g_vstFromSlot = -1;
int g_vstFromFile = -1;
int g_vstDownX = 0;
int g_vstDownY = 0;
int g_vstDragX = 0;
int g_vstDragY = 0;
int g_vstHoverGroup = -2;
wchar_t g_vstDragName[128]{};
int g_hover = 0;
int g_focusStrip = 0;
int g_appTab = 0;
int g_drag = 0;
int g_scroll = 0;
int g_mixWidth = 0;
wchar_t g_hint[256] = L"Hover a control for its action.";
wchar_t g_folder[MAX_PATH] = {};
wchar_t g_lastTake[MAX_PATH] = {};
wchar_t g_playPath[MAX_PATH] = {};
bool g_playing = false;
HANDLE g_playThread = nullptr;
std::atomic<bool> g_playStop{false};
HWAVEOUT g_wave = nullptr;
CRITICAL_SECTION g_playLock{};
bool g_playLockOn = false;
ULONGLONG g_recStart = 0;
float g_meter[anymix::kStrips]{};
float g_meterL[anymix::kStrips]{};
float g_meterR[anymix::kStrips]{};
float g_appMeter[anymix::kVirtualStrips][32]{};
unsigned long g_appMeterId[anymix::kVirtualStrips][32]{};
StripUi g_strip[anymix::kStrips];
MicGateUi g_micGate[anymix::kMicStrips];
bool g_gateOpen[anymix::kMicStrips]{};
float g_busDb[anymix::kBuses]{};
bool g_busMute[anymix::kBuses]{};
bool g_busMono[anymix::kBuses]{};
bool g_busSolo[anymix::kPhysicalBuses]{};
float g_stripMix[anymix::kStrips]{};
float g_busMix[anymix::kBuses]{};
float g_busMeter[anymix::kBuses]{};
float g_busMeterL[anymix::kBuses]{};
float g_busMeterR[anymix::kBuses]{};
float g_navMix[kPageCount]{};
float g_pageAlpha = 1.f;
std::vector<anymix::DeviceInfo> g_renders;
std::vector<anymix::DeviceInfo> g_captures;
int g_micSel[anymix::kMicStrips]{1, 0};
bool g_micUser[anymix::kMicStrips]{};
int g_outSel[anymix::kPhysicalBuses]{};
int g_recBus = 0;
int g_recFormat = 0;
uint32_t g_recMask = 0;
uint32_t g_playMask = 0;
std::atomic<int> g_encodeState{0};
HANDLE g_encodeThread = nullptr;

struct RecFormat {
  const wchar_t* label;
  const wchar_t* ext;
  int kind;
};
constexpr int kRecWav = 0;
constexpr int kRecMp3 = 1;
constexpr int kRecFlac = 2;
constexpr int kRecAac = 3;
constexpr int kRecWma = 4;
constexpr RecFormat kRecFormats[] = {
    {L"WAV", L".wav", kRecWav},   {L"MP3", L".mp3", kRecMp3}, {L"FLAC", L".flac", kRecFlac},
    {L"AAC", L".m4a", kRecAac},   {L"WMA", L".wma", kRecWma},
};
constexpr int kRecFormatCount = 5;

int g_pop = 0;
int g_popScroll = 0;
RECT g_popAnchor{};
bool g_appPending = false;
bool g_appDrag = false;
int g_appFrom = -1;
int g_appRow = -1;
unsigned long g_appPid = 0;
wchar_t g_appName[128]{};
int g_appDownX = 0;
int g_appDownY = 0;
int g_appX = 0;
int g_appY = 0;
int g_dropStrip = -1;
int g_appsOpen = -1;
int g_captionHot = 0;
int g_captionPress = 0;
HWND g_mainWindow = nullptr;
HWND g_miniWindow = nullptr;
bool g_miniMode = false;
bool g_havePlace = false;
bool g_openMini = false;
int g_miniX = 0;
int g_miniY = 0;
WINDOWPLACEMENT g_place{};
int g_miniDrag = -1;
int g_miniHover = 0;
ID2D1HwndRenderTarget* g_miniRt = nullptr;
ID2D1SolidColorBrush* g_miniBrush = nullptr;
std::vector<std::vector<anymix::SessionInfo>> g_sessions(anymix::kVirtualStrips);

struct SavedApp {
  wchar_t name[64]{};
  int strip = 0;
  float volume = 1.f;
  bool muted = false;
};

std::vector<SavedApp> g_savedApps;
std::vector<unsigned long> g_seenApp;
wchar_t g_savedMic[anymix::kMicStrips][512]{};
wchar_t g_savedOut[anymix::kPhysicalBuses][512]{};
bool g_haveDeviceIds = false;
std::vector<Hot> g_hot;
Gdiplus::Image* g_logo = nullptr;
Gdiplus::Image* g_logoLong = nullptr;
Gdiplus::Bitmap* g_logoLongOnDark = nullptr;
ULONG_PTR g_gdi = 0;
float g_frameDt = 0.016f;
ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_write = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
ID2D1StrokeStyle* g_round = nullptr;
int g_targetGen = 0;

int ClampInt(int value, int lo, int hi) {
  if (value < lo) {
    return lo;
  }
  return value > hi ? hi : value;
}
int Sid(int strip, int part) { return 1000 + strip * 40 + part; }

const wchar_t* StripLabel(int strip) {
  if (strip < 0 || strip >= anymix::kStrips || g_label[strip][0] == 0) {
    return anymix::kStripNames[std::max(0, strip)];
  }
  return g_label[strip];
}
int NavId(int page) { return 100 + page; }

Palette Colors() {
  Palette p{};
  if (g_dark) {
    p.bg = Gdiplus::Color(255, 4, 14, 32);
    p.bar = Gdiplus::Color(255, 7, 20, 42);
    p.surface = Gdiplus::Color(255, 12, 32, 64);
    p.strip = Gdiplus::Color(255, 14, 36, 70);
    p.text = Gdiplus::Color(255, 236, 242, 250);
    p.dim = Gdiplus::Color(255, 154, 176, 206);
    p.line = Gdiplus::Color(255, 32, 58, 96);
    p.accent = Gdiplus::Color(255, 0, 115, 239);
    p.mute = Gdiplus::Color(255, 226, 91, 91);
    p.solo = Gdiplus::Color(255, 224, 177, 90);
    p.ok = Gdiplus::Color(255, 61, 214, 151);
    p.warn = Gdiplus::Color(255, 224, 177, 90);
    p.clip = Gdiplus::Color(255, 255, 90, 90);
    p.meter = Gdiplus::Color(255, 61, 214, 151);
  } else {
    p.bg = Gdiplus::Color(255, 243, 245, 248);
    p.bar = Gdiplus::Color(255, 255, 255, 255);
    p.surface = Gdiplus::Color(255, 255, 255, 255);
    p.strip = Gdiplus::Color(255, 247, 248, 251);
    p.text = Gdiplus::Color(255, 4, 14, 32);
    p.dim = Gdiplus::Color(255, 92, 107, 126);
    p.line = Gdiplus::Color(255, 213, 220, 230);
    p.accent = Gdiplus::Color(255, 0, 115, 239);
    p.mute = Gdiplus::Color(255, 198, 40, 40);
    p.solo = Gdiplus::Color(255, 168, 118, 12);
    p.ok = Gdiplus::Color(255, 27, 127, 78);
    p.warn = Gdiplus::Color(255, 168, 118, 12);
    p.clip = Gdiplus::Color(255, 198, 40, 40);
    p.meter = Gdiplus::Color(255, 27, 127, 78);
  }
  return p;
}

void CopyText(wchar_t* dst, int cap, const wchar_t* src) {
  if (cap <= 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = 0;
    return;
  }
  wcsncpy(dst, src, static_cast<size_t>(cap - 1));
  dst[cap - 1] = 0;
}

bool NameHas(const wchar_t* full, const wchar_t* part) {
  return full != nullptr && part != nullptr && wcsstr(full, part) != nullptr;
}

int FindDevice(const std::vector<anymix::DeviceInfo>& devices, const wchar_t* fragment) {
  for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
    if (wcsstr(devices[static_cast<size_t>(i)].name, fragment) != nullptr) {
      return i;
    }
  }
  return 0;
}

bool SameDeviceId(const wchar_t* a, const wchar_t* b) {
  return a != nullptr && b != nullptr && a[0] != 0 && _wcsicmp(a, b) == 0;
}

int IndexOfDevice(const std::vector<anymix::DeviceInfo>& devices, const wchar_t* id) {
  for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
    if (SameDeviceId(devices[static_cast<size_t>(i)].id, id)) {
      return i;
    }
  }
  return -1;
}

void AddHot(const RECT& r, int id, const wchar_t* tip) {
  g_hot.push_back(Hot{r, id, tip});
}

RECT R(int x, int y, int w, int h) {
  RECT r{x, y, x + w, y + h};
  return r;
}

struct CachedBmp {
  Gdiplus::Image* key = nullptr;
  ID2D1Bitmap* bmp = nullptr;
};

struct TextFmt {
  float size = 0.f;
  bool center = false;
  IDWriteTextFormat* fmt = nullptr;
};

CachedBmp g_uploads[4]{};
int g_uploadGen = -1;
TextFmt g_fonts[24]{};

D2D1_COLOR_F D2dColor(Gdiplus::Color color) {
  return D2D1::ColorF(color.GetR() / 255.f, color.GetG() / 255.f, color.GetB() / 255.f, color.GetA() / 255.f);
}

Gdiplus::Color BrushColor(const Gdiplus::Brush* brush) {
  Gdiplus::Color color;
  static_cast<const Gdiplus::SolidBrush*>(brush)->GetColor(&color);
  return color;
}

void DropUploads() {
  for (CachedBmp& slot : g_uploads) {
    if (slot.bmp != nullptr) {
      slot.bmp->Release();
    }
    slot = {};
  }
  g_uploadGen = -1;
}

void DropTarget() {
  DropUploads();
  if (g_brush != nullptr) {
    g_brush->Release();
    g_brush = nullptr;
  }
  if (g_round != nullptr) {
    g_round->Release();
    g_round = nullptr;
  }
  if (g_rt != nullptr) {
    g_rt->Release();
    g_rt = nullptr;
  }
  ++g_targetGen;
}

ID2D1PathGeometry* GeometryOf(const Gdiplus::GraphicsPath* path) {
  const INT count = path->GetPointCount();
  if (count <= 0 || g_d2d == nullptr) {
    return nullptr;
  }
  std::vector<Gdiplus::PointF> points(static_cast<size_t>(count));
  std::vector<BYTE> types(static_cast<size_t>(count));
  if (path->GetPathPoints(points.data(), count) != Gdiplus::Ok ||
      path->GetPathTypes(types.data(), count) != Gdiplus::Ok) {
    return nullptr;
  }
  ID2D1PathGeometry* geo = nullptr;
  if (FAILED(g_d2d->CreatePathGeometry(&geo)) || geo == nullptr) {
    return nullptr;
  }
  ID2D1GeometrySink* sink = nullptr;
  if (FAILED(geo->Open(&sink)) || sink == nullptr) {
    geo->Release();
    return nullptr;
  }
  bool open = false;
  for (int i = 0; i < count;) {
    const BYTE raw = types[static_cast<size_t>(i)];
    const BYTE kind = raw & 0x7;
    const bool close = (raw & 0x80) != 0;
    if (kind == 0) {
      if (open) {
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        open = false;
      }
      sink->BeginFigure(D2D1::Point2F(points[static_cast<size_t>(i)].X, points[static_cast<size_t>(i)].Y),
                        D2D1_FIGURE_BEGIN_FILLED);
      open = true;
      ++i;
    } else if (kind == 1 && open) {
      sink->AddLine(D2D1::Point2F(points[static_cast<size_t>(i)].X, points[static_cast<size_t>(i)].Y));
      if (close) {
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        open = false;
      }
      ++i;
    } else if (kind == 3 && open && i + 2 < count) {
      sink->AddBezier(D2D1::BezierSegment(
          D2D1::Point2F(points[static_cast<size_t>(i)].X, points[static_cast<size_t>(i)].Y),
          D2D1::Point2F(points[static_cast<size_t>(i + 1)].X, points[static_cast<size_t>(i + 1)].Y),
          D2D1::Point2F(points[static_cast<size_t>(i + 2)].X, points[static_cast<size_t>(i + 2)].Y)));
      if ((types[static_cast<size_t>(i + 2)] & 0x80) != 0) {
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        open = false;
      }
      i += 3;
    } else {
      ++i;
    }
  }
  if (open) {
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
  }
  sink->Close();
  sink->Release();
  return geo;
}

struct Canvas {
  ID2D1RenderTarget* rt = nullptr;

  void SetSmoothingMode(Gdiplus::SmoothingMode) {}
  void SetTextRenderingHint(Gdiplus::TextRenderingHint) {}
  void SetInterpolationMode(Gdiplus::InterpolationMode) {}

  void Use(Gdiplus::Color color) const {
    if (g_brush != nullptr) {
      g_brush->SetColor(D2dColor(color));
    }
  }

  void FillRectangle(const Gdiplus::Brush* brush, float x, float y, float w, float h) const {
    Use(BrushColor(brush));
    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_brush);
  }

  void FillEllipse(const Gdiplus::Brush* brush, float x, float y, float w, float h) const {
    Use(BrushColor(brush));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + w * 0.5f, y + h * 0.5f), w * 0.5f, h * 0.5f), g_brush);
  }

  void FillPath(const Gdiplus::Brush* brush, const Gdiplus::GraphicsPath* path) const {
    ID2D1PathGeometry* geo = GeometryOf(path);
    if (geo == nullptr) {
      return;
    }
    Use(BrushColor(brush));
    rt->FillGeometry(geo, g_brush);
    geo->Release();
  }

  void DrawPath(const Gdiplus::Pen* pen, const Gdiplus::GraphicsPath* path) const {
    ID2D1PathGeometry* geo = GeometryOf(path);
    if (geo == nullptr) {
      return;
    }
    Gdiplus::Color color;
    pen->GetColor(&color);
    Use(color);
    rt->DrawGeometry(geo, g_brush, pen->GetWidth(), g_round);
    geo->Release();
  }

  void DrawLine(const Gdiplus::Pen* pen, float x1, float y1, float x2, float y2) const {
    Gdiplus::Color color;
    pen->GetColor(&color);
    Use(color);
    rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush, pen->GetWidth(), g_round);
  }

  void DrawBezier(const Gdiplus::Pen* pen, float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                  float y4) const {
    ID2D1PathGeometry* geo = nullptr;
    if (g_d2d == nullptr || FAILED(g_d2d->CreatePathGeometry(&geo)) || geo == nullptr) {
      return;
    }
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink)) || sink == nullptr) {
      geo->Release();
      return;
    }
    sink->BeginFigure(D2D1::Point2F(x1, y1), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x2, y2), D2D1::Point2F(x3, y3), D2D1::Point2F(x4, y4)));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    Gdiplus::Color color;
    pen->GetColor(&color);
    Use(color);
    rt->DrawGeometry(geo, g_brush, pen->GetWidth(), g_round);
    geo->Release();
  }

  void DrawArc(const Gdiplus::Pen* pen, float x, float y, float w, float h, float start, float sweep) const {
    if (w <= 0.f || h <= 0.f || sweep == 0.f || g_d2d == nullptr) {
      return;
    }
    const float rx = w * 0.5f;
    const float ry = h * 0.5f;
    const float cx = x + rx;
    const float cy = y + ry;
    auto at = [&](float deg) {
      const float a = deg * 3.14159265f / 180.f;
      return D2D1::Point2F(cx + rx * std::cos(a), cy + ry * std::sin(a));
    };
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&geo)) || geo == nullptr) {
      return;
    }
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink)) || sink == nullptr) {
      geo->Release();
      return;
    }
    sink->BeginFigure(at(start), D2D1_FIGURE_BEGIN_HOLLOW);
    float left = sweep;
    float cursor = start;
    const float dir = sweep >= 0.f ? 1.f : -1.f;
    while (std::fabs(left) > 0.01f) {
      float step = left;
      if (std::fabs(step) > 180.f) {
        step = dir * 180.f;
      }
      sink->AddArc(D2D1::ArcSegment(at(cursor + step), D2D1::SizeF(rx, ry), 0.f,
                                    dir > 0.f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                    std::fabs(step) >= 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
      cursor += step;
      left -= step;
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    Gdiplus::Color color;
    pen->GetColor(&color);
    Use(color);
    rt->DrawGeometry(geo, g_brush, pen->GetWidth(), g_round);
    geo->Release();
  }

  void DrawImage(Gdiplus::Image* image, float x, float y, float w, float h) const;
  void DrawLabel(const wchar_t* text, RECT r, Gdiplus::Color color, float size, bool center) const;
};

ID2D1Bitmap* UploadImage(Gdiplus::Image* image) {
  const UINT w = image->GetWidth();
  const UINT h = image->GetHeight();
  if (w == 0 || h == 0 || g_rt == nullptr) {
    return nullptr;
  }
  Gdiplus::Bitmap bmp(static_cast<INT>(w), static_cast<INT>(h), PixelFormat32bppPARGB);
  Gdiplus::Graphics copy(&bmp);
  copy.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
  copy.DrawImage(image, 0, 0, static_cast<INT>(w), static_cast<INT>(h));
  Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
  Gdiplus::BitmapData data{};
  if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) {
    return nullptr;
  }
  ID2D1Bitmap* out = nullptr;
  g_rt->CreateBitmap(D2D1::SizeU(w, h), data.Scan0, data.Stride,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                     &out);
  bmp.UnlockBits(&data);
  return out;
}

ID2D1Bitmap* CachedImage(Gdiplus::Image* image) {
  if (image == nullptr || g_rt == nullptr) {
    return nullptr;
  }
  if (g_uploadGen != g_targetGen) {
    DropUploads();
    g_uploadGen = g_targetGen;
  }
  for (CachedBmp& slot : g_uploads) {
    if (slot.key == image) {
      return slot.bmp;
    }
  }
  for (CachedBmp& slot : g_uploads) {
    if (slot.key == nullptr) {
      slot.key = image;
      slot.bmp = UploadImage(image);
      return slot.bmp;
    }
  }
  return nullptr;
}

void Canvas::DrawImage(Gdiplus::Image* image, float x, float y, float w, float h) const {
  ID2D1Bitmap* bmp = CachedImage(image);
  if (bmp == nullptr) {
    return;
  }
  rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + w, y + h), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

IDWriteTextFormat* FontFor(float size, bool center) {
  for (TextFmt& slot : g_fonts) {
    if (slot.fmt != nullptr && slot.center == center && std::fabs(slot.size - size) < 0.1f) {
      return slot.fmt;
    }
  }
  TextFmt* free = nullptr;
  for (TextFmt& slot : g_fonts) {
    if (slot.fmt == nullptr) {
      free = &slot;
      break;
    }
  }
  if (free == nullptr || g_write == nullptr) {
    return nullptr;
  }
  IDWriteTextFormat* fmt = nullptr;
  if (FAILED(g_write->CreateTextFormat(L"Century Gothic", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt))) {
    fmt = nullptr;
    if (FAILED(g_write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt))) {
      return nullptr;
    }
  }
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  fmt->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
  fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  IDWriteInlineObject* ellipsis = nullptr;
  if (SUCCEEDED(g_write->CreateEllipsisTrimmingSign(fmt, &ellipsis)) && ellipsis != nullptr) {
    DWRITE_TRIMMING trim{};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    fmt->SetTrimming(&trim, ellipsis);
    ellipsis->Release();
  }
  free->size = size;
  free->center = center;
  free->fmt = fmt;
  return fmt;
}

void Canvas::DrawLabel(const wchar_t* text, RECT r, Gdiplus::Color color, float size, bool center) const {
  IDWriteTextFormat* fmt = FontFor(size, center);
  if (fmt == nullptr || text == nullptr) {
    return;
  }
  Use(color);
  rt->DrawText(text, static_cast<UINT32>(wcslen(text)), fmt,
               D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                           static_cast<float>(r.bottom)),
               g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_NO_SNAP, DWRITE_MEASURING_MODE_NATURAL);
}

bool EnsureTarget(HWND window, int w, int h) {
  if (g_d2d == nullptr) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) || g_d2d == nullptr) {
      return false;
    }
  }
  if (g_d2d != nullptr && g_round == nullptr) {
    D2D1_STROKE_STYLE_PROPERTIES stroke = D2D1::StrokeStyleProperties();
    stroke.startCap = D2D1_CAP_STYLE_ROUND;
    stroke.endCap = D2D1_CAP_STYLE_ROUND;
    stroke.dashCap = D2D1_CAP_STYLE_ROUND;
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    g_d2d->CreateStrokeStyle(stroke, nullptr, 0, &g_round);
  }
  if (g_write == nullptr) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(&g_write))) ||
        g_write == nullptr) {
      return false;
    }
  }
  if (g_rt == nullptr) {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
    if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(w, h)),
                                            &g_rt)) ||
        g_rt == nullptr) {
      props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
      if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(w, h)),
                                              &g_rt)) ||
          g_rt == nullptr) {
        return false;
      }
    }
    g_rt->SetDpi(96.f, 96.f);
    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f), &g_brush);
    ++g_targetGen;
  } else {
    const D2D1_SIZE_U size = g_rt->GetPixelSize();
    if (size.width != static_cast<UINT32>(w) || size.height != static_cast<UINT32>(h)) {
      g_rt->Resize(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)));
    }
  }
  return g_rt != nullptr && g_brush != nullptr;
}

void FillRR(Canvas& g, RECT r, int rad, Gdiplus::Color color) {
  Gdiplus::SolidBrush brush(color);
  Gdiplus::GraphicsPath path;
  const int d = rad * 2;
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;
  if (rad <= 0 || w < d || h < d) {
    g.FillRectangle(&brush, r.left, r.top, w, h);
    return;
  }
  path.AddArc(r.left, r.top, d, d, 180, 90);
  path.AddArc(r.right - d, r.top, d, d, 270, 90);
  path.AddArc(r.right - d, r.bottom - d, d, d, 0, 90);
  path.AddArc(r.left, r.bottom - d, d, d, 90, 90);
  path.CloseFigure();
  g.FillPath(&brush, &path);
}

void AddRoundRect(Gdiplus::GraphicsPath& path, RECT r, int rad) {
  const int d = rad * 2;
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;
  if (w < 2 || h < 2) {
    return;
  }
  if (w < d || h < d) {
    path.AddRectangle(Gdiplus::Rect(r.left, r.top, w, h));
    return;
  }
  path.AddArc(r.left, r.top, d, d, 180, 90);
  path.AddArc(r.right - d, r.top, d, d, 270, 90);
  path.AddArc(r.right - d, r.bottom - d, d, d, 0, 90);
  path.AddArc(r.left, r.bottom - d, d, d, 90, 90);
  path.CloseFigure();
}

void StrokeRR(Canvas& g, RECT r, int rad, Gdiplus::Color color, float width) {
  if (color.GetA() == 0 || r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  Gdiplus::GraphicsPath path;
  AddRoundRect(path, r, rad);
  Gdiplus::Pen pen(color, width);
  g.DrawPath(&pen, &path);
}

Gdiplus::Color Fade(Gdiplus::Color color, int alpha) {
  return Gdiplus::Color(static_cast<BYTE>(ClampInt(alpha, 0, 255)), color.GetR(), color.GetG(), color.GetB());
}

double AnimT() { return static_cast<double>(GetTickCount64()) * 0.001; }

float Pulse(float hz, float phase) {
  return static_cast<float>(0.5 + 0.5 * std::sin(AnimT() * static_cast<double>(hz) * 6.28318530718 + phase));
}

void Approach(float& value, float target) {
  const float factor = 1.f - std::exp(-7.53f * g_frameDt);
  value += (target - value) * factor;
  if (std::fabs(value - target) < 0.004f) {
    value = target;
  }
}

void Text(Canvas& g, const wchar_t* s, RECT r, Gdiplus::Color color, float size, bool center,
          bool bold = false) {
  (void)bold;
  g.DrawLabel(s == nullptr ? L"" : s, r, color, size, center);
}

void LoadLogos() {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* slash = wcsrchr(exe, L'\\');
  if (slash != nullptr) {
    *slash = 0;
  }
  wchar_t path[MAX_PATH];
  wsprintfW(path, L"%s\\..\\public\\logo.png", exe);
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
    wsprintfW(path, L"%s\\logo.png", exe);
  }
  g_logo = new Gdiplus::Image(path);
  wsprintfW(path, L"%s\\..\\public\\logo_long.png", exe);
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
    wsprintfW(path, L"%s\\logo_long.png", exe);
  }
  g_logoLong = new Gdiplus::Image(path);
}

HICON g_iconBig = nullptr;
HICON g_iconSmall = nullptr;

HICON IconFromImage(Gdiplus::Image* image, int size) {
  if (image == nullptr || image->GetLastStatus() != Gdiplus::Ok || image->GetWidth() == 0 || size <= 0) {
    return nullptr;
  }
  Gdiplus::Bitmap canvas(size, size, PixelFormat32bppARGB);
  Gdiplus::Graphics draw(&canvas);
  draw.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  draw.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
  draw.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
  draw.Clear(Gdiplus::Color(0, 0, 0, 0));
  const float scale = std::min(static_cast<float>(size) / static_cast<float>(image->GetWidth()),
                                static_cast<float>(size) / static_cast<float>(image->GetHeight()));
  const float dw = static_cast<float>(image->GetWidth()) * scale;
  const float dh = static_cast<float>(image->GetHeight()) * scale;
  draw.DrawImage(image, (static_cast<float>(size) - dw) * 0.5f, (static_cast<float>(size) - dh) * 0.5f, dw, dh);

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size;
  info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color == nullptr || bits == nullptr) {
    if (color != nullptr) {
      DeleteObject(color);
    }
    return nullptr;
  }
  Gdiplus::Rect rect(0, 0, size, size);
  Gdiplus::BitmapData data{};
  if (canvas.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
    DeleteObject(color);
    return nullptr;
  }
  auto* dst = static_cast<unsigned char*>(bits);
  for (int row = 0; row < size; ++row) {
    const auto* src = static_cast<const unsigned char*>(data.Scan0) + static_cast<size_t>(row) * data.Stride;
    unsigned char* out = dst + static_cast<size_t>(row) * static_cast<size_t>(size) * 4u;
    for (int col = 0; col < size; ++col) {
      const unsigned int alpha = src[col * 4 + 3];
      out[col * 4 + 0] = static_cast<unsigned char>(src[col * 4 + 0] * alpha / 255u);
      out[col * 4 + 1] = static_cast<unsigned char>(src[col * 4 + 1] * alpha / 255u);
      out[col * 4 + 2] = static_cast<unsigned char>(src[col * 4 + 2] * alpha / 255u);
      out[col * 4 + 3] = static_cast<unsigned char>(alpha);
    }
  }
  canvas.UnlockBits(&data);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO icon{};
  icon.fIcon = TRUE;
  icon.hbmColor = color;
  icon.hbmMask = mask;
  HICON result = CreateIconIndirect(&icon);
  DeleteObject(color);
  DeleteObject(mask);
  return result;
}

void DrawLogo(Canvas& g, Gdiplus::Image* image, RECT box) {
  if (image == nullptr || image->GetLastStatus() != Gdiplus::Ok || image->GetWidth() == 0) {
    return;
  }
  const float bw = static_cast<float>(box.right - box.left);
  const float bh = static_cast<float>(box.bottom - box.top);
  const float scale = std::min(bw / static_cast<float>(image->GetWidth()),
                               bh / static_cast<float>(image->GetHeight()));
  const float dw = static_cast<float>(image->GetWidth()) * scale;
  const float dh = static_cast<float>(image->GetHeight()) * scale;
  const float x = static_cast<float>(box.left) + (bw - dw) * 0.5f;
  const float y = static_cast<float>(box.top) + (bh - dh) * 0.5f;
  Gdiplus::Image* drawn = image;
  if (g_dark && image == g_logoLong) {
    if (g_logoLongOnDark == nullptr && g_logoLong->GetWidth() > 0) {
      const int lw = static_cast<int>(g_logoLong->GetWidth());
      const int lh = static_cast<int>(g_logoLong->GetHeight());
      g_logoLongOnDark = new Gdiplus::Bitmap(lw, lh, PixelFormat32bppARGB);
      Gdiplus::Graphics copy(g_logoLongOnDark);
      copy.DrawImage(g_logoLong, 0, 0, lw, lh);
      Gdiplus::Rect rect(0, 0, lw, lh);
      Gdiplus::BitmapData bits{};
      if (g_logoLongOnDark->LockBits(&rect, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                                    PixelFormat32bppARGB, &bits) == Gdiplus::Ok) {
        const int x0 = lw * 40 / 100;
        const int x1 = lw * 68 / 100;
        for (int row = 0; row < lh; ++row) {
          auto* pixels = reinterpret_cast<unsigned char*>(bits.Scan0) + static_cast<size_t>(row) * bits.Stride;
          for (int col = x0; col < x1; ++col) {
            unsigned char* px = pixels + col * 4;
            if (px[3] > 180 && px[2] < 40 && px[1] < 40 && px[0] < 60) {
              px[0] = 250;
              px[1] = 242;
              px[2] = 236;
            }
          }
        }
        g_logoLongOnDark->UnlockBits(&bits);
      }
    }
    if (g_logoLongOnDark != nullptr) {
      drawn = g_logoLongOnDark;
    }
  }
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g.DrawImage(drawn, x, y, dw, dh);
  g.SetInterpolationMode(Gdiplus::InterpolationModeDefault);
}

void SetStripSolo(int strip, bool on) {
  if (on) {
    for (int i = 0; i < anymix::kStrips; ++i) {
      if (i != strip && g_strip[i].solo) {
        g_strip[i].solo = false;
        g_engine->SetSolo(i, false);
      }
    }
  }
  g_strip[strip].solo = on;
  g_engine->SetSolo(strip, on);
}

void SetBusSolo(int bus, bool on) {
  if (on) {
    for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
      if (i != bus && g_busSolo[i]) {
        g_busSolo[i] = false;
        g_engine->SetBusSolo(i, false);
      }
    }
  }
  g_busSolo[bus] = on;
  g_engine->SetBusSolo(bus, on);
}

int MicOf(int strip) {
  const int mic = strip - anymix::kVirtualStrips;
  return mic >= 0 && mic < anymix::kMicStrips ? mic : -1;
}

bool IsGateSlider(int part) { return part == kGateLevel; }

float ClampF(float value, float lo, float hi) {
  return std::max(lo, std::min(hi, value));
}

void ApplyMicGateLevel(int mic, float level) {
  if (mic < 0 || mic >= anymix::kMicStrips) {
    return;
  }
  level = ClampF(level, 0.f, 10.f);
  g_micGate[mic].level = level;
  const int strip = anymix::kVirtualStrips + mic;
  g_strip[strip].gate = level >= 0.05f;
  if (g_engine != nullptr) {
    g_engine->SetGateLevel(strip, level);
    g_engine->SetGate(strip, g_strip[strip].gate);
  }
}

void PushMicGate(int mic) { ApplyMicGateLevel(mic, g_micGate[mic].level); }

#pragma pack(push, 1)
struct NoiseGateBlob {
  uint32_t magic = 0;
  uint32_t version = 0;
  float thresholdDb[anymix::kMicStrips]{};
  float attackMs[anymix::kMicStrips]{};
  float holdMs[anymix::kMicStrips]{};
  float releaseMs[anymix::kMicStrips]{};
  float rangeDb[anymix::kMicStrips]{};
  uint8_t bypass[anymix::kMicStrips]{};
  float level[anymix::kMicStrips]{};
};
#pragma pack(pop)

constexpr uint32_t kNoiseGateV1Bytes = offsetof(NoiseGateBlob, level);

constexpr uint32_t kNoiseGateMagic = 0x3147544Eu;

void WriteNoiseGate(HKEY key) {
  NoiseGateBlob blob{};
  blob.magic = kNoiseGateMagic;
  blob.version = 2;
  for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
    const anymix::GateSettings settings = anymix::CalculateGateSettings(g_micGate[mic].level);
    blob.level[mic] = g_micGate[mic].level;
    blob.thresholdDb[mic] = settings.thresholdDb;
    blob.attackMs[mic] = settings.attackMs;
    blob.holdMs[mic] = settings.holdMs;
    blob.releaseMs[mic] = settings.releaseMs;
    blob.rangeDb[mic] = settings.rangeDb;
    blob.bypass[mic] = settings.enabled ? 0 : 1;
  }
  RegSetValueExW(key, L"NoiseGate", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&blob), sizeof(blob));
}

void SaveNoiseGate() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  WriteNoiseGate(key);
  RegCloseKey(key);
}

void LoadNoiseGate() {
  auto fromDepth = [](int mic) {
    const int strip = anymix::kVirtualStrips + mic;
    if (!g_strip[strip].gate) {
      return 0.f;
    }
    return ClampF(10.f * g_strip[strip].gateDepth, 0.f, 10.f);
  };
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
      g_micGate[mic].level = fromDepth(mic);
    }
    return;
  }
  NoiseGateBlob blob{};
  DWORD size = sizeof(blob);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"NoiseGate", nullptr, &type, reinterpret_cast<BYTE*>(&blob), &size);
  RegCloseKey(key);
  const bool magicOk = status == ERROR_SUCCESS && type == REG_BINARY && blob.magic == kNoiseGateMagic;
  if (magicOk && blob.version == 2 && size == sizeof(blob)) {
    for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
      g_micGate[mic].level = ClampF(blob.level[mic], 0.f, 10.f);
      g_strip[anymix::kVirtualStrips + mic].gate = g_micGate[mic].level >= 0.05f;
    }
    return;
  }
  if (magicOk && blob.version == 1 && size == kNoiseGateV1Bytes) {
    for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
      const int strip = anymix::kVirtualStrips + mic;
      if (!g_strip[strip].gate || blob.bypass[mic] != 0) {
        g_micGate[mic].level = 0.f;
      } else {
        g_micGate[mic].level = anymix::GateLevelFromLegacy(blob.thresholdDb[mic], blob.rangeDb[mic]);
      }
      g_strip[strip].gate = g_micGate[mic].level >= 0.05f;
    }
    return;
  }
  for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
    g_micGate[mic].level = fromDepth(mic);
  }
}

void PushEngineStrip(int strip) {
  g_engine->SetGainDb(strip, g_strip[strip].gain);
  g_engine->SetBassDb(strip, g_strip[strip].bass);
  g_engine->SetTrebleDb(strip, g_strip[strip].treble);
  g_engine->SetMute(strip, g_strip[strip].mute);
  g_engine->SetSolo(strip, g_strip[strip].solo);
  g_engine->SetMono(strip, g_strip[strip].mono);
  g_engine->SetPan(strip, g_strip[strip].pan);
  g_engine->SetInsert(strip, g_strip[strip].insert);
  g_engine->SetLocalVst(strip, g_strip[strip].local);
  g_engine->SetGate(strip, g_strip[strip].gate);
  g_engine->SetGateDepth(strip, g_strip[strip].gateDepth);
  const int mic = MicOf(strip);
  if (mic >= 0) {
    PushMicGate(mic);
  }
  for (int bus = 0; bus < anymix::kBuses; ++bus) {
    g_engine->SetRoute(strip, bus, g_strip[strip].route[bus]);
  }
}

void PushDevices() {
  for (int i = 0; i < anymix::kMicStrips; ++i) {
    const int sel = g_micSel[i];
    const wchar_t* id = L"";
    if (sel > 0 && sel < static_cast<int>(g_captures.size())) {
      id = g_captures[static_cast<size_t>(sel)].id;
    } else if (sel < 0 && g_savedMic[i][0] != 0) {
      id = g_savedMic[i];
    }
    g_engine->SetCaptureDevice(i, id);
  }
  for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
    const int sel = g_outSel[i];
    const wchar_t* id =
        (sel >= 0 && sel < static_cast<int>(g_renders.size())) ? g_renders[static_cast<size_t>(sel)].id : L"";
    g_engine->SetRenderDevice(i, id);
  }
}

void ReloadDevices(bool initial) {
  std::vector<anymix::DeviceInfo> renders(256);
  std::vector<anymix::DeviceInfo> captures(256);
  renders.resize(static_cast<size_t>(anymix::EnumRenderDevices(renders.data(), 255)));
  captures.resize(static_cast<size_t>(anymix::EnumCaptureDevices(captures.data(), 255)));
  if (!initial && captures.empty() && g_captures.size() > 1) {
    return;
  }
  if (!initial && renders.empty() && g_renders.size() > 1) {
    return;
  }
  renders.insert(renders.begin(), anymix::DeviceInfo{});
  captures.insert(captures.begin(), anymix::DeviceInfo{});
  CopyText(renders[0].name, 256, L"None");
  CopyText(captures[0].name, 256, L"None");
  wchar_t keepMic[anymix::kMicStrips][512]{};
  wchar_t keepOut[anymix::kPhysicalBuses][512]{};
  if (!initial) {
    for (int i = 0; i < anymix::kMicStrips; ++i) {
      if (g_micSel[i] > 0 && g_micSel[i] < static_cast<int>(g_captures.size())) {
        CopyText(keepMic[i], 512, g_captures[static_cast<size_t>(g_micSel[i])].id);
      } else if (g_savedMic[i][0] != 0) {
        CopyText(keepMic[i], 512, g_savedMic[i]);
      }
    }
    for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
      if (g_outSel[i] > 0 && g_outSel[i] < static_cast<int>(g_renders.size())) {
        CopyText(keepOut[i], 512, g_renders[static_cast<size_t>(g_outSel[i])].id);
      } else if (g_savedOut[i][0] != 0) {
        CopyText(keepOut[i], 512, g_savedOut[i]);
      }
    }
  }
  g_renders.swap(renders);
  g_captures.swap(captures);
  if (initial && !g_haveDeviceIds) {
    const int mic1 = FindDevice(g_captures, L"Microphone");
    const int casque = FindDevice(g_renders, L"Casque");
    const int enceinte = FindDevice(g_renders, L"Enceinte");
    g_micSel[0] = mic1 == 0 && g_captures.size() > 1 ? 1 : mic1;
    g_micSel[1] = 0;
    g_outSel[0] = casque == 0 && g_renders.size() > 1 ? 1 : casque;
    g_outSel[1] = enceinte;
    for (int i = 2; i < anymix::kPhysicalBuses; ++i) {
      g_outSel[i] = 0;
    }
  } else {
    for (int i = 0; i < anymix::kMicStrips; ++i) {
      const wchar_t* wanted = initial ? g_savedMic[i] : keepMic[i];
      const int found = IndexOfDevice(g_captures, wanted);
      if (found > 0) {
        g_micSel[i] = found;
        CopyText(g_savedMic[i], 512, g_captures[static_cast<size_t>(found)].id);
      } else if (wanted[0] != 0) {
        g_micSel[i] = -1;
        CopyText(g_savedMic[i], 512, wanted);
      } else {
        g_micSel[i] = 0;
      }
    }
    for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
      const wchar_t* wanted = initial ? g_savedOut[i] : keepOut[i];
      const int found = IndexOfDevice(g_renders, wanted);
      if (found > 0) {
        g_outSel[i] = found;
        CopyText(g_savedOut[i], 512, g_renders[static_cast<size_t>(found)].id);
      } else if (wanted[0] != 0) {
        g_outSel[i] = -1;
        CopyText(g_savedOut[i], 512, wanted);
      } else {
        g_outSel[i] = 0;
      }
    }
  }
  if (g_engine != nullptr) {
    PushDevices();
  }
}

const wchar_t* CaptureChoice(int mic);
const wchar_t* RenderChoice(int bus);

const wchar_t* DeviceName(const std::vector<anymix::DeviceInfo>& list, int sel) {
  if (sel < 0 || sel >= static_cast<int>(list.size())) {
    return L"None";
  }
  return list[static_cast<size_t>(sel)].name;
}

bool DeviceStillThere(const std::vector<anymix::DeviceInfo>& list, int sel) {
  if (sel < 0) {
    return false;
  }
  if (sel == 0) {
    return true;
  }
  if (sel >= static_cast<int>(list.size())) {
    return false;
  }
  return list[static_cast<size_t>(sel)].id[0] != 0;
}

bool VadLabel(const wchar_t* name, const wchar_t* label) {
  if (name == nullptr || label == nullptr) {
    return false;
  }
  wchar_t plain[160];
  wchar_t second[180];
  wsprintfW(plain, L"%s (ASIOVADPRO Driver)", label);
  wsprintfW(second, L"%s (2- ASIOVADPRO Driver)", label);
  return wcscmp(name, plain) == 0 || wcscmp(name, second) == 0;
}

int VirtualPresent(bool capture) {
  int count = 0;
  const auto& list = capture ? g_captures : g_renders;
  const int n = capture ? anymix::kVirtualBuses : anymix::kVirtualStrips;
  for (int i = 0; i < n; ++i) {
    const wchar_t* name = capture ? anymix::kCaptureDeviceNames[i] : anymix::kVirtualDeviceNames[i];
    for (const auto& device : list) {
      if (VadLabel(device.name, name)) {
        ++count;
        break;
      }
    }
  }
  return count;
}

const wchar_t* VirtualId(int strip) {
  for (const auto& device : g_renders) {
    if (VadLabel(device.name, anymix::kVirtualDeviceNames[strip])) {
      return device.id;
    }
  }
  return nullptr;
}

void ApplySavedApps() {
  for (int strip = 0; strip < anymix::kVirtualStrips; ++strip) {
    const wchar_t* endpoint = VirtualId(strip);
    if (endpoint == nullptr) {
      continue;
    }
    auto& sessions = g_sessions[static_cast<size_t>(strip)];
    for (auto& session : sessions) {
      if (session.sessionId == 0) {
        continue;
      }
      bool seen = false;
      for (unsigned long pid : g_seenApp) {
        if (pid == session.sessionId) {
          seen = true;
          break;
        }
      }
      if (seen) {
        continue;
      }
      const SavedApp* match = nullptr;
      for (const SavedApp& saved : g_savedApps) {
        if (saved.strip == strip && _wcsicmp(saved.name, session.name) == 0) {
          match = &saved;
          break;
        }
      }
      g_seenApp.push_back(session.sessionId);
      if (match == nullptr) {
        continue;
      }
      std::wstring error;
      anymix::SetSessionVolume(endpoint, session.sessionId, match->volume, error);
      anymix::SetSessionMute(endpoint, session.sessionId, match->muted, error);
      session.volume = match->volume;
      session.muted = match->muted;
    }
  }
}

void RefreshSessions() {
  for (int strip = 0; strip < anymix::kVirtualStrips; ++strip) {
    g_sessions[static_cast<size_t>(strip)].clear();
    const wchar_t* endpoint = VirtualId(strip);
    if (endpoint == nullptr) {
      continue;
    }
    std::wstring error;
    anymix::SessionInfo infos[16];
    const int count = anymix::EnumSessions(endpoint, infos, 16, error);
    if (count > 0) {
      g_sessions[static_cast<size_t>(strip)].assign(infos, infos + count);
    }
  }
  ApplySavedApps();
}

bool AsioRegistered() {
  wchar_t exe[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    return false;
  }
  wchar_t* slash = wcsrchr(exe, L'\\');
  if (slash == nullptr) {
    return false;
  }
  wchar_t expected[MAX_PATH]{};
  wcsncpy(expected, exe, static_cast<size_t>(slash - exe));
  expected[slash - exe] = 0;
  wcsncat(expected, L"\\AnyMixInsertASIO.dll", MAX_PATH - wcslen(expected) - 1);
  if (GetFileAttributesW(expected) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Classes\\CLSID\\{8F3A6C21-4B7E-4D15-9A62-1C0E5B7D4A90}\\InprocServer32", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t registered[MAX_PATH]{};
  DWORD size = sizeof(registered);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(registered), &size);
  RegCloseKey(key);
  return status == ERROR_SUCCESS && type == REG_SZ && _wcsicmp(registered, expected) == 0;
}

bool AsioHostRunning() {
  HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, anymix::kAsioMappingName);
  if (map == nullptr) {
    return false;
  }
  void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(anymix::AsioBridge));
  bool running = false;
  if (view != nullptr) {
    const auto* bridge = static_cast<const anymix::AsioBridge*>(view);
    running = bridge->magic == anymix::kAsioMagic && bridge->hostRunning != 0;
    UnmapViewOfFile(view);
  }
  CloseHandle(map);
  return running;
}

void DrawChip(Canvas& g, RECT r, const wchar_t* label, Gdiplus::Color color, const Palette& pal, int id,
              const wchar_t* tip, bool on) {
  FillRR(g, r, 8, on ? color : pal.surface);
  Text(g, label, r, on ? Gdiplus::Color(255, 12, 16, 22) : pal.text, 12, true);
  if (id != 0) {
    AddHot(r, id, tip);
  }
}

float DbOf(float peak) {
  if (peak <= 0.00001f) {
    return -80.f;
  }
  return 20.f * std::log10(peak);
}

Gdiplus::Color MeterZone(float db, const Palette& pal, bool gray) {
  if (gray) {
    return pal.dim;
  }
  if (db >= -6.f) {
    return pal.clip;
  }
  if (db >= -18.f) {
    return pal.warn;
  }
  return pal.ok;
}

void DrawMeter(Canvas& g, RECT r, float db, const Palette& pal, bool gray = false, bool background = true,
               int insetL = 2, int insetR = 2) {
  if (background) {
    FillRR(g, r, 3, pal.bg);
  }
  const int top = r.top + 2;
  const int bottom = r.bottom - 2;
  const int height = bottom - top;
  const int segH = 3;
  const int gap = 1;
  const int pitch = segH + gap;
  const int count = height / pitch;
  if (count <= 0) {
    return;
  }
  const int barW = std::max(4, static_cast<int>(r.right - r.left) - insetL - insetR);
  const int x = r.left + insetL;
  const float clamped = std::max(-60.f, std::min(0.f, db));
  int topLit = -1;
  for (int i = 0; i < count; ++i) {
    const float segDb = -60.f + (static_cast<float>(i) + 0.5f) / static_cast<float>(count) * 60.f;
    if (clamped >= segDb) {
      topLit = i;
    }
  }
  for (int i = 0; i < count; ++i) {
    const float segDb = -60.f + (static_cast<float>(i) + 0.5f) / static_cast<float>(count) * 60.f;
    const bool lit = i <= topLit;
    const int y = bottom - (i + 1) * pitch;
    Gdiplus::Color zone = MeterZone(segDb, pal, gray);
    if (lit && i == topLit && !gray) {
      zone = Gdiplus::Color(255, std::min(255, zone.GetR() + 40), std::min(255, zone.GetG() + 40),
                            std::min(255, zone.GetB() + 28));
    }
    Gdiplus::SolidBrush brush(lit ? zone : Fade(zone, g_dark ? 38 : 55));
    g.FillRectangle(&brush, x, y, barW, segH);
  }
}

void DrawMeterPair(Canvas& g, RECT left, RECT right, float dbL, float dbR, const Palette& pal, bool gray) {
  Text(g, L"L", R(left.left, left.top - 13, std::max(8, static_cast<int>(left.right - left.left)), 12), pal.dim, 9,
       true);
  Text(g, L"R", R(right.left, right.top - 13, std::max(8, static_cast<int>(right.right - right.left)), 12), pal.dim, 9,
       true);
  RECT both{left.left, left.top, right.right, right.bottom};
  FillRR(g, both, 3, pal.bg);
  DrawMeter(g, left, dbL, pal, gray, false, 2, 0);
  DrawMeter(g, right, dbR, pal, gray, false, 0, 2);
  const int top = both.top + 2;
  const int bottom = both.bottom - 2;
  const int lineW = std::max(1, static_cast<int>(right.left - left.right));
  Gdiplus::SolidBrush divider(pal.line);
  g.FillRectangle(&divider, left.right, top, lineW, std::max(1, bottom - top));
}

void DrawFader(Canvas& g, RECT r, float gain, const Palette& pal, bool hot) {
  const int cx = (r.left + r.right) / 2;
  RECT track{cx - 3, r.top + 8, cx + 3, r.bottom - 8};
  FillRR(g, track, 3, pal.line);
  const float t = (anymix::kFaderMaxDb - gain) / (anymix::kFaderMaxDb - anymix::kFaderMinDb);
  const int travel = (r.bottom - 8) - (r.top + 8);
  const int y = r.top + 8 + static_cast<int>(t * static_cast<float>(travel));
  if (y < track.bottom - 2) {
    RECT filled{track.left, y, track.right, track.bottom};
    FillRR(g, filled, 3, Fade(pal.accent, hot ? 170 : 80));
  }
  RECT cap{cx - 12, y - 8, cx + 12, y + 8};
  if (hot) {
    RECT halo{cap.left - 4, cap.top - 3, cap.right + 4, cap.bottom + 3};
    FillRR(g, halo, 8, Fade(pal.accent, 50 + static_cast<int>(40 * Pulse(2.4f, 0.4f))));
  }
  FillRR(g, cap, 6, hot ? pal.accent : pal.text);
}

float GainFromY(RECT r, int y) {
  const float span = static_cast<float>((r.bottom - 8) - (r.top + 8));
  if (span <= 1.f) {
    return 0.f;
  }
  float t = (static_cast<float>(y) - static_cast<float>(r.top + 8)) / span;
  t = std::max(0.f, std::min(1.f, t));
  return std::max(anymix::kFaderMinDb, std::min(anymix::kFaderMaxDb,
                                               anymix::kFaderMaxDb - t * (anymix::kFaderMaxDb - anymix::kFaderMinDb)));
}

float PanFromX(RECT r, int x) {
  const float span = static_cast<float>(r.right - r.left);
  if (span <= 1.f) {
    return 0.f;
  }
  float t = (static_cast<float>(x) - static_cast<float>(r.left)) / span;
  t = std::max(0.f, std::min(1.f, t));
  return std::max(-1.f, std::min(1.f, t * 2.f - 1.f));
}

float EqFromX(RECT r, int x) {
  const float span = static_cast<float>(r.right - r.left);
  if (span <= 1.f) {
    return 0.f;
  }
  float t = (static_cast<float>(x) - static_cast<float>(r.left)) / span;
  t = std::max(0.f, std::min(1.f, t));
  return std::max(-12.f, std::min(12.f, -12.f + t * 24.f));
}

void DrawDepth(Canvas& g, RECT r, float amount, const Palette& pal) {
  FillRR(g, r, 4, pal.bg);
  const float t = std::max(0.f, std::min(1.f, amount));
  const int span = std::max(1, static_cast<int>(r.right - r.left));
  const int fill = static_cast<int>(t * static_cast<float>(span));
  if (fill > 2) {
    FillRR(g, R(r.left, r.top, fill, r.bottom - r.top), 4, pal.ok);
  }
  const int knob = r.left + fill;
  Gdiplus::SolidBrush brush(pal.text);
  g.FillEllipse(&brush, static_cast<float>(knob - 5), static_cast<float>(r.top) - 2.f, 10.f,
                static_cast<float>(r.bottom - r.top) + 4.f);
}

void DrawEq(Canvas& g, RECT r, float db, const Palette& pal) {
  FillRR(g, r, 4, pal.bg);
  const int mid = (r.left + r.right) / 2;
  const float t = (db + 12.f) / 24.f;
  const int x = r.left + static_cast<int>(t * static_cast<float>(r.right - r.left));
  RECT mark{x - 4, r.top + 2, x + 4, r.bottom - 2};
  FillRR(g, mark, 3, pal.accent);
  RECT zero{mid, r.top + 3, mid + 1, r.bottom - 3};
  Gdiplus::SolidBrush brush(pal.dim);
  g.FillRectangle(&brush, zero.left, zero.top, 1, zero.bottom - zero.top);
}

void DrawButton(Canvas& g, RECT r, const wchar_t* label, bool on, Gdiplus::Color onColor,
                const Palette& pal, bool hot, float labelSize = 0.f) {
  if (hot && !on) {
    RECT glow{r.left - 1, r.top - 1, r.right + 1, r.bottom + 1};
    StrokeRR(g, glow, 7, Fade(pal.accent, 90), 1.4f);
  }
  FillRR(g, r, 6, on ? onColor : (hot ? pal.surface : pal.line));
  const float size = labelSize > 0.f ? labelSize : (g_compact ? 11.f : 12.f);
  Text(g, label, r, on ? Gdiplus::Color(255, 255, 255, 255) : pal.text, size, true);
}

void PaintMixer(Canvas& g, RECT area, const Palette& pal) {
  const int scaleW = 36;
  const int stripW = g_compact ? 132 : 156;
  const int gap = 8;
  g_mixWidth = anymix::kStrips * (stripW + gap);
  const int viewW = area.right - area.left - scaleW - 16;
  if (g_scroll > g_mixWidth - viewW) {
    g_scroll = std::max(0, g_mixWidth - viewW);
  }
  if (g_scroll < 0) {
    g_scroll = 0;
  }
  const int top = area.top + 8;
  const int meterTop = top + 28;
  const int meterH = g_compact ? 150 : 210;
  const int marks[6] = {0, -3, -6, -12, -30, -60};
  for (int i = 0; i < 6; ++i) {
    const float t = (static_cast<float>(marks[i]) + 60.f) / 60.f;
    const int y = meterTop + static_cast<int>((1.f - t) * static_cast<float>(meterH));
    wchar_t label[8];
    wsprintfW(label, L"%d", marks[i]);
    Text(g, label, R(area.left, y - 8, scaleW - 4, 16), pal.dim, 10, true);
  }
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    const int x = area.left + scaleW - g_scroll + strip * (stripW + gap);
    if (x + stripW < area.left + scaleW || x > area.right) {
      continue;
    }
    RECT card = R(x, top, stripW, area.bottom - top - 28);
    FillRR(g, card, 12, pal.strip);
    Text(g, StripLabel(strip), R(x + 8, top + 4, stripW - 16, 22), pal.text, 14, true);
    RECT meter = R(x + 10, meterTop, 16, meterH);
    const float db = DbOf(g_meter[strip]);
    DrawMeter(g, meter, db, pal);
    if (db >= -0.5f && g_meter[strip] > 0.00001f) {
      Text(g, L"CLIP", R(x + 8, meterTop - 16, 40, 14), pal.clip, 10, false);
    }
    RECT fader = R(x + 32, meterTop, 36, meterH);
    DrawFader(g, fader, g_strip[strip].gain, pal, g_hover == Sid(strip, kFader) || g_drag == Sid(strip, kFader));
    AddHot(fader, Sid(strip, kFader), L"Fader, -60 to +24 dB. Wheel or arrows change gain. Double-click returns to 0 dB.");
    wchar_t gain[32];
    swprintf(gain, 32, L"%+.1f dB", static_cast<double>(g_strip[strip].gain));
    Text(g, gain, R(x + 8, meterTop + meterH + 2, stripW - 16, 18), pal.dim, 12, true);
    int y = meterTop + meterH + 22;
    const int bw = (stripW - 20) / 3;
    RECT mute = R(x + 8, y, bw, 22);
    RECT solo = R(x + 10 + bw, y, bw, 22);
    RECT mono = R(x + 12 + bw * 2, y, bw, 22);
    DrawButton(g, mute, L"Mute", g_strip[strip].mute, pal.mute, pal, g_hover == Sid(strip, kMute));
    DrawButton(g, solo, L"Solo", g_strip[strip].solo, pal.solo, pal, g_hover == Sid(strip, kSolo));
    DrawButton(g, mono, L"Mono", g_strip[strip].mono, pal.accent, pal, g_hover == Sid(strip, kMono));
    AddHot(mute, Sid(strip, kMute), L"Mute this strip.");
    AddHot(solo, Sid(strip, kSolo), L"Solo. Other strips are silenced while any solo is on.");
    AddHot(mono, Sid(strip, kMono), L"Mono. Left and right are summed.");
    y += 28;
    wchar_t bass[32];
    wchar_t treble[32];
    swprintf(bass, 32, L"Bass %+.1f", static_cast<double>(g_strip[strip].bass));
    swprintf(treble, 32, L"Treble %+.1f", static_cast<double>(g_strip[strip].treble));
    Text(g, bass, R(x + 8, y, stripW - 16, 16), pal.dim, 11, false);
    RECT bassR = R(x + 8, y + 16, stripW - 16, 14);
    DrawEq(g, bassR, g_strip[strip].bass, pal);
    AddHot(bassR, Sid(strip, kBass), L"Bass boost, -12 to +12 dB. The low end comes forward without saturating. Double-click resets to 0.");
    Text(g, treble, R(x + 8, y + 32, stripW - 16, 16), pal.dim, 11, false);
    RECT trebleR = R(x + 8, y + 48, stripW - 16, 14);
    DrawEq(g, trebleR, g_strip[strip].treble, pal);
    AddHot(trebleR, Sid(strip, kTreble), L"Treble, -12 to +12 dB. Brings the highs out without saturating. Double-click resets to 0.");
    y += 70;
    Text(g, L"A", R(x + 8, y, 16, 18), pal.dim, 11, false);
    Text(g, L"B", R(x + 8, y + 24, 16, 18), pal.dim, 11, false);
    for (int bus = 0; bus < anymix::kBuses; ++bus) {
      const int col = bus % 5;
      const int row = bus / 5;
      RECT btn = R(x + 24 + col * 24, y + row * 24, 22, 20);
      DrawButton(g, btn, anymix::kBusNames[bus] + 1, g_strip[strip].route[bus],
                 row == 0 ? pal.accent : pal.solo, pal, g_hover == Sid(strip, kRoute0 + bus));
      wchar_t tip[64];
      wsprintfW(tip, L"Route %s to %s.", StripLabel(strip), anymix::kBusNames[bus]);
      AddHot(btn, Sid(strip, kRoute0 + bus), L"Toggle this route.");
      (void)tip;
    }
  }
  if (g_mixWidth > viewW) {
    RECT bar = R(area.left + scaleW, area.bottom - 18, viewW, 10);
    FillRR(g, bar, 4, pal.line);
    const int thumbW = std::max(24, viewW * viewW / std::max(1, g_mixWidth));
    const int travel = std::max(1, viewW - thumbW);
    const int maxScroll = std::max(1, g_mixWidth - viewW);
    const int thumbX = bar.left + g_scroll * travel / maxScroll;
    RECT thumb{thumbX, bar.top, thumbX + thumbW, bar.bottom};
    FillRR(g, thumb, 4, pal.dim);
    AddHot(bar, 7001, L"Scroll the mixer.");
  }
}

void DrawSelector(Canvas& g, RECT r, const wchar_t* label, const wchar_t* value, bool ok,
                  const Palette& pal, int id, const wchar_t* tip) {
  FillRR(g, r, 8, pal.surface);
  Text(g, label, R(r.left + 10, r.top + 4, 80, r.bottom - r.top - 8), pal.dim, 12, false);
  Text(g, value, R(r.left + 78, r.top + 4, r.right - r.left - 110, r.bottom - r.top - 8), pal.text, 13, false);
  Text(g, ok ? L"Connected" : L"Unavailable", R(r.right - 110, r.top + 4, 100, r.bottom - r.top - 8),
       ok ? pal.ok : pal.warn, 12, true);
  AddHot(r, id, tip);
}

void PaintDevices(Canvas& g, RECT area, const Palette& pal) {
  Text(g, L"Inputs", R(area.left + 16, area.top + 8, 200, 24), pal.text, 16, false);
  for (int i = 0; i < anymix::kMicStrips; ++i) {
    wchar_t label[16];
    wsprintfW(label, L"Mic %d", i + 1);
    const bool ok = DeviceStillThere(g_captures, g_micSel[i]);
    DrawSelector(g, R(area.left + 16, area.top + 40 + i * 48,
                      ClampInt(static_cast<int>(area.right - area.left - 32), 240, 640), 40),
                 label, CaptureChoice(i), ok, pal, 2000 + i,
                 L"Choose the Windows capture device for this microphone.");
  }
  Text(g, L"Outputs", R(area.left + 16, area.top + 150, 200, 24), pal.text, 16, false);
  for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
    const bool ok = DeviceStillThere(g_renders, g_outSel[i]);
    DrawSelector(g, R(area.left + 16, area.top + 182 + i * 48,
                      ClampInt(static_cast<int>(area.right - area.left - 32), 240, 640), 40),
                 anymix::kBusNames[i], RenderChoice(i), ok, pal, 2100 + i,
                 L"Choose the Windows playback device for this output.");
  }
  wchar_t virt[128];
  wsprintfW(virt, L"Virtual playback %d/5    Virtual microphones %d/5", VirtualPresent(false),
            VirtualPresent(true));
  Text(g, virt, R(area.left + 16, area.bottom - 36, 500, 22), pal.dim, 13, false);
  const wchar_t* err = g_engine->DeviceError();
  if (err != nullptr && err[0] != 0) {
    Text(g, err, R(area.left + 16, area.bottom - 58, area.right - area.left - 32, 20), pal.clip, 12, false);
  }
}

void DrawAppRow(Canvas& g, RECT line, int strip, int index, const anymix::SessionInfo& session, const Palette& pal,
               bool compact, bool levels = false) {
  const int appId = 6000 + strip * 32 + index;
  const int volId = 9000 + strip * 32 + index;
  const float vol = session.volume < 0.f ? 0.f : (session.volume > 1.f ? 1.f : session.volume);
  if (levels) {
    const int vuH = compact ? 5 : 8;
    const int fadH = compact ? 7 : 10;
    const int gap = compact ? 2 : 4;
    RECT name = line;
    name.bottom = line.top + std::max(compact ? 13 : 18, static_cast<int>(line.bottom - line.top) - vuH - fadH - gap * 2);
    RECT vu = R(line.left, name.bottom + gap, line.right - line.left, vuH);
    RECT fad = R(line.left, vu.bottom + gap, line.right - line.left, fadH);
    DrawButton(g, name, session.name, session.muted, pal.mute, pal, g_hover == appId, compact ? 10.f : 12.f);
    AddHot(name, appId, L"Drag onto Game, Chat, Music, Aux, or Add. Click mutes this application.");
    FillRR(g, vu, 3, pal.bg);
    const float peak = (strip >= 0 && strip < anymix::kVirtualStrips && index >= 0 && index < 32)
                           ? g_appMeter[strip][index]
                           : 0.f;
    const int vuW = static_cast<int>(static_cast<float>(vu.right - vu.left) * (peak < 0.f ? 0.f : (peak > 1.f ? 1.f : peak)));
    if (vuW > 1) {
      const Gdiplus::Color zone = session.muted ? pal.dim : (peak >= 0.9f ? pal.clip : pal.meter);
      FillRR(g, R(vu.left, vu.top, vuW, vu.bottom - vu.top), 3, zone);
    }
    FillRR(g, fad, 4, pal.bg);
    const int trackW = std::max(1, static_cast<int>(fad.right - fad.left));
    const int fillW = static_cast<int>(static_cast<float>(trackW) * vol);
    if (fillW > 1) {
      FillRR(g, R(fad.left, fad.top, fillW, fad.bottom - fad.top), 4, session.muted ? pal.dim : Fade(pal.accent, 170));
    }
    const int knob = compact ? 6 : 8;
    int knobX = fad.left + static_cast<int>(static_cast<float>(trackW - knob) * vol);
    if (knobX < fad.left) {
      knobX = fad.left;
    }
    FillRR(g, R(knobX, fad.top - 1, knob, fadH + 2), 3, session.muted ? pal.dim : pal.text);
    AddHot(fad, volId, L"Volume. Drag left or right.");
    return;
  }
  const int barH = compact ? 5 : 6;
  RECT name = line;
  name.bottom = std::max(name.top + 8, line.bottom - barH - 1);
  RECT bar = R(line.left, name.bottom + 1, line.right - line.left, line.bottom - (name.bottom + 1));
  DrawButton(g, name, session.name, session.muted, pal.mute, pal, g_hover == appId, compact ? 9.f : 12.f);
  AddHot(name, appId, L"Drag onto Game, Chat, Music, Aux, or Add. Click mutes this application.");
  FillRR(g, bar, 2, pal.bg);
  const int fillW = static_cast<int>(static_cast<float>(bar.right - bar.left) * vol);
  if (fillW > 0) {
    FillRR(g, R(bar.left, bar.top, fillW, bar.bottom - bar.top), 2, session.muted ? pal.dim : pal.accent);
  }
  AddHot(bar, volId, L"Application volume. Drag left or right.");
}

Gdiplus::Color Accent(int strip);
void DrawChannelBadge(Canvas& g, RECT card, int strip, const Palette& pal);

void PaintApps(Canvas& g, RECT area, const Palette& pal) {
  const int cols = anymix::kVirtualStrips;
  const int gap = 10;
  const int span = std::max(cols, static_cast<int>(area.right - area.left));
  const int colH = std::max(80, static_cast<int>(area.bottom - area.top));
  const int colW = std::max(80, (span - (cols - 1) * gap) / cols);
  for (int strip = 0; strip < cols; ++strip) {
    const int x = area.left + strip * (colW + gap);
    const int w = strip == cols - 1 ? std::max(80, static_cast<int>(area.right - x)) : colW;
    RECT card = R(x, area.top, w, colH);
    const Gdiplus::Color accent = Accent(strip);
    FillRR(g, card, 10, pal.strip);
    StrokeRR(g, card, 10, Fade(accent, 48), 1.5f);
    DrawChannelBadge(g, card, strip, pal);
    RECT list = R(x + 10, area.top + 56, w - 20, colH - 68);
    if (VirtualId(strip) == nullptr) {
      Text(g, L"Device not present", list, pal.warn, 13, false);
      continue;
    }
    const auto& sessions = g_sessions[static_cast<size_t>(strip)];
    if (sessions.empty()) {
      Text(g, L"No applications", list, pal.dim, 13, false);
      continue;
    }
    const int rowH = 50;
    const int rowGap = 8;
    int shown = 0;
    for (int i = 0; i < static_cast<int>(sessions.size()) && i < 31; ++i) {
      const int y = list.top + shown * (rowH + rowGap);
      if (y + rowH > list.bottom) {
        const int left = static_cast<int>(sessions.size()) - i;
        if (left > 0) {
          wchar_t more[24];
          wsprintfW(more, L"+%d", left);
          Text(g, more, R(list.left, y, list.right - list.left, 16), pal.dim, 12, true);
        }
        break;
      }
      RECT line = R(list.left, y, list.right - list.left, rowH);
      DrawAppRow(g, line, strip, i, sessions[static_cast<size_t>(i)], pal, false, true);
      ++shown;
    }
  }
}

void PaintRecord(Canvas& g, RECT area, const Palette& pal) {
  Text(g, L"Recorder", R(area.left + 16, area.top + 12, 240, 24), pal.text, 16, false);
  const bool rec = g_engine->Recording();
  ULONGLONG sec = 0;
  if (rec && g_recStart != 0) {
    sec = (GetTickCount64() - g_recStart) / 1000;
  }
  wchar_t clock[32];
  swprintf(clock, 32, L"%02u:%02u:%02u", static_cast<unsigned>(sec / 3600),
           static_cast<unsigned>((sec / 60) % 60), static_cast<unsigned>(sec % 60));
  Text(g, rec ? L"Recording" : L"Stopped", R(area.left + 16, area.top + 48, 120, 22), rec ? pal.clip : pal.dim, 14,
       false);
  Text(g, clock, R(area.left + 140, area.top + 44, 160, 28), pal.text, 20, false);
  const int columnW = 560;
  const int previewBus = std::max(0, std::min(anymix::kBuses - 1, g_recBus));
  bool previewSilent = g_busMute[previewBus];
  if (!previewSilent && previewBus < anymix::kPhysicalBuses) {
    bool anySolo = false;
    for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
      if (g_busSolo[i]) {
        anySolo = true;
      }
    }
    previewSilent = anySolo && !g_busSolo[previewBus];
  }
  const float previewL = previewSilent ? -60.f : DbOf(g_busMeterL[previewBus]);
  const float previewR = previewSilent ? -60.f : DbOf(g_busMeterR[previewBus]);
  const int barW = 42;
  const int meterTop = area.top + 36;
  const int meterH = std::max(280, std::min(460, static_cast<int>(area.bottom - area.top) - 72));
  const int meterX = area.left + columnW + 36;
  RECT meterL = R(meterX, meterTop, barW, meterH);
  RECT meterR = R(meterX + barW + 12, meterTop, barW, meterH);
  DrawMeterPair(g, meterL, meterR, previewL, previewR, pal, previewSilent);
  Text(g, anymix::kBusNames[previewBus], R(meterL.left - 8, meterTop + meterH + 6, barW * 2 + 28, 22), pal.text, 16,
       true);
  DrawSelector(g, R(area.left + 16, area.top + 88, 280, 40), L"Bus", anymix::kBusNames[g_recBus], true, pal, 4000,
               L"Bus written into the recording.");
  const int routeCols = 5;
  const int routeW = 44;
  const int routeH = 30;
  const int routeGap = 6;
  const int gridW = routeCols * routeW + (routeCols - 1) * routeGap;
  auto routeGrid = [&](int originX, int originY, uint32_t mask, int idBase, const wchar_t* tip) {
    for (int bus = 0; bus < anymix::kBuses; ++bus) {
      const int col = bus % routeCols;
      const int row = bus / routeCols;
      RECT btn = R(originX + col * (routeW + routeGap), originY + row * (routeH + routeGap), routeW, routeH);
      DrawButton(g, btn, anymix::kBusNames[bus], (mask & (1u << bus)) != 0, row == 0 ? pal.accent : pal.solo, pal,
                 g_hover == idBase + bus, 12.f);
      AddHot(btn, idBase + bus, tip);
    }
  };
  Text(g, L"Record out", R(area.left + 16, area.top + 136, gridW, 20), pal.dim, 12, false);
  routeGrid(area.left + 16, area.top + 158, g_recMask, 4200,
            L"While recording, also send that signal here. Several buses can be on.");
  const int playX = area.left + 16 + gridW + 28;
  Text(g, L"Play out", R(playX, area.top + 136, gridW, 20), pal.dim, 12, false);
  routeGrid(playX, area.top + 158, g_playMask, 4300,
            L"Play the file into these buses. Several can be on. None plays through Windows.");
  const wchar_t* formatName = kRecFormats[std::max(0, std::min(kRecFormatCount - 1, g_recFormat))].label;
  RECT format = R(area.left + 308, area.top + 88, 200, 40);
  FillRR(g, format, 8, pal.surface);
  Text(g, L"Format", R(format.left + 10, format.top + 4, 70, 32), pal.dim, 12, false);
  Text(g, formatName, R(format.left + 78, format.top + 4, 100, 32), pal.text, 13, false);
  AddHot(format, 4007, L"File type: WAV, MP3, FLAC, AAC, or WMA.");
  RECT record = R(area.left + 16, area.top + 236, 100, 32);
  RECT stop = R(area.left + 124, area.top + 236, 100, 32);
  DrawButton(g, record, L"Record", rec, pal.clip, pal, g_hover == 4001);
  DrawButton(g, stop, L"Stop", false, pal.accent, pal, g_hover == 4002);
  AddHot(record, 4001, L"Start recording the selected bus.");
  AddHot(stop, 4002, L"Stop recording and save it in the chosen format.");
  if (g_encodeState.load() == 1) {
    Text(g, L"Encoding...", R(area.left + 236, area.top + 240, 220, 24), pal.warn, 13, false);
  } else if (g_encodeState.load() == 3) {
    Text(g, L"Could not encode. WAV was kept.", R(area.left + 236, area.top + 240, 280, 24), pal.clip, 13, false);
  }
  Text(g, L"Folder", R(area.left + 16, area.top + 282, 80, 22), pal.dim, 13, false);
  RECT folder = R(area.left + 16, area.top + 306,
                  ClampInt(static_cast<int>(area.right - area.left - 32), 200, 560), 36);
  FillRR(g, folder, 8, pal.surface);
  Text(g, g_folder, R(folder.left + 10, folder.top, folder.right - folder.left - 20, 36), pal.text, 13, false);
  AddHot(folder, 4003, L"Choose the recording folder.");
  if (rec) {
    Text(g, g_engine->RecordingPath(), R(area.left + 16, area.top + 352, columnW - 8, 22), pal.dim, 12, false);
  }
  Text(g, L"Playback", R(area.left + 16, area.top + 384, 200, 22), pal.text, 15, false);
  RECT last = R(area.left + 16, area.top + 414, 150, 32);
  RECT open = R(area.left + 174, area.top + 414, 120, 32);
  RECT halt = R(area.left + 302, area.top + 414, 90, 32);
  DrawButton(g, last, L"Last recording", false, pal.accent, pal, g_hover == 4004);
  DrawButton(g, open, L"Open file", false, pal.accent, pal, g_hover == 4005);
  DrawButton(g, halt, L"Stop", g_playing, pal.mute, pal, g_hover == 4006);
  AddHot(last, 4004, L"Play the newest recording in the folder.");
  AddHot(open, 4005, L"Play a WAV, MP3, FLAC, AAC, WMA, OGG, or another audio file.");
  AddHot(halt, 4006, L"Stop playback.");
  const wchar_t* shown = g_playPath[0] != 0 ? g_playPath : g_lastTake;
  const wchar_t* leaf = shown;
  if (shown != nullptr && shown[0] != 0) {
    const wchar_t* slash = wcsrchr(shown, L'\\');
    if (slash != nullptr && slash[1] != 0) {
      leaf = slash + 1;
    }
    Text(g, leaf, R(area.left + 16, area.top + 454, columnW - 8, 22), g_playing ? pal.ok : pal.dim, 13, false);
  }
}

#pragma pack(push, 1)
struct VstSlotDisk {
  int32_t strip = -1;
  uint8_t bypass = 0;
  uint8_t used = 0;
  wchar_t path[520]{};
};
struct VstDisk {
  uint32_t magic = 0;
  uint32_t version = 0;
  VstSlotDisk slots[anymix::kVstSlots]{};
};
#pragma pack(pop)

constexpr uint32_t kVstDiskMagic = 0x31545356u;
constexpr uint32_t kVstGroupMagic = 0x31504756u;

void SaveVstGroups();

void SaveVstRack() {
  if (g_engine == nullptr) {
    return;
  }
  VstDisk disk{};
  disk.magic = kVstDiskMagic;
  disk.version = 1;
  for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
    anymix::VstSlotView view{};
    g_engine->VstSlot(slot, view);
    disk.slots[slot].used = view.used ? 1 : 0;
    disk.slots[slot].bypass = view.bypass ? 1 : 0;
    disk.slots[slot].strip = view.strip;
    if (view.used) {
      wcsncpy(disk.slots[slot].path, view.path, 519);
    }
  }
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, L"VstRack", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&disk), sizeof(disk));
  RegCloseKey(key);
  SaveVstGroups();
}

void SaveVstLocal() {
  uint8_t flags[anymix::kStrips]{};
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    flags[strip] = g_strip[strip].local ? 1 : 0;
  }
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, L"VstLocal", 0, REG_BINARY, flags, sizeof(flags));
  RegCloseKey(key);
}

void LoadVstLocal() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  uint8_t flags[anymix::kStrips]{};
  DWORD size = sizeof(flags);
  DWORD kind = 0;
  const LONG read = RegQueryValueExW(key, L"VstLocal", nullptr, &kind, flags, &size);
  RegCloseKey(key);
  if (read != ERROR_SUCCESS || kind != REG_BINARY || size != sizeof(flags)) {
    return;
  }
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    g_strip[strip].local = flags[strip] != 0;
  }
}

#pragma pack(push, 1)
struct VstGroupDisk {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint8_t used[anymix::kVstGroups]{};
  uint8_t bypass[anymix::kVstGroups]{};
  wchar_t name[anymix::kVstGroups][32]{};
  int8_t slotGroup[anymix::kVstSlots]{};
};
#pragma pack(pop)

void SaveVstGroups() {
  VstGroupDisk disk{};
  disk.magic = kVstGroupMagic;
  disk.version = 1;
  for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
    disk.slotGroup[slot] = -1;
  }
  for (int group = 0; group < anymix::kVstGroups; ++group) {
    disk.used[group] = g_vstGroup[group].used ? 1 : 0;
    disk.bypass[group] = g_vstGroup[group].bypass ? 1 : 0;
    wcsncpy(disk.name[group], g_vstGroup[group].name, 31);
  }
  if (g_engine != nullptr) {
    for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
      anymix::VstSlotView view{};
      g_engine->VstSlot(slot, view);
      disk.slotGroup[slot] = view.used && view.group >= 0 ? static_cast<int8_t>(view.group) : static_cast<int8_t>(-1);
    }
  }
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, L"VstGroups", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&disk), sizeof(disk));
  RegCloseKey(key);
}

void LoadVstGroups() {
  for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
    g_vstSlotGroup[slot] = -1;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  VstGroupDisk disk{};
  DWORD size = sizeof(disk);
  DWORD kind = 0;
  const LONG read = RegQueryValueExW(key, L"VstGroups", nullptr, &kind, reinterpret_cast<BYTE*>(&disk), &size);
  RegCloseKey(key);
  if (read != ERROR_SUCCESS || kind != REG_BINARY || size != sizeof(disk) || disk.magic != kVstGroupMagic ||
      disk.version != 1) {
    return;
  }
  for (int group = 0; group < anymix::kVstGroups; ++group) {
    g_vstGroup[group].used = disk.used[group] != 0;
    g_vstGroup[group].bypass = disk.bypass[group] != 0;
    wcsncpy(g_vstGroup[group].name, disk.name[group], 31);
    g_vstGroup[group].name[31] = 0;
    if (g_engine != nullptr) {
      g_engine->SetVstGroupBypass(group, g_vstGroup[group].bypass);
    }
  }
  for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
    g_vstSlotGroup[slot] = disk.slotGroup[slot];
  }
}

void LoadVstRack() {
  LoadVstGroups();
  if (g_engine == nullptr) {
    return;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  VstDisk disk{};
  DWORD size = sizeof(disk);
  DWORD kind = 0;
  const LONG read =
      RegQueryValueExW(key, L"VstRack", nullptr, &kind, reinterpret_cast<BYTE*>(&disk), &size);
  RegCloseKey(key);
  if (read != ERROR_SUCCESS || kind != REG_BINARY || size != sizeof(disk) || disk.magic != kVstDiskMagic ||
      disk.version != 1) {
    return;
  }
  for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
    if (disk.slots[slot].used == 0 || disk.slots[slot].path[0] == 0) {
      continue;
    }
    std::wstring error;
    const int added = g_engine->AddVst(disk.slots[slot].strip, disk.slots[slot].path, error);
    if (added < 0) {
      wcsncpy(g_vstStatus, error.c_str(), 159);
      g_vstStatus[159] = 0;
      continue;
    }
    if (disk.slots[slot].bypass != 0) {
      g_engine->SetVstBypass(added, true);
    }
    const int savedGroup = g_vstSlotGroup[slot];
    if (savedGroup >= 0 && savedGroup < anymix::kVstGroups && g_vstGroup[savedGroup].used) {
      g_engine->SetVstGroup(added, savedGroup);
    }
  }
}

bool VstExt(const wchar_t* name, const wchar_t* ext) {
  const size_t n = wcslen(name);
  const size_t m = wcslen(ext);
  return n >= m && _wcsicmp(name + n - m, ext) == 0;
}

void RememberVstFile(const wchar_t* path) {
  if (g_vstFileCount >= 128 || path == nullptr) {
    return;
  }
  const wchar_t* slash = wcsrchr(path, L'\\');
  const wchar_t* leaf = slash != nullptr ? slash + 1 : path;
  wcsncpy(g_vstFiles[g_vstFileCount].path, path, 519);
  g_vstFiles[g_vstFileCount].path[519] = 0;
  wcsncpy(g_vstFiles[g_vstFileCount].name, leaf, 95);
  g_vstFiles[g_vstFileCount].name[95] = 0;
  wchar_t* dot = wcsrchr(g_vstFiles[g_vstFileCount].name, L'.');
  if (dot != nullptr) {
    *dot = 0;
  }
  ++g_vstFileCount;
}

void ScanVstDir(const wchar_t* dir, int depth) {
  if (dir == nullptr || depth > 4 || g_vstFileCount >= 128) {
    return;
  }
  wchar_t pattern[MAX_PATH];
  _snwprintf(pattern, MAX_PATH, L"%s\\*", dir);
  pattern[MAX_PATH - 1] = 0;
  WIN32_FIND_DATAW data{};
  const HANDLE found = FindFirstFileW(pattern, &data);
  if (found == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (data.cFileName[0] == L'.') {
      continue;
    }
    wchar_t full[MAX_PATH];
    _snwprintf(full, MAX_PATH, L"%s\\%s", dir, data.cFileName);
    full[MAX_PATH - 1] = 0;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      if (VstExt(data.cFileName, L".vst3")) {
        RememberVstFile(full);
      } else {
        ScanVstDir(full, depth + 1);
      }
    } else if (VstExt(data.cFileName, L".vst3") || VstExt(data.cFileName, L".dll")) {
      RememberVstFile(full);
    }
  } while (FindNextFileW(found, &data) && g_vstFileCount < 128);
  FindClose(found);
}

void ScanVstFolders() {
  g_vstFileCount = 0;
  g_vstScroll = 0;
  ScanVstDir(L"C:\\Program Files\\Common Files\\VST3", 0);
  ScanVstDir(L"C:\\Program Files\\Common Files\\VST2", 0);
  g_vstScanned = true;
  if (g_vstFileCount == 0) {
    wcsncpy(g_vstStatus, L"No plug-ins found in Common Files\\VST2 or VST3.", 159);
  } else {
    wsprintfW(g_vstStatus, L"%d plug-ins found. They load on the selected strip.", g_vstFileCount);
  }
  g_vstStatus[159] = 0;
}

int VstDropGroup(int x, int y) {
  POINT point{x, y};
  for (int group = 0; group < anymix::kVstGroups; ++group) {
    if (g_vstGroupRectOn[group] && PtInRect(&g_vstGroupRect[group], point)) {
      return group;
    }
  }
  if (g_vstLooseOn && PtInRect(&g_vstLooseRect, point)) {
    return -1;
  }
  return -2;
}

void DrawVstSlotRow(Canvas& g, RECT row, int slot, const anymix::VstSlotView& view, const Palette& pal, bool groupedOff) {
  FillRR(g, row, 8, pal.strip);
  const int handleW = std::max(80, static_cast<int>(row.right - row.left - 230));
  RECT handle = R(row.left, row.top, handleW, row.bottom - row.top);
  AddHot(handle, 8700 + slot, L"Drag this plug-in onto a group.");
  Text(g, view.name, R(row.left + 10, row.top, 160, 40), groupedOff ? pal.dim : pal.text, 13, false);
  Text(g, view.strip >= 0 ? StripLabel(view.strip) : L"", R(row.left + 174, row.top, 64, 40), pal.dim, 12, false);
  RECT bypass = R(row.right - 228, row.top + 6, 70, 28);
  RECT editor = R(row.right - 152, row.top + 6, 70, 28);
  RECT remove = R(row.right - 76, row.top + 6, 66, 28);
  DrawButton(g, bypass, L"Bypass", view.bypass, pal.warn, pal, g_hover == 8950 + slot);
  DrawButton(g, editor, view.editor ? L"Close" : L"Editor", view.editor, pal.accent, pal, g_hover == 8960 + slot);
  DrawButton(g, remove, L"Remove", false, pal.mute, pal, g_hover == 8970 + slot);
  AddHot(bypass, 8950 + slot, L"Bypass this plug-in.");
  AddHot(editor, 8960 + slot, L"Open or close the plug-in window.");
  AddHot(remove, 8970 + slot, L"Remove this plug-in.");
}

void PaintVst(Canvas& g, RECT area, const Palette& pal) {
  if (!g_vstScanned) {
    ScanVstFolders();
  }
  const int width = area.right - area.left;
  Text(g, L"VST", R(area.left + 16, area.top + 8, 200, 28), pal.text, 16, false);
  Text(g, L"Plug-ins run on the selected strip, before the external ASIO insert.",
       R(area.left + 16, area.top + 36, width - 32, 20), pal.dim, 12, false);
  const int chips[anymix::kStrips] = {anymix::kMic1, anymix::kMic2, anymix::kGame, anymix::kChat,
                                      anymix::kMusic, anymix::kAux, anymix::kAdd};
  for (int i = 0; i < anymix::kStrips; ++i) {
    RECT chip = R(area.left + 16 + i * 92, area.top + 64, 86, 28);
    DrawButton(g, chip, StripLabel(chips[i]), g_vstStrip == chips[i], pal.accent, pal,
               g_hover == 8810 + chips[i]);
    AddHot(chip, 8810 + chips[i], L"New plug-ins go on this strip.");
  }
  RECT rescan = R(area.right - 120, area.top + 64, 104, 28);
  DrawButton(g, rescan, L"Rescan", false, pal.accent, pal, g_hover == 8801);
  AddHot(rescan, 8801, L"Look again in the VST2 and VST3 folders.");

  const bool split = width > 780;
  const int rackRight = split ? area.left + width / 2 - 8 : area.right - 16;
  const int rackLeft = area.left + 16;
  const int rackW = rackRight - rackLeft;
  Text(g, L"Rack", R(rackLeft, area.top + 104, 80, 22), pal.text, 14, false);
  RECT makeGroup = R(rackLeft + 88, area.top + 100, 110, 26);
  DrawButton(g, makeGroup, L"New group", false, pal.accent, pal, g_hover == 8730);
  AddHot(makeGroup, 8730, L"Create a group, then drag plug-ins onto it.");
  for (int group = 0; group < anymix::kVstGroups; ++group) {
    g_vstGroupRectOn[group] = false;
  }
  g_vstLooseOn = false;
  int y = area.top + 136;
  anymix::VstSlotView loaded[anymix::kVstSlots]{};
  int shown = 0;
  if (g_engine != nullptr) {
    for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
      g_engine->VstSlot(slot, loaded[slot]);
      if (loaded[slot].used) {
        ++shown;
      }
    }
  }
  for (int group = 0; group < anymix::kVstGroups; ++group) {
    if (!g_vstGroup[group].used) {
      continue;
    }
    const int top = y;
    const bool hotGroup = g_vstDrag && g_vstHoverGroup == group;
    RECT header = R(rackLeft, y, rackW, 34);
    FillRR(g, header, 8, hotGroup ? pal.surface : pal.bg);
    const int nameW = std::max(48, static_cast<int>(header.right - 176 - header.left));
    RECT groupName = R(header.left + 8, header.top + 4, nameW, 26);
    Text(g, g_vstGroup[group].name, groupName, pal.text, 13, false);
    AddHot(groupName, 8760 + group, L"Right-click to rename this group.");
    RECT groupBypass = R(header.right - 158, header.top + 4, 72, 26);
    RECT groupRemove = R(header.right - 80, header.top + 4, 70, 26);
    DrawButton(g, groupBypass, L"Bypass", g_vstGroup[group].bypass, pal.warn, pal, g_hover == 8740 + group);
    DrawButton(g, groupRemove, L"Remove", false, pal.mute, pal, g_hover == 8750 + group);
    AddHot(groupBypass, 8740 + group, L"Bypass every plug-in in this group.");
    AddHot(groupRemove, 8750 + group, L"Remove this group and its plug-ins.");
    y += 40;
    int members = 0;
    for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
      if (!loaded[slot].used || loaded[slot].group != group) {
        continue;
      }
      ++members;
      DrawVstSlotRow(g, R(rackLeft + 12, y, rackW - 12, 40), slot, loaded[slot], pal, g_vstGroup[group].bypass);
      y += 46;
    }
    if (members == 0) {
      Text(g, L"Drop plug-ins here.", R(rackLeft + 12, y, rackW - 24, 22), pal.dim, 12, false);
      y += 28;
    }
    g_vstGroupRect[group] = R(rackLeft, top, rackW, y - top + 4);
    g_vstGroupRectOn[group] = true;
    StrokeRR(g, g_vstGroupRect[group], 8, hotGroup ? pal.accent : pal.line, hotGroup ? 1.8f : 1.f);
    y += 10;
  }
  bool anyGroup = false;
  for (int group = 0; group < anymix::kVstGroups; ++group) {
    if (g_vstGroup[group].used) {
      anyGroup = true;
    }
  }
  const int looseTop = y;
  int loose = 0;
  if (anyGroup) {
    Text(g, L"Not in a group", R(rackLeft, y, rackW, 20), pal.dim, 12, false);
    y += 24;
  }
  for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
    if (!loaded[slot].used || loaded[slot].group >= 0) {
      continue;
    }
    ++loose;
    DrawVstSlotRow(g, R(rackLeft, y, rackW, 40), slot, loaded[slot], pal, false);
    y += 46;
  }
  if (anyGroup && loose == 0) {
    Text(g, L"Drop a plug-in here to take it out of a group.", R(rackLeft, y, rackW, 20), pal.dim, 12, false);
    y += 26;
  }
  if (anyGroup || loose > 0) {
    g_vstLooseRect = R(rackLeft, looseTop, rackW, std::max(28, y - looseTop));
    g_vstLooseOn = true;
    if (g_vstDrag && g_vstHoverGroup == -1) {
      StrokeRR(g, g_vstLooseRect, 8, pal.accent, 1.6f);
    }
  }
  if (shown == 0) {
    Text(g, L"The rack is empty. Choose a plug-in from the list, or make a group and drop one in.",
         R(rackLeft, y, rackW, 36), pal.dim, 13, false);
    y += 40;
  }

  const int listLeft = split ? area.left + width / 2 + 8 : area.left + 16;
  const int listTop = split ? area.top + 104 : y + 12;
  Text(g, L"Installed", R(listLeft, listTop, 220, 22), pal.text, 14, false);
  const int rowH = 30;
  const int listBottom = area.bottom - 36;
  int visible = (listBottom - (listTop + 28)) / rowH;
  if (visible < 1) {
    visible = 1;
  }
  g_vstVisible = visible;
  if (g_vstScroll > std::max(0, g_vstFileCount - visible)) {
    g_vstScroll = std::max(0, g_vstFileCount - visible);
  }
  if (g_vstFileCount == 0) {
    Text(g, L"Nothing in Common Files\\VST2 or VST3.", R(listLeft, listTop + 28, area.right - listLeft - 16, 22),
         pal.dim, 12, false);
  }
  for (int i = 0; i < visible && g_vstScroll + i < g_vstFileCount; ++i) {
    const int index = g_vstScroll + i;
    const int rowW = area.right - listLeft - 16 > 40 ? static_cast<int>(area.right - listLeft - 16) : 40;
    RECT row = R(listLeft, listTop + 28 + i * rowH, rowW, rowH - 2);
    DrawButton(g, row, g_vstFiles[index].name, false, pal.accent, pal, g_hover == 8820 + index, 12.f);
    AddHot(row, 8820 + index, L"Click to load on the selected strip, or drag onto a group.");
  }
}

void PaintAsio(Canvas& g, RECT area, const Palette& pal) {
  Text(g, L"AnyMix Insert ASIO", R(area.left + 16, area.top + 12, 320, 26), pal.text, 16, false);
  const bool registered = AsioRegistered();
  const bool host = AsioHostRunning();
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* slash = wcsrchr(exe, L'\\');
  if (slash != nullptr) {
    *slash = 0;
  }
  wchar_t dll[MAX_PATH];
  wsprintfW(dll, L"%s\\AnyMixInsertASIO.dll", exe);
  const bool dllThere = GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES;
  Text(g, dllThere ? L"DLL present" : L"DLL missing", R(area.left + 16, area.top + 48, 220, 22),
       dllThere ? pal.ok : pal.clip, 13, false);
  Text(g, registered ? L"Registered" : L"Not registered", R(area.left + 240, area.top + 48, 220, 22),
       registered ? pal.ok : pal.warn, 13, false);
  Text(g, host ? L"Host running" : L"No ASIO host", R(area.left + 460, area.top + 48, 220, 22),
       host ? pal.ok : pal.dim, 13, false);
  const wchar_t* pairs[anymix::kStrips] = {L"1/2", L"3/4", L"5/6", L"7/8", L"9/10", L"11/12", L"13/14"};
  for (int i = 0; i < anymix::kStrips; ++i) {
    RECT row = R(area.left + 16, area.top + 96 + i * 40, 360, 34);
    FillRR(g, row, 8, pal.strip);
    Text(g, StripLabel(i), R(row.left + 12, row.top, 120, 36), pal.text, 14, false);
    Text(g, pairs[i], R(row.left + 140, row.top, 80, 36), pal.dim, 14, false);
    Text(g, host ? L"In use" : L"Idle", R(row.left + 240, row.top, 100, 36), host ? pal.ok : pal.dim, 13, false);
  }
  Text(g, L"Send and return follow the strip, including Mic 1 and Mic 2. Internal plug-ins are on the VST page.",
       R(area.left + 16, area.top + 390, 700, 22), pal.dim, 12, false);
  wchar_t linkStatus[160]{};
  int linkFault = 2;
  if (g_engine != nullptr) {
    g_engine->AsioLinkStatus(linkStatus, 160, linkFault);
  }
  const wchar_t* linkText = linkStatus[0] != 0 ? linkStatus : L"ASIO Link Pro offline";
  const Gdiplus::Color linkColor = linkFault == 1 ? pal.clip : (linkFault == 0 ? pal.ok : pal.warn);
  Text(g, linkText, R(area.left + 16, area.top + 416, 720, 22), linkColor, 13, false);
  if (!registered && dllThere) {
    RECT reg = R(area.left + 400, area.top + 96, 180, 36);
    DrawButton(g, reg, L"Register", false, pal.accent, pal, g_hover == 4101);
    AddHot(reg, 4101, L"Register AnyMix Insert ASIO. Windows asks for administrator approval.");
  }
}

void PaintSettings(Canvas& g, RECT area, const Palette& pal) {
  const bool wide = (area.right - area.left) > 520;
  RECT logo = R(area.left + 16, area.top + 8, wide ? 420 : 104, wide ? 128 : 104);
  DrawLogo(g, wide ? g_logoLong : g_logo, logo);
  Text(g, L"by Anyxir", R(area.left + 16, area.top + 140, 220, 22), pal.dim, 13, false);
  Text(g, L"Audio", R(area.left + 16, area.top + 172, 200, 22), pal.text, 15, false);
  wchar_t audio[128];
  wsprintfW(audio, L"%d Hz     %d samples     fixed by the engine", anymix::kSampleRate, anymix::kBlockFrames);
  Text(g, audio, R(area.left + 16, area.top + 198, 480, 22), pal.dim, 13, false);
  const wchar_t* err = g_engine->DeviceError();
  if (err != nullptr && err[0] != 0) {
    Text(g, err, R(area.left + 16, area.top + 222, area.right - area.left - 32, 20), pal.clip, 12, false);
  }
  Text(g, L"Interface", R(area.left + 16, area.top + 252, 200, 22), pal.text, 15, false);
  RECT dark = R(area.left + 16, area.top + 282, 90, 30);
  RECT light = R(area.left + 114, area.top + 282, 90, 30);
  DrawButton(g, dark, L"Dark", g_dark, pal.accent, pal, g_hover == 5001);
  DrawButton(g, light, L"Light", !g_dark, pal.accent, pal, g_hover == 5002);
  AddHot(dark, 5001, L"Dark theme.");
  AddHot(light, 5002, L"Light theme.");
  RECT comfort = R(area.left + 16, area.top + 322, 120, 30);
  RECT compact = R(area.left + 144, area.top + 322, 120, 30);
  DrawButton(g, comfort, L"Comfortable", !g_compact, pal.accent, pal, g_hover == 5003);
  DrawButton(g, compact, L"Compact", g_compact, pal.accent, pal, g_hover == 5004);
  AddHot(comfort, 5003, L"Larger mixer strips.");
  AddHot(compact, 5004, L"Denser mixer strips.");
  RECT anim = R(area.left + 16, area.top + 362, 160, 30);
  DrawButton(g, anim, g_anim ? L"Meter ballistics" : L"Meter instant", g_anim, pal.accent, pal, g_hover == 5005);
  AddHot(anim, 5005, L"Meter fall time. Levels still come from the engine.");
  RECT tray = R(area.left + 184, area.top + 362, 160, 30);
  DrawButton(g, tray, L"Close to tray", g_closeToTray, pal.accent, pal, g_hover == 5006);
  AddHot(tray, 5006, L"Closing the window hides AnyMix in the notification area. Right-click that icon and choose Quit to exit.");
  RECT boot = R(area.left + 16, area.top + 402, 200, 30);
  DrawButton(g, boot, L"Start with Windows", g_startWithWindows, pal.accent, pal, g_hover == 5007);
  AddHot(boot, 5007, L"Opens AnyMix as soon as you sign in, before other audio apps.");
  Text(g, L"Recording", R(area.left + 16, area.top + 452, 200, 22), pal.text, 15, false);
  Text(g, L"48 kHz stereo, saved as WAV, MP3, FLAC, AAC, or WMA", R(area.left + 16, area.top + 478, 460, 20),
       pal.dim, 13, false);
  Text(g, g_folder, R(area.left + 16, area.top + 500,
                     ClampInt(static_cast<int>(area.right - area.left - 32), 200, 520), 20), pal.text, 12,
       false);
}

int PopupCount() {
  if (g_pop >= 2000 && g_pop < 2100) {
    return static_cast<int>(g_captures.size());
  }
  if (g_pop >= 2100 && g_pop < 2200) {
    return static_cast<int>(g_renders.size());
  }
  if (g_pop == 4000) {
    return anymix::kBuses;
  }
  if (g_pop == 4007) {
    return kRecFormatCount;
  }
  if (g_pop >= 8100 && g_pop < 8105) {
    return static_cast<int>(g_sessions[static_cast<size_t>(g_pop - 8100)].size());
  }
  return 0;
}

void PaintPopup(Canvas& g, const Palette& pal, int clientW, int clientH) {
  if (g_pop == 0) {
    return;
  }
  const std::vector<anymix::DeviceInfo>* list = nullptr;
  if (g_pop >= 2000 && g_pop < 2100) {
    list = &g_captures;
  } else if (g_pop >= 2100 && g_pop < 2200) {
    list = &g_renders;
  }
  const int count = PopupCount();
  const int maxScroll = std::max(0, count - 12);
  if (g_popScroll < 0) {
    g_popScroll = 0;
  }
  if (g_popScroll > maxScroll) {
    g_popScroll = maxScroll;
  }
  const int rows = std::min(12, std::max(0, count - g_popScroll));
  const int boxH = std::max(36, rows * 28 + 8);
  int width = std::max(280, static_cast<int>(g_popAnchor.right - g_popAnchor.left));
  int left = g_popAnchor.left;
  if (left + width > clientW - 8) {
    left = std::max(8, clientW - 8 - width);
  }
  int top = g_popAnchor.bottom + 4;
  if (top + boxH > clientH - 8) {
    top = g_popAnchor.top - 4 - boxH;
  }
  if (top < 8) {
    top = 8;
  }
  RECT box{left, top, left + width, top + boxH};
  FillRR(g, box, 8, pal.bar);
  for (int i = 0; i < rows; ++i) {
    const int index = g_popScroll + i;
    RECT row{box.left + 4, box.top + 4 + i * 28, box.right - 4, box.top + 30 + i * 28};
    const wchar_t* name = L"";
    if (list != nullptr && index < static_cast<int>(list->size())) {
      name = (*list)[static_cast<size_t>(index)].name;
    } else if (g_pop >= 8100 && g_pop < 8105) {
      name = g_sessions[static_cast<size_t>(g_pop - 8100)][static_cast<size_t>(index)].name;
    } else if (g_pop == 4000 && index >= 0 && index < anymix::kBuses) {
      name = anymix::kBusNames[index];
    } else if (g_pop == 4007 && index >= 0 && index < kRecFormatCount) {
      name = kRecFormats[index].label;
    }
    if (g_hover == 3000 + i) {
      FillRR(g, row, 4, pal.surface);
    }
    Text(g, name, R(row.left + 8, row.top, row.right - row.left - 12, 26), pal.text, 13, false);
    AddHot(row, 3000 + i, L"Select. Wheel scrolls the list.");
  }
}

Gdiplus::Color Accent(int strip) {
  static const Gdiplus::Color colors[anymix::kStrips] = {
      Gdiplus::Color(255, 59, 130, 246),  Gdiplus::Color(255, 139, 92, 246),
      Gdiplus::Color(255, 52, 211, 153), Gdiplus::Color(255, 251, 146, 60),
      Gdiplus::Color(255, 34, 211, 238), Gdiplus::Color(255, 244, 114, 182),
      Gdiplus::Color(255, 251, 113, 133)};
  if (strip < 0 || strip >= anymix::kStrips) {
    return colors[0];
  }
  return colors[strip];
}

const wchar_t* CaptureChoice(int mic) {
  if (mic < 0 || mic >= anymix::kMicStrips) {
    return L"None";
  }
  if (g_micSel[mic] < 0) {
    return L"Offline";
  }
  return DeviceName(g_captures, g_micSel[mic]);
}

const wchar_t* RenderChoice(int bus) {
  if (bus < 0 || bus >= anymix::kPhysicalBuses) {
    return L"None";
  }
  if (g_outSel[bus] < 0) {
    return L"Offline";
  }
  return DeviceName(g_renders, g_outSel[bus]);
}

const wchar_t* AppLabel(int strip) {
  if (strip >= anymix::kVirtualStrips) {
    return CaptureChoice(strip - anymix::kVirtualStrips);
  }
  if (VirtualId(strip) == nullptr) {
    return L"Unavailable";
  }
  const auto& sessions = g_sessions[static_cast<size_t>(strip)];
  if (sessions.empty()) {
    return L"No application";
  }
  return sessions[0].name;
}

void UpperCopy(wchar_t* dst, int cap, const wchar_t* src) {
  if (cap <= 0) {
    return;
  }
  int i = 0;
  if (src != nullptr) {
    for (; src[i] != 0 && i < cap - 1; ++i) {
      wchar_t c = src[i];
      if (c >= L'a' && c <= L'z') {
        c = static_cast<wchar_t>(c - 32);
      }
      dst[i] = c;
    }
  }
  dst[i] = 0;
}

void DrawIcon(Canvas& g, int kind, float x, float y, float s, Gdiplus::Color color) {
  Gdiplus::Pen pen(color, std::max(1.35f, s * 0.09f));
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  Gdiplus::SolidBrush brush(color);
  auto X = [&](float u) { return x + u * s; };
  auto Y = [&](float v) { return y + v * s; };
  if (kind == 0) {
    Gdiplus::GraphicsPath body;
    AddRoundRect(body, RECT{static_cast<LONG>(X(0.02f)), static_cast<LONG>(Y(0.30f)),
                            static_cast<LONG>(X(0.98f)), static_cast<LONG>(Y(0.78f))},
                 static_cast<int>(s * 0.22f));
    g.DrawPath(&pen, &body);
    g.DrawLine(&pen, X(0.28f), Y(0.42f), X(0.28f), Y(0.66f));
    g.DrawLine(&pen, X(0.16f), Y(0.54f), X(0.40f), Y(0.54f));
    g.FillEllipse(&brush, X(0.62f), Y(0.42f), s * 0.10f, s * 0.10f);
    g.FillEllipse(&brush, X(0.76f), Y(0.54f), s * 0.10f, s * 0.10f);
  } else if (kind == 1) {
    Gdiplus::GraphicsPath bubble;
    AddRoundRect(bubble, RECT{static_cast<LONG>(X(0.08f)), static_cast<LONG>(Y(0.10f)),
                              static_cast<LONG>(X(0.92f)), static_cast<LONG>(Y(0.68f))},
                 static_cast<int>(s * 0.16f));
    g.DrawPath(&pen, &bubble);
    g.DrawLine(&pen, X(0.28f), Y(0.68f), X(0.20f), Y(0.90f));
    g.DrawLine(&pen, X(0.20f), Y(0.90f), X(0.48f), Y(0.68f));
  } else if (kind == 2) {
    g.DrawLine(&pen, X(0.64f), Y(0.16f), X(0.64f), Y(0.70f));
    g.FillEllipse(&brush, X(0.28f), Y(0.58f), s * 0.36f, s * 0.26f);
    g.DrawBezier(&pen, X(0.64f), Y(0.16f), X(0.86f), Y(0.20f), X(0.90f), Y(0.38f), X(0.70f), Y(0.42f));
  } else if (kind == 3) {
    g.DrawArc(&pen, X(0.18f), Y(0.02f), s * 0.64f, s * 0.58f, 200, 140);
    g.DrawLine(&pen, X(0.20f), Y(0.30f), X(0.20f), Y(0.52f));
    g.DrawLine(&pen, X(0.80f), Y(0.30f), X(0.80f), Y(0.52f));
    Gdiplus::GraphicsPath left;
    AddRoundRect(left, RECT{static_cast<LONG>(X(0.06f)), static_cast<LONG>(Y(0.50f)),
                            static_cast<LONG>(X(0.32f)), static_cast<LONG>(Y(0.92f))},
                 static_cast<int>(s * 0.1f));
    Gdiplus::GraphicsPath right;
    AddRoundRect(right, RECT{static_cast<LONG>(X(0.68f)), static_cast<LONG>(Y(0.50f)),
                             static_cast<LONG>(X(0.94f)), static_cast<LONG>(Y(0.92f))},
                 static_cast<int>(s * 0.1f));
    g.DrawPath(&pen, &left);
    g.DrawPath(&pen, &right);
  } else if (kind == 4) {
    g.DrawLine(&pen, X(0.50f), Y(0.16f), X(0.50f), Y(0.84f));
    g.DrawLine(&pen, X(0.16f), Y(0.50f), X(0.84f), Y(0.50f));
  } else {
    Gdiplus::GraphicsPath cap;
    AddRoundRect(cap, RECT{static_cast<LONG>(X(0.34f)), static_cast<LONG>(Y(0.06f)),
                           static_cast<LONG>(X(0.66f)), static_cast<LONG>(Y(0.50f))},
                 static_cast<int>(s * 0.14f));
    g.DrawPath(&pen, &cap);
    g.DrawArc(&pen, X(0.18f), Y(0.22f), s * 0.64f, s * 0.46f, 20, 140);
    g.DrawLine(&pen, X(0.50f), Y(0.68f), X(0.50f), Y(0.80f));
    g.DrawLine(&pen, X(0.32f), Y(0.86f), X(0.68f), Y(0.86f));
  }
}

void DrawBalance(Canvas& g, RECT r, float pan, const Palette& pal, bool hot) {
  FillRR(g, r, 5, g_dark ? Gdiplus::Color(255, 8, 18, 32) : pal.bg);
  StrokeRR(g, r, 5, hot ? pal.accent : pal.line, hot ? 1.4f : 1.f);
  const int cx = (r.left + r.right) / 2;
  const int cy = (r.top + r.bottom) / 2;
  Gdiplus::Pen hair(Fade(pal.dim, 160), 1.f);
  g.DrawLine(&hair, cx, r.top + 3, cx, r.bottom - 3);
  g.DrawLine(&hair, r.left + 3, cy, r.right - 3, cy);
  const int travel = std::max(1, static_cast<int>(r.right - r.left) / 2 - 5);
  const int dx = static_cast<int>(std::max(-1.f, std::min(1.f, pan)) * static_cast<float>(travel));
  const int dot = 4;
  RECT mark{cx + dx - dot, cy - dot, cx + dx + dot, cy + dot};
  FillRR(g, mark, 2, Gdiplus::Color(255, 232, 64, 74));
}

void DrawChannelBadge(Canvas& g, RECT card, int strip, const Palette& pal) {
  const Gdiplus::Color accent = Accent(strip);
  const float hover = g_stripMix[strip];
  const float level = std::min(1.f, g_meter[strip] * 3.2f);
  const bool muted = g_strip[strip].mute;
  const float energy = muted ? 0.12f : (0.28f + level * 0.85f + hover * 0.5f);
  const float bob =
      std::sin(AnimT() * 2.3 + strip * 0.65) * (0.45 + (muted ? 0.f : level) * 1.8 + hover * 0.8);
  const int w = card.right - card.left;
  RECT badge = R(card.left + 6, card.top + 6, w - 12, 40);
  for (int ring = 2; ring >= 1; --ring) {
    RECT halo{badge.left - ring * 2, badge.top - ring * 2, badge.right + ring * 2, badge.bottom + ring * 2};
    StrokeRR(g, halo, 12, Fade(accent, static_cast<int>(energy * (34.f - ring * 10.f))), 2.f);
  }
  FillRR(g, badge, 10, g_dark ? Gdiplus::Color(255, 6, 16, 34) : pal.bar);
  FillRR(g, badge, 10, Fade(accent, muted ? 16 : 22 + static_cast<int>(hover * 36.f + level * 30.f)));
  const int border = muted ? 80 : ClampInt(150 + static_cast<int>(hover * 80.f + level * 40.f), 0, 255);
  StrokeRR(g, badge, 10, Fade(accent, border), 1.6f + hover * 0.6f);
  if (g_strip[strip].solo && !muted) {
    const float spread = Pulse(3.4f, strip * 0.4f);
    RECT solo{badge.left - 2, badge.top - 2, badge.right + 2, badge.bottom + 2};
    StrokeRR(g, solo, 12, Fade(pal.solo, 60 + static_cast<int>(90 * spread)), 1.5f);
  }
  const float icon = 16.f;
  const Gdiplus::Color ink = muted ? pal.dim : accent;
  DrawIcon(g, strip, static_cast<float>(badge.left + 8),
           static_cast<float>(badge.top) + (40.f - icon) * 0.5f + static_cast<float>(bob) * 0.35f, icon, ink);
  wchar_t upper[32];
  UpperCopy(upper, 32, StripLabel(strip));
  const int badgeW = badge.right - badge.left;
  const int panH = 22;
  const int panW = std::max(64, std::min(120, badgeW - 86));
  RECT pan = R(badge.right - panW - 6, badge.top + (40 - panH) / 2, panW, panH);
  Text(g, upper, R(badge.left + 28, badge.top, std::max(8, static_cast<int>(pan.left - badge.left - 32)), 40), ink,
       11, false, true);
  RECT nameHit{badge.left, badge.top, pan.left - 2, badge.bottom};
  AddHot(nameHit, Sid(strip, kName), L"Right-click to rename this strip.");
  DrawBalance(g, pan, g_strip[strip].pan, pal, g_hover == Sid(strip, kPan) || g_drag == Sid(strip, kPan));
  AddHot(pan, Sid(strip, kPan), L"Balance. Drag left or right. Double-click returns to the center.");
}

void DrawStripApps(Canvas& g, RECT area, int strip, const Palette& pal, bool expanded) {
  AddHot(area, 8300 + strip,
         expanded ? L"Right-click to show the strip again."
                  : L"Right-click to fill this strip with its applications.");
  if (VirtualId(strip) == nullptr) {
    Text(g, L"Unavailable", area, pal.warn, expanded ? 13 : 11, false);
    return;
  }
  const auto& sessions = g_sessions[static_cast<size_t>(strip)];
  if (sessions.empty()) {
    Text(g, L"No application", area, pal.dim, expanded ? 13 : 11, false);
    return;
  }
  const int rowH = expanded ? 50 : 32;
  const int gap = expanded ? 8 : 4;
  int shown = 0;
  for (int i = 0; i < static_cast<int>(sessions.size()) && i < 31; ++i) {
    const int y = area.top + shown * (rowH + gap);
    const int left = static_cast<int>(sessions.size()) - i;
    if (y + rowH > area.bottom) {
      if (left > 0) {
        wchar_t more[24];
        wsprintfW(more, L"+%d", left);
        Text(g, more, R(area.left, std::min(y, static_cast<int>(area.bottom - 14)), area.right - area.left, 14), pal.dim,
             11, true);
      }
      break;
    }
    RECT line = R(area.left, y, area.right - area.left, rowH);
    DrawAppRow(g, line, strip, i, sessions[static_cast<size_t>(i)], pal, !expanded, true);
    ++shown;
  }
}

void DrawConsoleStrip(Canvas& g, RECT card, int strip, const Palette& pal) {
  const Gdiplus::Color accent = Accent(strip);
  const float hover = g_stripMix[strip];
  const float level = g_strip[strip].mute ? 0.f : std::min(1.f, g_meter[strip] * 3.2f);
  FillRR(g, card, 10, pal.strip);
  const bool dropHere = g_appDrag && strip == g_dropStrip && strip < anymix::kVirtualStrips;
  StrokeRR(g, card, 10, Fade(accent, dropHere ? 230 : static_cast<int>(28 + hover * 110 + level * 70)),
           dropHere ? 2.5f : 1.5f);
  DrawChannelBadge(g, card, strip, pal);
  const int x = card.left;
  const int w = card.right - card.left;
  if (g_appsOpen == strip && strip < anymix::kVirtualStrips) {
    DrawStripApps(g, R(x + 8, card.top + 54, w - 16, card.bottom - card.top - 62), strip, pal, true);
    return;
  }
  const int appH = strip < anymix::kVirtualStrips ? 116 : 28;
  const int meterTop = card.top + 66;
  const int below = strip >= anymix::kVirtualStrips ? 218 : 182;
  const int meterH = std::max(64, static_cast<int>(card.bottom - card.top - 66 - below - appH));
  const int meterGap = 1;
  const int faderW = 28;
  int barW = 24;
  const int left = x + 28;
  const int rightLimit = x + w - 6;
  if (left + barW * 2 + meterGap + 12 + faderW > rightLimit) {
    barW = std::max(8, (rightLimit - left - meterGap - 12 - faderW) / 2);
  }
  RECT meterL = R(left, meterTop, barW, meterH);
  RECT meterR = R(meterL.right + meterGap, meterTop, barW, meterH);
  const int marks[7] = {0, -6, -12, -18, -24, -30, -60};
  int lastMark = -1000;
  for (int i = 0; i < 7; ++i) {
    const float t = (static_cast<float>(marks[i]) + 60.f) / 60.f;
    const int y = meterL.top + static_cast<int>((1.f - t) * static_cast<float>(meterL.bottom - meterL.top));
    if (lastMark != -1000 && std::abs(y - lastMark) < 11) {
      continue;
    }
    lastMark = y;
    wchar_t label[8];
    wsprintfW(label, L"%d", marks[i]);
    Text(g, label, R(x + 1, y - 6, 26, 12), pal.dim, 8, true);
  }
  const bool muted = g_strip[strip].mute;
  DrawMeterPair(g, meterL, meterR, DbOf(g_meterL[strip]), DbOf(g_meterR[strip]), pal, muted);
  int faderX = meterR.right + 12;
  if (faderX + faderW > rightLimit) {
    faderX = rightLimit - faderW;
  }
  RECT fader = R(faderX, meterTop, faderW, meterH);
  DrawFader(g, fader, g_strip[strip].gain, pal, g_hover == Sid(strip, kFader) || g_drag == Sid(strip, kFader));
  AddHot(fader, Sid(strip, kFader), L"Fader, -60 to +24 dB. Wheel or arrows change gain. Double-click returns to 0 dB.");
  wchar_t gain[32];
  swprintf(gain, 32, L"%+.1f dB", static_cast<double>(g_strip[strip].gain));
  int y = meterTop + meterH + 2;
  Text(g, gain, R(x + 4, y, w - 8, 16), pal.text, 11, true);
  y += 18;
  RECT mute = R(x + 8, y, 28, 20);
  RECT mono = R(x + 40, y, (w - 78), 20);
  RECT solo = R(x + w - 36, y, 28, 20);
  DrawButton(g, mute, L"M", g_strip[strip].mute, pal.mute, pal, g_hover == Sid(strip, kMute));
  DrawButton(g, mono, L"Mono", g_strip[strip].mono, accent, pal, g_hover == Sid(strip, kMono));
  DrawButton(g, solo, L"S", g_strip[strip].solo, pal.solo, pal, g_hover == Sid(strip, kSolo));
  AddHot(mute, Sid(strip, kMute), L"Mute this strip.");
  AddHot(mono, Sid(strip, kMono), L"Mono.");
  AddHot(solo, Sid(strip, kSolo), L"Solo. Only one channel at a time. A outputs keep their own solo.");
  y += 24;
  wchar_t bass[24];
  wchar_t treble[24];
  swprintf(bass, 24, L"Bass %+.1f", static_cast<double>(g_strip[strip].bass));
  swprintf(treble, 24, L"Treble %+.1f", static_cast<double>(g_strip[strip].treble));
  Text(g, bass, R(x + 8, y, w - 16, 12), pal.dim, 10, false);
  RECT bassR = R(x + 8, y + 12, w - 16, 10);
  DrawEq(g, bassR, g_strip[strip].bass, pal);
  AddHot(bassR, Sid(strip, kBass), L"Bass boost. The low end comes forward without saturating. Double-click resets.");
  Text(g, treble, R(x + 8, y + 24, w - 16, 12), pal.dim, 10, false);
  RECT trebleR = R(x + 8, y + 36, w - 16, 10);
  DrawEq(g, trebleR, g_strip[strip].treble, pal);
  AddHot(trebleR, Sid(strip, kTreble), L"Treble. Brings the highs out without saturating. Double-click resets.");
  y += 52;
  const int pairGap = 4;
  const int pairW = std::max(28, (w - 16 - pairGap) / 2);
  RECT cantabile = R(x + 8, y, pairW, 20);
  RECT local = R(x + 8 + pairW + pairGap, y, pairW, 20);
  DrawButton(g, cantabile, L"Cantabile", g_strip[strip].insert, accent, pal, g_hover == Sid(strip, kInsert), 9.f);
  DrawButton(g, local, L"Local", g_strip[strip].local, accent, pal, g_hover == Sid(strip, kLocal), 9.f);
  AddHot(cantabile, Sid(strip, kInsert),
         g_strip[strip].insert ? L"VST Cantabile is on. Click to hear the strip without it."
                               : L"VST Cantabile. Sends this strip to Cantabile.");
  AddHot(local, Sid(strip, kLocal),
         g_strip[strip].local ? L"VST local is on. Click to bypass the plug-ins loaded in AnyMix."
                              : L"VST local. Plays the plug-ins loaded on this strip in the VST page.");
  y += 24;
  const int mic = MicOf(strip);
  if (mic >= 0) {
    const bool armed = g_micGate[mic].level >= 0.05f;
    const bool open = armed && g_gateOpen[mic];
    Gdiplus::SolidBrush ledBrush(armed && open ? pal.ok : (armed ? pal.warn : pal.line));
    g.FillEllipse(&ledBrush, static_cast<float>(x + 8), static_cast<float>(y + 2), 8.f, 8.f);
    AddHot(R(x + 6, y, 12, 14), Sid(strip, kGateLed), open ? L"Gate open." : L"Gate closed.");
    Text(g, L"Gate", R(x + 20, y, 40, 12), pal.dim, 10, false);
    wchar_t value[16];
    swprintf(value, 16, L"%.1f", static_cast<double>(g_micGate[mic].level));
    Text(g, value, R(x + w - 72, y, 64, 12), pal.text, 10, true);
    Text(g, L"0", R(x + 8, y + 14, 12, 10), pal.dim, 9, false);
    Text(g, L"10", R(x + w - 22, y + 14, 14, 10), pal.dim, 9, true);
    RECT bar = R(x + 22, y + 16, w - 46, 8);
    DrawDepth(g, bar, g_micGate[mic].level / 10.f, pal);
    AddHot(bar, Sid(strip, kGateLevel),
           L"Noise gate, 0 to 10. 0 is off. Higher quiets the room and keeps the voice. Double-click turns it off.");
    y += 32;
  }
  const int routeCols = 5;
  const int routeGap = 3;
  const int routeSpan = std::max(routeCols, w - 16);
  const int routeW = std::max(18, (routeSpan - (routeCols - 1) * routeGap) / routeCols);
  const int routeH = 20;
  for (int bus = 0; bus < anymix::kBuses; ++bus) {
    const int col = bus % routeCols;
    const int row = bus / routeCols;
    RECT btn = R(x + 8 + col * (routeW + routeGap), y + row * (routeH + 3), routeW, routeH);
    DrawButton(g, btn, anymix::kBusNames[bus], g_strip[strip].route[bus], row == 0 ? accent : pal.solo, pal,
               g_hover == Sid(strip, kRoute0 + bus), routeW >= 32 ? 11.f : 9.f);
    AddHot(btn, Sid(strip, kRoute0 + bus), row == 0 ? L"Route to this A output." : L"Route to this B bus.");
  }
  const int appTop = card.bottom - appH + 4;
  if (strip < anymix::kVirtualStrips) {
    DrawStripApps(g, R(x + 8, appTop, w - 16, appH - 4), strip, pal, false);
  } else {
    RECT mic = R(x + 8, appTop, w - 16, 20);
    Text(g, CaptureChoice(strip - anymix::kVirtualStrips), mic, pal.dim, 11, false);
    AddHot(mic, 2000 + (strip - anymix::kVirtualStrips), L"Choose the Windows capture device.");
  }
}

void DrawMatrix(Canvas& g, RECT area, const Palette& pal) {
  FillRR(g, area, 10, pal.strip);
  Text(g, L"Routing", R(area.left + 18, area.top + 14, area.right - area.left - 36, 24), pal.text, 16, false);
  const int cols = anymix::kBuses;
  const int rows = anymix::kStrips;
  const int labelW = 118;
  const int headerH = 28;
  const int gap = 8;
  const int gridLeft = area.left + 18 + labelW;
  const int gridTop = area.top + 52 + headerH;
  const int gridW = std::max(cols, static_cast<int>(area.right - 18 - gridLeft));
  const int gridH = std::max(rows, static_cast<int>(area.bottom - 18 - gridTop));
  const int cellW = std::max(28, (gridW - (cols - 1) * gap) / cols);
  const int cellH = std::max(28, (gridH - (rows - 1) * gap) / rows);
  const float nameSize = cellH >= 48 ? 15.f : 13.f;
  for (int bus = 0; bus < cols; ++bus) {
    Text(g, anymix::kBusNames[bus], R(gridLeft + bus * (cellW + gap), gridTop - headerH, cellW, headerH), pal.dim,
         nameSize, true);
  }
  for (int strip = 0; strip < rows; ++strip) {
    const int y = gridTop + strip * (cellH + gap);
    Text(g, StripLabel(strip), R(area.left + 18, y, labelW - 12, cellH), pal.text, nameSize, false);
    for (int bus = 0; bus < cols; ++bus) {
      RECT btn = R(gridLeft + bus * (cellW + gap), y, cellW, cellH);
      if (g_strip[strip].route[bus]) {
        FillRR(g, btn, 8, Accent(strip));
      } else {
        FillRR(g, btn, 8, pal.bg);
        StrokeRR(g, btn, 8, pal.line, 1.f);
      }
      AddHot(btn, Sid(strip, kRoute0 + bus), L"Toggle this route.");
    }
  }
}

void DrawDevicePanel(Canvas& g, RECT area, const Palette& pal) {
  FillRR(g, area, 10, pal.strip);
  Text(g, L"Devices", R(area.left + 10, area.top + 6, 120, 16), pal.dim, 11, false);
  for (int i = 0; i < anymix::kMicStrips; ++i) {
    const int y = area.top + 28 + i * 28;
    const bool ok = g_micSel[i] > 0 && DeviceStillThere(g_captures, g_micSel[i]);
    Gdiplus::SolidBrush dot(ok ? pal.ok : pal.dim);
    g.FillEllipse(&dot, area.left + 10, y + 5, 8, 8);
    wchar_t label[16];
    wsprintfW(label, L"Mic %d", i + 1);
    Text(g, label, R(area.left + 24, y, 42, 18), pal.dim, 11, false);
    Text(g, CaptureChoice(i), R(area.left + 68, y, area.right - area.left - 78, 18), pal.text, 11,
         false);
    AddHot(R(area.left + 8, y, area.right - area.left - 16, 20), 2000 + i, L"Choose this microphone.");
  }
  for (int i = 0; i < anymix::kPhysicalBuses; ++i) {
    const int y = area.top + 88 + i * 22;
    const bool ok = g_outSel[i] > 0 && DeviceStillThere(g_renders, g_outSel[i]);
    Gdiplus::SolidBrush dot(ok ? pal.ok : pal.dim);
    g.FillEllipse(&dot, area.left + 10, y + 4, 8, 8);
    Text(g, anymix::kBusNames[i], R(area.left + 24, y, 28, 16), pal.dim, 11, false);
    Text(g, RenderChoice(i), R(area.left + 54, y, area.right - area.left - 64, 16), pal.text, 11,
         false);
    AddHot(R(area.left + 8, y, area.right - area.left - 16, 18), 2100 + i, L"Choose this output.");
  }
}

void DrawAppPanel(Canvas& g, RECT area, const Palette& pal) {
  FillRR(g, area, 10, pal.strip);
  Text(g, L"Applications", R(area.left + 10, area.top + 6, 120, 16), pal.dim, 11, false);
  const int tabW = std::max(34, static_cast<int>(area.right - area.left - 24) / anymix::kVirtualStrips);
  for (int i = 0; i < anymix::kVirtualStrips; ++i) {
    RECT tab = R(area.left + 8 + i * tabW, area.top + 26, tabW - 4, 18);
    DrawButton(g, tab, StripLabel(i), g_appTab == i, Accent(i), pal, g_hover == 8200 + i);
    AddHot(tab, 8200 + i, L"Show applications for this strip.");
  }
  const int strip = g_appTab;
  if (VirtualId(strip) == nullptr) {
    Text(g, L"Unavailable", R(area.left + 10, area.top + 54, area.right - area.left - 20, 20), pal.warn, 12, false);
    return;
  }
  const auto& sessions = g_sessions[static_cast<size_t>(strip)];
  if (sessions.empty()) {
    Text(g, L"No application", R(area.left + 10, area.top + 54, area.right - area.left - 20, 20), pal.dim, 12, false);
    return;
  }
  for (int i = 0; i < static_cast<int>(sessions.size()) && i < 5; ++i) {
    RECT line = R(area.left + 10, area.top + 52 + i * 24, area.right - area.left - 20, 22);
    DrawAppRow(g, line, strip, i, sessions[static_cast<size_t>(i)], pal, false);
  }
}

void DrawRecorderPanel(Canvas& g, RECT area, const Palette& pal) {
  FillRR(g, area, 10, pal.strip);
  Text(g, L"Recorder", R(area.left + 10, area.top + 6, 80, 16), pal.dim, 11, false);
  const bool rec = g_engine->Recording();
  ULONGLONG sec = 0;
  if (rec && g_recStart != 0) {
    sec = (GetTickCount64() - g_recStart) / 1000;
  }
  wchar_t clock[32];
  swprintf(clock, 32, L"%02u:%02u:%02u", static_cast<unsigned>(sec / 3600),
           static_cast<unsigned>((sec / 60) % 60), static_cast<unsigned>(sec % 60));
  Gdiplus::SolidBrush dot(rec ? pal.clip : pal.dim);
  g.FillEllipse(&dot, area.left + 10, area.top + 30, 8, 8);
  Text(g, rec ? L"Recording" : L"Stopped", R(area.left + 24, area.top + 24, 80, 18), pal.text, 12, false);
  Text(g, clock, R(area.left + 110, area.top + 24, 80, 18), pal.text, 12, false);
  RECT bus = R(area.left + 10, area.top + 48, 70, 20);
  DrawButton(g, bus, anymix::kBusNames[g_recBus], false, pal.accent, pal, g_hover == 4000);
  AddHot(bus, 4000, L"Recording bus.");
  RECT record = R(area.left + 86, area.top + 48, 58, 20);
  RECT stop = R(area.left + 148, area.top + 48, 50, 20);
  DrawButton(g, record, L"Rec", rec, pal.clip, pal, g_hover == 4001);
  DrawButton(g, stop, L"Stop", false, pal.accent, pal, g_hover == 4002);
  AddHot(record, 4001, L"Record the selected bus as WAV.");
  AddHot(stop, 4002, L"Stop and finalize the WAV file.");
  const wchar_t* path = rec ? g_engine->RecordingPath() : g_folder;
  Text(g, path, R(area.left + 10, area.top + 74, area.right - area.left - 20, 32), pal.dim, 10, false);
  AddHot(R(area.left + 10, area.top + 74, area.right - area.left - 20, 32), 4003, L"Choose the recording folder.");
  const bool registered = AsioRegistered();
  const bool host = AsioHostRunning();
  Text(g, L"ASIO Insert", R(area.left + 10, area.top + 110, 120, 16), pal.dim, 11, false);
  Text(g, registered ? (host ? L"Host running" : L"Registered") : L"Not registered",
       R(area.left + 110, area.top + 110, area.right - area.left - 120, 16), registered ? pal.ok : pal.warn, 11,
       false);
  const wchar_t* pairs[5] = {L"1/2", L"3/4", L"5/6", L"7/8", L"9/10"};
  for (int i = 0; i < 5; ++i) {
    wchar_t line[32];
    wsprintfW(line, L"%s  %s", StripLabel(i), pairs[i]);
    Text(g, line, R(area.left + 10, area.top + 128 + i * 14, area.right - area.left - 20, 14),
         host ? pal.text : pal.dim, 10, false);
  }
}

void DrawBusMaster(Canvas& g, RECT card, int bus, const Palette& pal) {
  const bool hardware = bus < anymix::kPhysicalBuses;
  const float hover = g_busMix[bus];
  const float lit = g_busMute[bus] ? 0.f : std::min(1.f, g_busMeter[bus] * 3.2f);
  const bool muted = g_busMute[bus];
  FillRR(g, card, 10, pal.strip);
  StrokeRR(g, card, 10, Fade(pal.accent, static_cast<int>(28 + hover * 100 + lit * 80)), 1.5f);
  const int w = card.right - card.left;
  RECT pill = R(card.left + 6, card.top + 6, w - 12, 26);
  const float energy = muted ? 0.1f : (0.25f + lit * 0.8f + hover * 0.45f);
  StrokeRR(g, RECT{pill.left - 2, pill.top - 2, pill.right + 2, pill.bottom + 2}, 10,
           Fade(pal.accent, static_cast<int>(energy * 40.f)), 2.f);
  FillRR(g, pill, 8, g_dark ? Gdiplus::Color(255, 6, 16, 34) : pal.bar);
  FillRR(g, pill, 8, Fade(pal.accent, muted ? 14 : 20 + static_cast<int>(hover * 30.f + lit * 28.f)));
  StrokeRR(g, pill, 8, Fade(pal.accent, muted ? 70 : 160 + static_cast<int>(hover * 60.f)), 1.5f);
  Text(g, anymix::kBusNames[bus], pill, muted ? pal.dim : pal.accent, 12, true, true);
  const int buttonH = 18;
  const int rows = hardware ? 3 : 2;
  const int nameH = hardware ? 22 : 0;
  const int meterTop = card.top + 52;
  const int below = 16 + rows * (buttonH + 2) + nameH + 6;
  const int meterH = std::max(24, static_cast<int>(card.bottom - meterTop - below));
  const int meterGap = 1;
  int barW = 24;
  RECT fader = R(card.left + 8, meterTop, 22, meterH);
  const int meterRight = card.right - 8;
  if (fader.right + 8 + barW * 2 + meterGap > meterRight) {
    barW = std::max(8, static_cast<int>(meterRight - fader.right - 8 - meterGap) / 2);
  }
  RECT meterR = R(meterRight - barW, meterTop, barW, meterH);
  RECT meterL = R(meterR.left - meterGap - barW, meterTop, barW, meterH);
  DrawFader(g, fader, g_busDb[bus], pal, g_hover == 7000 + bus || g_drag == 7000 + bus);
  AddHot(fader, 7000 + bus, hardware ? L"Master volume for this output. Double-click returns to 0 dB."
                                    : L"Master volume for this bus. Double-click returns to 0 dB.");
  DrawMeterPair(g, meterL, meterR, DbOf(g_busMeterL[bus]), DbOf(g_busMeterR[bus]), pal, muted);
  wchar_t level[32];
  swprintf(level, 32, L"%+.0f", static_cast<double>(g_busDb[bus]));
  int y = meterTop + meterH + 1;
  Text(g, level, R(card.left + 4, y, w - 8, 14), pal.text, 11, true);
  y += 15;
  RECT mute = R(card.left + 6, y, w - 12, buttonH);
  RECT mono = R(card.left + 6, y + buttonH + 2, w - 12, buttonH);
  DrawButton(g, mute, L"Mute", g_busMute[bus], pal.mute, pal, g_hover == 7100 + bus, 10.f);
  DrawButton(g, mono, L"Mono", g_busMono[bus], pal.accent, pal, g_hover == 7200 + bus, 10.f);
  AddHot(mute, 7100 + bus, hardware ? L"Mute this output." : L"Mute this bus.");
  AddHot(mono, 7200 + bus, L"Mono. Left and right are summed.");
  if (hardware) {
    RECT solo = R(card.left + 6, y + (buttonH + 2) * 2, w - 12, buttonH);
    DrawButton(g, solo, L"Solo", g_busSolo[bus], pal.solo, pal, g_hover == 7300 + bus, 10.f);
    AddHot(solo, 7300 + bus, L"Solo. Only one A output at a time. Channel solos stay separate.");
    const bool ok = g_outSel[bus] > 0 && DeviceStillThere(g_renders, g_outSel[bus]);
    Gdiplus::SolidBrush dot(ok ? pal.ok : pal.dim);
    g.FillEllipse(&dot, card.left + 8, card.bottom - 18, 7, 7);
    RECT name = R(card.left + 18, card.bottom - 22, w - 26, 16);
    Text(g, RenderChoice(bus), name, pal.dim, 10, false);
    AddHot(R(card.left + 6, card.bottom - 24, w - 12, 20), 2100 + bus, L"Choose this output.");
  }
}

void CaptionRects(int width, RECT& miniR, RECT& minR, RECT& maxR, RECT& closeR);
void EnterMini();
void LeaveMini();
LRESULT CALLBACK MiniProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

void DrawCaptionPlate(Canvas& g, RECT button, Gdiplus::Color color) {
  const int w = button.right - button.left;
  const int h = button.bottom - button.top;
  FillRR(g, R(button.left + 8, button.top + (h - 28) / 2, w - 16, 28), 8, color);
}

void DrawCaptionIcon(Canvas& g, RECT button, int kind, bool zoomed, Gdiplus::Color ink, Gdiplus::Color plate) {
  const int cx = (button.left + button.right) / 2;
  const int cy = (button.top + button.bottom) / 2;
  const float x = static_cast<float>(cx - 5);
  const float y = static_cast<float>(cy - 5);
  Gdiplus::Pen pen(ink, 1.35f);
  if (kind == 1) {
    g.DrawLine(&pen, x, static_cast<float>(cy), x + 10.f, static_cast<float>(cy));
  } else if (kind == 3) {
    g.DrawLine(&pen, x + 1.f, y + 1.f, x + 9.f, y + 9.f);
    g.DrawLine(&pen, x + 9.f, y + 1.f, x + 1.f, y + 9.f);
  } else if (kind == 2 && zoomed) {
    StrokeRR(g, R(cx - 2, cy - 5, 7, 7), 1, ink, 1.35f);
    FillRR(g, R(cx - 5, cy - 2, 7, 7), 1, plate);
    StrokeRR(g, R(cx - 5, cy - 2, 7, 7), 1, ink, 1.35f);
  } else if (kind == 2) {
    StrokeRR(g, R(cx - 5, cy - 5, 10, 10), 2, ink, 1.35f);
  } else {
    StrokeRR(g, R(cx - 5, cy - 4, 10, 8), 2, ink, 1.35f);
    g.DrawLine(&pen, x + 2.f, y + 3.f, x + 8.f, y + 3.f);
  }
}

void Paint(HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  const int w = std::max(1, static_cast<int>(client.right));
  const int h = std::max(1, static_cast<int>(client.bottom));
  if (!EnsureTarget(window, w, h)) {
    return;
  }
  g_rt->BeginDraw();
  Canvas g{g_rt};
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
  const Palette pal = Colors();
  g_hot.clear();
  Gdiplus::SolidBrush bg(pal.bg);
  g.FillRectangle(&bg, 0, 0, w, h);

  const bool wide = w >= 1180;
  const int side = wide ? 196 : 68;
  const int topH = 48;
  FillRR(g, R(0, 0, side, h), 0, pal.bar);
  RECT logoBox = wide ? R(8, 8, side - 16, 108) : R(6, 8, side - 12, side - 12);
  DrawLogo(g, wide ? g_logoLong : g_logo, logoBox);
  const wchar_t* names[kPageCount] = {L"Mixer", L"Routing", L"Devices", L"Applications", L"Recorder", L"ASIO",
                                      L"Settings", L"VST"};
  const wchar_t* shortNames[kPageCount] = {L"Mix", L"Map", L"Dev", L"App", L"Rec", L"ASIO", L"Set", L"VST"};
  const Page navOrder[kPageCount] = {kMixer, kRouting, kVst, kDevices, kApps, kRecord, kAsio, kSettings};
  for (int i = 0; i < kPageCount; ++i) {
    const int page = navOrder[i];
    RECT item = R(10, (wide ? 126 : 80) + i * 36, side - 20, 32);
    const bool on = g_page == static_cast<Page>(page);
    const float mix = g_navMix[page];
    if (mix > 0.02f) {
      FillRR(g, item, 8, Fade(on ? pal.accent : pal.surface, static_cast<int>(18 + mix * (on ? 36.f : 28.f))));
    }
    if (on) {
      Gdiplus::SolidBrush markBrush(pal.accent);
      g.FillRectangle(&markBrush, item.left, item.top + 6, 3, item.bottom - item.top - 12);
    }
    Text(g, wide ? names[page] : shortNames[page], R(item.left + 12, item.top, item.right - item.left - 16, 32),
         on ? pal.text : pal.dim, 13, false);
    AddHot(item, NavId(page), names[page]);
  }
  RECT dark = wide ? R(12, h - 92, (side - 28) / 2, 26) : R(8, h - 118, side - 16, 24);
  RECT light = wide ? R(dark.right + 4, h - 92, (side - 28) / 2, 26) : R(8, h - 90, side - 16, 24);
  DrawButton(g, dark, wide ? L"Dark" : L"Dk", g_dark, pal.accent, pal, g_hover == 5001);
  DrawButton(g, light, wide ? L"Light" : L"Lt", !g_dark, pal.accent, pal, g_hover == 5002);
  AddHot(dark, 5001, L"Dark theme.");
  AddHot(light, 5002, L"Light theme.");
  if (wide) {
    Text(g, L"Your Audio. Your Control.", R(12, h - 58, side - 24, 16), pal.dim, 10, false);
    Text(g, L"v1.0.0", R(12, h - 40, side - 24, 16), pal.dim, 10, false);
  }

  FillRR(g, R(side, 0, w - side, topH), 0, pal.bg);
  const bool online = g_engineOn && g_engine->Running();
  const wchar_t* err = g_engine->DeviceError();
  const bool fault = err != nullptr && err[0] != 0;
  wchar_t linkStatus[160]{};
  int linkFault = 2;
  g_engine->AsioLinkStatus(linkStatus, 160, linkFault);
  const Gdiplus::Color live = !online ? pal.clip : (linkFault == 1 ? pal.clip : (fault ? pal.warn : pal.ok));
  Gdiplus::SolidBrush dot(live);
  g.FillEllipse(&dot, side + 16, 19, 9, 9);
  Text(g, online ? L"Engine Online" : L"Engine Offline", R(side + 32, 8, 130, 32), pal.text, 13, false);
  wchar_t rate[16];
  wchar_t block[16];
  wsprintfW(rate, L"%d kHz", anymix::kSampleRate / 1000);
  wsprintfW(block, L"%d", anymix::kBlockFrames);
  Text(g, rate, R(side + 180, 4, 70, 22), pal.text, 13, false);
  Text(g, L"Sample Rate", R(side + 180, 22, 80, 16), pal.dim, 10, false);
  Text(g, block, R(side + 270, 4, 50, 22), pal.text, 13, false);
  Text(g, L"Buffer Size", R(side + 270, 22, 80, 16), pal.dim, 10, false);
  const bool driver = g_engine->DriverConnected();
  const wchar_t* linkLabel = linkFault == 1 ? L"ASIO Link error" : (linkFault == 0 ? L"ASIO Link OK" : L"ASIO Link offline");
  const Gdiplus::Color linkColor = linkFault == 1 ? pal.clip : (linkFault == 0 ? pal.ok : pal.warn);
  RECT linkBox = R(w - 430, 8, 220, 32);
  Text(g, linkLabel, linkBox, linkColor, 12, true);
  AddHot(linkBox, 4190, linkStatus[0] != 0 ? linkStatus : (driver ? L"ASIO Link Pro is connected." : L"ASIO Link Pro is not running."));
  RECT miniR{};
  RECT minR{};
  RECT maxR{};
  RECT closeR{};
  CaptionRects(w, miniR, minR, maxR, closeR);
  const bool zoomed = IsZoomed(window);
  const Gdiplus::Color iconInk = pal.text;
  const Gdiplus::Color closeInk = g_captionHot == 3 ? Gdiplus::Color(255, 255, 255, 255) : pal.text;
  if (g_captionHot == 4) {
    DrawCaptionPlate(g, miniR, pal.surface);
  }
  if (g_captionHot == 1) {
    DrawCaptionPlate(g, minR, pal.surface);
  }
  if (g_captionHot == 2) {
    DrawCaptionPlate(g, maxR, pal.surface);
  }
  if (g_captionHot == 3) {
    DrawCaptionPlate(g, closeR, pal.mute);
  }
  DrawCaptionIcon(g, miniR, 4, false, iconInk, pal.bg);
  DrawCaptionIcon(g, minR, 1, false, iconInk, pal.bg);
  DrawCaptionIcon(g, maxR, 2, zoomed, iconInk, g_captionHot == 2 ? pal.surface : pal.bg);
  DrawCaptionIcon(g, closeR, 3, false, closeInk, g_captionHot == 3 ? pal.mute : pal.bg);
  AddHot(miniR, 5200, L"Mini mixer");

  if (g_page == kMixer) {
    const int mixerTop = topH + 8;
    const int mixerH = h - topH - 36;
    const int masterW = 100;
    const int gap = 8;
    const int mastersW = anymix::kPhysicalBuses * masterW + (anymix::kPhysicalBuses - 1) * gap;
    const int shown = 6;
    const int viewW = std::max(0, w - side - mastersW - 24);
    const int gaps = (shown - 1) * gap;
    const int stripW = viewW > gaps ? (viewW - gaps) / shown : 1;
    g_scroll = 0;
    g_mixWidth = viewW;
    const int order[6] = {anymix::kMic1, anymix::kGame, anymix::kChat, anymix::kMusic, anymix::kAux, anymix::kAdd};
    for (int slot = 0; slot < shown; ++slot) {
      const int strip = order[slot];
      const int x = side + 8 + slot * (stripW + gap);
      if (x + stripW < side || x > w - mastersW - 16) {
        continue;
      }
      DrawConsoleStrip(g, R(x, mixerTop, stripW, mixerH - 8), strip, pal);
    }
    const int rowGap = 8;
    const int cardH = std::max(120, (mixerH - 8 - rowGap) / 2);
    for (int bus = 0; bus < anymix::kPhysicalBuses; ++bus) {
      const int x = w - mastersW - 8 + bus * (masterW + gap);
      DrawBusMaster(g, R(x, mixerTop, masterW, cardH), bus, pal);
      DrawBusMaster(g, R(x, mixerTop + cardH + rowGap, masterW, cardH), anymix::kPhysicalBuses + bus, pal);
    }
  } else {
    RECT page{side + 8, topH + 8, w - 8, h - 8};
    if (g_page == kRouting) {
      DrawMatrix(g, page, pal);
    } else if (g_page == kDevices) {
      PaintDevices(g, page, pal);
    } else if (g_page == kApps) {
      PaintApps(g, page, pal);
    } else if (g_page == kRecord) {
      PaintRecord(g, page, pal);
    } else if (g_page == kAsio) {
      PaintAsio(g, page, pal);
    } else if (g_page == kVst) {
      PaintVst(g, page, pal);
    } else {
      PaintSettings(g, page, pal);
    }
  }
  if (g_pageAlpha < 0.995f) {
    const int veil = static_cast<int>((1.f - g_pageAlpha) * 255.f);
    Gdiplus::SolidBrush shade(Gdiplus::Color(static_cast<BYTE>(veil), pal.bg.GetR(), pal.bg.GetG(), pal.bg.GetB()));
    g.FillRectangle(&shade, side, topH, w - side, h - topH);
  }
  if (g_appDrag && g_appName[0] != 0) {
    RECT chip = R(g_appX + 14, g_appY + 14, 168, 30);
    FillRR(g, chip, 8, pal.surface);
    StrokeRR(g, chip, 8, Accent(g_appFrom >= 0 ? g_appFrom : 0), 1.5f);
    Text(g, g_appName, R(chip.left + 8, chip.top, 152, 30), pal.text, 12, false);
  }
  if (g_vstDrag && g_vstDragName[0] != 0) {
    RECT chip = R(g_vstDragX + 12, g_vstDragY + 12, 180, 30);
    FillRR(g, chip, 8, pal.surface);
    StrokeRR(g, chip, 8, pal.accent, 1.5f);
    Text(g, g_vstDragName, R(chip.left + 8, chip.top, 164, 30), pal.text, 12, false);
  }
  PaintPopup(g, pal, w, h);

  const wchar_t* foot = g_hint;
  if (g_page == kVst && g_hover == 0 && g_vstStatus[0] != 0) {
    foot = g_vstStatus;
  }
  Text(g, foot, R(side + 12, h - 28, w - side - 24, 22), pal.dim, 12, false);

  const HRESULT drawn = g_rt->EndDraw();
  if (drawn == static_cast<HRESULT>(0x8899000CL)) {
    DropTarget();
  }
}

int HitAt(int x, int y) {
  POINT p{x, y};
  for (int i = static_cast<int>(g_hot.size()) - 1; i >= 0; --i) {
    if (PtInRect(&g_hot[static_cast<size_t>(i)].r, p)) {
      return g_hot[static_cast<size_t>(i)].id;
    }
  }
  return 0;
}

const Hot* HotOf(int id) {
  for (int i = static_cast<int>(g_hot.size()) - 1; i >= 0; --i) {
    if (g_hot[static_cast<size_t>(i)].id == id) {
      return &g_hot[static_cast<size_t>(i)];
    }
  }
  return nullptr;
}

void ApplySessionVolume(int x) {
  if (g_drag < 9000 || g_drag >= 9000 + anymix::kVirtualStrips * 32) {
    return;
  }
  const int strip = (g_drag - 9000) / 32;
  const int row = (g_drag - 9000) % 32;
  const Hot* hot = HotOf(g_drag);
  if (hot == nullptr || strip < 0 || strip >= anymix::kVirtualStrips) {
    return;
  }
  auto& sessions = g_sessions[static_cast<size_t>(strip)];
  if (row < 0 || row >= static_cast<int>(sessions.size())) {
    return;
  }
  const int width = std::max(1, static_cast<int>(hot->r.right - hot->r.left));
  float level = static_cast<float>(x - hot->r.left) / static_cast<float>(width);
  if (level < 0.f) {
    level = 0.f;
  } else if (level > 1.f) {
    level = 1.f;
  }
  sessions[static_cast<size_t>(row)].volume = level;
  const wchar_t* endpoint = VirtualId(strip);
  if (endpoint != nullptr) {
    std::wstring error;
    anymix::SetSessionVolume(endpoint, sessions[static_cast<size_t>(row)].sessionId, level, error);
  }
}

void ApplyDrag(int x, int y) {
  if (g_drag >= 9000 && g_drag < 9000 + anymix::kVirtualStrips * 32) {
    ApplySessionVolume(x);
    return;
  }
  if (g_drag >= 7000 && g_drag < 7000 + anymix::kBuses) {
    const int bus = g_drag - 7000;
    if (const Hot* hot = HotOf(g_drag)) {
      g_busDb[bus] = GainFromY(hot->r, y);
      g_engine->SetBusGainDb(bus, g_busDb[bus]);
    }
    return;
  }
  if (g_drag < 1000) {
    return;
  }
  const int strip = (g_drag - 1000) / 40;
  const int part = (g_drag - 1000) % 40;
  const Hot* hot = HotOf(g_drag);
  if (hot == nullptr || strip < 0 || strip >= anymix::kStrips) {
    return;
  }
  if (part == kFader) {
    g_strip[strip].gain = GainFromY(hot->r, y);
  } else if (part == kBass) {
    g_strip[strip].bass = EqFromX(hot->r, x);
  } else if (part == kTreble) {
    g_strip[strip].treble = EqFromX(hot->r, x);
  } else if (part == kPan) {
    g_strip[strip].pan = PanFromX(hot->r, x);
  } else if (part == kGateDepth) {
    const float span = static_cast<float>(std::max(1, static_cast<int>(hot->r.right - hot->r.left)));
    float t = (static_cast<float>(x) - static_cast<float>(hot->r.left)) / span;
    g_strip[strip].gateDepth = std::max(0.f, std::min(1.f, t));
  } else if (IsGateSlider(part)) {
    const int mic = MicOf(strip);
    if (mic < 0) {
      return;
    }
    const float span = static_cast<float>(std::max(1, static_cast<int>(hot->r.right - hot->r.left)));
    const float t = (static_cast<float>(x) - static_cast<float>(hot->r.left)) / span;
    ApplyMicGateLevel(mic, ClampF(t, 0.f, 1.f) * 10.f);
    PushMicGate(mic);
    return;
  } else {
    return;
  }
  PushEngineStrip(strip);
}

void SaveRoutes() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  uint16_t bits[anymix::kStrips]{};
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    uint16_t mask = 0;
    for (int bus = 0; bus < anymix::kBuses; ++bus) {
      if (g_strip[strip].route[bus]) {
        mask = static_cast<uint16_t>(mask | (1u << bus));
      }
    }
    if (g_strip[strip].insert) {
      mask = static_cast<uint16_t>(mask | 0x8000u);
    }
    if (g_strip[strip].gate) {
      mask = static_cast<uint16_t>(mask | 0x4000u);
    }
    bits[strip] = mask;
  }
  RegSetValueExW(key, L"Routes", 0, REG_BINARY, reinterpret_cast<const BYTE*>(bits), sizeof(bits));
  RegCloseKey(key);
}

void LoadRoutes() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  uint16_t bits[anymix::kStrips]{};
  DWORD size = sizeof(bits);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"Routes", nullptr, &type, reinterpret_cast<BYTE*>(bits), &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(bits)) {
    return;
  }
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    for (int bus = 0; bus < anymix::kBuses; ++bus) {
      g_strip[strip].route[bus] = (bits[strip] & static_cast<uint16_t>(1u << bus)) != 0;
    }
    g_strip[strip].insert = (bits[strip] & 0x8000u) != 0;
    g_strip[strip].gate = (bits[strip] & 0x4000u) != 0;
  }
}

void SaveGateDepth() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  uint8_t depth[anymix::kStrips]{};
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    const float t = std::max(0.f, std::min(1.f, g_strip[strip].gateDepth));
    depth[strip] = static_cast<uint8_t>(t * 100.f + 0.5f);
  }
  RegSetValueExW(key, L"GateDepth", 0, REG_BINARY, depth, sizeof(depth));
  RegCloseKey(key);
}

void LoadGateDepth() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  uint8_t depth[anymix::kStrips]{};
  DWORD size = sizeof(depth);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"GateDepth", nullptr, &type, depth, &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(depth)) {
    return;
  }
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    g_strip[strip].gateDepth = std::max(0.f, std::min(1.f, static_cast<float>(depth[strip]) / 100.f));
  }
}

void SaveCloseToTray() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  const DWORD value = g_closeToTray ? 1u : 0u;
  RegSetValueExW(key, L"CloseToTray", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

bool RunHidden(const wchar_t* command, DWORD* exitCode) {
  STARTUPINFOW info{};
  info.cb = sizeof(info);
  info.dwFlags = STARTF_USESHOWWINDOW;
  info.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::wstring editable(command);
  if (!CreateProcessW(nullptr, editable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &info,
                      &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, 8000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode != nullptr) {
    *exitCode = code;
  }
  return code == 0;
}

void XmlEscape(wchar_t* dst, int cap, const wchar_t* src) {
  int n = 0;
  if (src == nullptr || cap <= 0) {
    return;
  }
  for (int i = 0; src[i] != 0 && n + 6 < cap; ++i) {
    const wchar_t c = src[i];
    const wchar_t* rep = nullptr;
    if (c == L'&') {
      rep = L"&amp;";
    } else if (c == L'<') {
      rep = L"&lt;";
    } else if (c == L'>') {
      rep = L"&gt;";
    }
    if (rep != nullptr) {
      for (int k = 0; rep[k] != 0 && n + 1 < cap; ++k) {
        dst[n++] = rep[k];
      }
    } else {
      dst[n++] = c;
    }
  }
  dst[n] = 0;
}

bool StartupRegistrationMatches(const wchar_t* exe) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t saved[MAX_PATH]{};
  DWORD size = sizeof(saved);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"StartupExe", nullptr, &type, reinterpret_cast<BYTE*>(saved), &size);
  DWORD on = 0;
  DWORD onSize = sizeof(on);
  DWORD onType = 0;
  const LONG onStatus = RegQueryValueExW(key, L"StartWithWindows", nullptr, &onType, reinterpret_cast<BYTE*>(&on), &onSize);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_SZ || onStatus != ERROR_SUCCESS || onType != REG_DWORD) {
    return false;
  }
  const bool wanted = g_startWithWindows;
  return wanted == (on != 0) && _wcsicmp(saved, exe) == 0;
}

void RememberStartup(const wchar_t* exe) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  const DWORD on = g_startWithWindows ? 1u : 0u;
  RegSetValueExW(key, L"StartWithWindows", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&on), sizeof(on));
  if (exe != nullptr) {
    RegSetValueExW(key, L"StartupExe", 0, REG_SZ, reinterpret_cast<const BYTE*>(exe),
                   static_cast<DWORD>((wcslen(exe) + 1) * sizeof(wchar_t)));
  }
  RegCloseKey(key);
}

bool SyncWindowsStartup() {
  wchar_t exe[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    return false;
  }
  if (StartupRegistrationMatches(exe)) {
    return true;
  }
  wchar_t system[MAX_PATH]{};
  GetSystemDirectoryW(system, MAX_PATH);
  wchar_t schtasks[MAX_PATH]{};
  wsprintfW(schtasks, L"%s\\schtasks.exe", system);
  if (!g_startWithWindows) {
    wchar_t removeTask[MAX_PATH + 64]{};
    wsprintfW(removeTask, L"\"%s\" /Delete /F /TN \"AnyMix\"", schtasks);
    RunHidden(removeTask, nullptr);
    HKEY run = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &run) ==
        ERROR_SUCCESS) {
      RegDeleteValueW(run, L"AnyMix");
      RegCloseKey(run);
    }
    RememberStartup(exe);
    return true;
  }
  wchar_t user[256]{};
  wchar_t domain[256]{};
  GetEnvironmentVariableW(L"USERNAME", user, 256);
  GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
  wchar_t account[520]{};
  if (domain[0] != 0 && user[0] != 0) {
    wsprintfW(account, L"%s\\%s", domain, user);
  } else {
    wcsncpy(account, user, 255);
    account[255] = 0;
  }
  wchar_t escaped[MAX_PATH * 2]{};
  XmlEscape(escaped, static_cast<int>(sizeof(escaped) / sizeof(escaped[0])), exe);
  wchar_t xml[4096]{};
  swprintf(xml, 4096,
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
            L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId><Delay>PT0S</Delay></LogonTrigger></Triggers>\r\n"
            L"  <Principals><Principal id=\"Author\"><UserId>%s</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
            L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>false</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\r\n"
            L"  <Actions Context=\"Author\"><Exec><Command>%s</Command><Arguments>--startup</Arguments></Exec></Actions>\r\n"
            L"</Task>\r\n",
            account, account, escaped);
  wchar_t folder[MAX_PATH]{};
  GetTempPathW(MAX_PATH, folder);
  wchar_t xmlPath[MAX_PATH]{};
  wsprintfW(xmlPath, L"%sAnyMix-startup-task.xml", folder);
  HANDLE file = CreateFileW(xmlPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  const WORD bom = 0xFEFF;
  DWORD wrote = 0;
  WriteFile(file, &bom, sizeof(bom), &wrote, nullptr);
  WriteFile(file, xml, static_cast<DWORD>(wcslen(xml) * sizeof(wchar_t)), &wrote, nullptr);
  CloseHandle(file);
  wchar_t createTask[MAX_PATH * 2]{};
  wsprintfW(createTask, L"\"%s\" /Create /F /TN \"AnyMix\" /XML \"%s\"", schtasks, xmlPath);
  DWORD code = 1;
  const bool created = RunHidden(createTask, &code);
  DeleteFileW(xmlPath);
  HKEY run = nullptr;
  bool runOk = false;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &run, nullptr) == ERROR_SUCCESS) {
    wchar_t command[MAX_PATH + 32]{};
    wsprintfW(command, L"\"%s\" --startup", exe);
    runOk = RegSetValueExW(run, L"AnyMix", 0, REG_SZ, reinterpret_cast<const BYTE*>(command),
                           static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(run);
  }
  if (!created && !runOk) {
    return false;
  }
  RememberStartup(exe);
  return true;
}

void LoadStartWithWindows() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"StartWithWindows", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
  RegCloseKey(key);
  if (status == ERROR_SUCCESS && type == REG_DWORD) {
    g_startWithWindows = value != 0;
  }
}

void LoadCloseToTray() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"CloseToTray", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
  RegCloseKey(key);
  if (status == ERROR_SUCCESS && type == REG_DWORD) {
    g_closeToTray = value != 0;
  }
}

#pragma pack(push, 1)
struct MixerBlob {
  uint32_t magic;
  uint32_t version;
  float gain[anymix::kStrips];
  float bass[anymix::kStrips];
  float treble[anymix::kStrips];
  float pan[anymix::kStrips];
  float gateDepth[anymix::kStrips];
  uint8_t mute[anymix::kStrips];
  uint8_t solo[anymix::kStrips];
  uint8_t mono[anymix::kStrips];
  uint8_t insertOn[anymix::kStrips];
  uint8_t gateOn[anymix::kStrips];
  uint16_t route[anymix::kStrips];
  float busDb[anymix::kBuses];
  uint8_t busMute[anymix::kBuses];
  uint8_t busMono[anymix::kBuses];
  uint8_t busSolo[anymix::kPhysicalBuses];
  uint8_t dark;
  uint8_t compact;
  uint8_t anim;
  uint8_t closeToTray;
  int32_t recBus;
  int32_t recFormat;
  int32_t page;
  wchar_t label[anymix::kStrips][32];
  wchar_t folder[MAX_PATH];
  wchar_t micId[anymix::kMicStrips][512];
  wchar_t outId[anymix::kPhysicalBuses][512];
};

struct AppBlobRow {
  wchar_t name[64];
  uint8_t strip;
  uint8_t muted;
  float volume;
};
#pragma pack(pop)

constexpr uint32_t kStateMagic = 0x32584D41u;
constexpr uint32_t kAppMagic = 0x31565041u;
constexpr int kSavedAppMax = 48;

void RememberApps() {
  for (int strip = 0; strip < anymix::kVirtualStrips; ++strip) {
    for (const anymix::SessionInfo& session : g_sessions[static_cast<size_t>(strip)]) {
      if (session.name[0] == 0) {
        continue;
      }
      SavedApp* found = nullptr;
      for (SavedApp& saved : g_savedApps) {
        if (saved.strip == strip && _wcsicmp(saved.name, session.name) == 0) {
          found = &saved;
          break;
        }
      }
      if (found == nullptr) {
        if (static_cast<int>(g_savedApps.size()) >= kSavedAppMax) {
          continue;
        }
        g_savedApps.push_back({});
        found = &g_savedApps.back();
        wcsncpy(found->name, session.name, 63);
        found->name[63] = 0;
        found->strip = strip;
      }
      found->volume = session.volume < 0.f ? 0.f : (session.volume > 1.f ? 1.f : session.volume);
      found->muted = session.muted;
    }
  }
}

void NoteMainWindow() {
  if (g_mainWindow == nullptr || !IsWindowVisible(g_mainWindow) || IsIconic(g_mainWindow)) {
    return;
  }
  WINDOWPLACEMENT place{};
  place.length = sizeof(place);
  if (!GetWindowPlacement(g_mainWindow, &place)) {
    return;
  }
  if (place.showCmd != SW_SHOWMAXIMIZED) {
    place.showCmd = SW_SHOWNORMAL;
  }
  g_place = place;
  g_havePlace = true;
}

void NoteMiniWindow() {
  if (g_miniMode && g_miniWindow != nullptr && IsWindowVisible(g_miniWindow)) {
    RECT rect{};
    GetWindowRect(g_miniWindow, &rect);
    g_openMini = true;
    g_miniX = rect.left;
    g_miniY = rect.top;
    return;
  }
  if (g_mainWindow != nullptr && IsWindowVisible(g_mainWindow)) {
    g_openMini = false;
  }
}

#pragma pack(push, 1)
struct PlaceBlob {
  uint32_t magic;
  uint32_t version;
  int32_t showCmd;
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
  uint8_t mini;
  int32_t miniX;
  int32_t miniY;
};
#pragma pack(pop)

constexpr uint32_t kPlaceMagic = 0x43414C50u;

void SaveState(bool flush = false) {
  NoteMainWindow();
  NoteMiniWindow();
  RememberApps();
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  MixerBlob blob{};
  blob.magic = kStateMagic;
  blob.version = 1;
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    blob.gain[strip] = g_strip[strip].gain;
    blob.bass[strip] = g_strip[strip].bass;
    blob.treble[strip] = g_strip[strip].treble;
    blob.pan[strip] = g_strip[strip].pan;
    blob.gateDepth[strip] = g_strip[strip].gateDepth;
    blob.mute[strip] = g_strip[strip].mute ? 1 : 0;
    blob.solo[strip] = g_strip[strip].solo ? 1 : 0;
    blob.mono[strip] = g_strip[strip].mono ? 1 : 0;
    blob.insertOn[strip] = g_strip[strip].insert ? 1 : 0;
    blob.gateOn[strip] = g_strip[strip].gate ? 1 : 0;
    uint16_t mask = 0;
    for (int bus = 0; bus < anymix::kBuses; ++bus) {
      if (g_strip[strip].route[bus]) {
        mask = static_cast<uint16_t>(mask | (1u << bus));
      }
    }
    blob.route[strip] = mask;
    wcsncpy(blob.label[strip], g_label[strip], 31);
    blob.label[strip][31] = 0;
  }
  for (int bus = 0; bus < anymix::kBuses; ++bus) {
    blob.busDb[bus] = g_busDb[bus];
    blob.busMute[bus] = g_busMute[bus] ? 1 : 0;
    blob.busMono[bus] = g_busMono[bus] ? 1 : 0;
  }
  for (int bus = 0; bus < anymix::kPhysicalBuses; ++bus) {
    blob.busSolo[bus] = g_busSolo[bus] ? 1 : 0;
    const int sel = g_outSel[bus];
    if (sel > 0 && sel < static_cast<int>(g_renders.size()) &&
        g_renders[static_cast<size_t>(sel)].id[0] != 0) {
      wcsncpy(blob.outId[bus], g_renders[static_cast<size_t>(sel)].id, 511);
      blob.outId[bus][511] = 0;
      CopyText(g_savedOut[bus], 512, blob.outId[bus]);
    } else if (sel < 0 && g_savedOut[bus][0] != 0) {
      wcsncpy(blob.outId[bus], g_savedOut[bus], 511);
      blob.outId[bus][511] = 0;
    } else {
      g_savedOut[bus][0] = 0;
    }
  }
  for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
    const int sel = g_micSel[mic];
    if (sel > 0 && sel < static_cast<int>(g_captures.size()) &&
        g_captures[static_cast<size_t>(sel)].id[0] != 0) {
      wcsncpy(blob.micId[mic], g_captures[static_cast<size_t>(sel)].id, 511);
      blob.micId[mic][511] = 0;
      CopyText(g_savedMic[mic], 512, blob.micId[mic]);
    } else if (!g_micUser[mic] && g_savedMic[mic][0] != 0) {
      wcsncpy(blob.micId[mic], g_savedMic[mic], 511);
      blob.micId[mic][511] = 0;
    } else {
      g_savedMic[mic][0] = 0;
    }
  }
  blob.dark = g_dark ? 1 : 0;
  blob.compact = g_compact ? 1 : 0;
  blob.anim = g_anim ? 1 : 0;
  blob.closeToTray = g_closeToTray ? 1 : 0;
  blob.recBus = g_recBus;
  blob.recFormat = g_recFormat;
  blob.page = static_cast<int32_t>(g_page);
  wcsncpy(blob.folder, g_folder, MAX_PATH - 1);
  RegSetValueExW(key, L"State", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&blob), sizeof(blob));

  AppBlobRow rows[kSavedAppMax]{};
  const int count = std::min(kSavedAppMax, static_cast<int>(g_savedApps.size()));
  for (int i = 0; i < count; ++i) {
    wcsncpy(rows[i].name, g_savedApps[static_cast<size_t>(i)].name, 63);
    rows[i].strip = static_cast<uint8_t>(g_savedApps[static_cast<size_t>(i)].strip);
    rows[i].muted = g_savedApps[static_cast<size_t>(i)].muted ? 1 : 0;
    rows[i].volume = g_savedApps[static_cast<size_t>(i)].volume;
  }
  std::vector<BYTE> apps(sizeof(uint32_t) * 2 + sizeof(AppBlobRow) * static_cast<size_t>(count));
  uint32_t header[2] = {kAppMagic, static_cast<uint32_t>(count)};
  std::memcpy(apps.data(), header, sizeof(header));
  if (count > 0) {
    std::memcpy(apps.data() + sizeof(header), rows, sizeof(AppBlobRow) * static_cast<size_t>(count));
  }
  RegSetValueExW(key, L"AppVolumes", 0, REG_BINARY, apps.data(), static_cast<DWORD>(apps.size()));
  if (g_havePlace) {
    PlaceBlob place{};
    place.magic = kPlaceMagic;
    place.version = 1;
    place.showCmd = g_place.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    place.left = g_place.rcNormalPosition.left;
    place.top = g_place.rcNormalPosition.top;
    place.right = g_place.rcNormalPosition.right;
    place.bottom = g_place.rcNormalPosition.bottom;
    place.mini = g_openMini ? 1 : 0;
    place.miniX = g_miniX;
    place.miniY = g_miniY;
    RegSetValueExW(key, L"Window", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&place), sizeof(place));
  }
  const DWORD recOut = 0x80000000u | g_recMask;
  const DWORD playOut = 0x80000000u | g_playMask;
  RegSetValueExW(key, L"RecordOut", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&recOut), sizeof(recOut));
  RegSetValueExW(key, L"PlayOut", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&playOut), sizeof(playOut));
  WriteNoiseGate(key);
  if (flush) {
    RegFlushKey(key);
  }
  RegCloseKey(key);
}

void Autosave() {
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  if (last != 0 && now - last < 60000) {
    return;
  }
  last = now;
  SaveState();
}

void LoadState() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  MixerBlob blob{};
  DWORD size = sizeof(blob);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"State", nullptr, &type, reinterpret_cast<BYTE*>(&blob), &size);
  if (status == ERROR_SUCCESS && type == REG_BINARY && size == sizeof(blob) && blob.magic == kStateMagic &&
      blob.version == 1) {
    for (int strip = 0; strip < anymix::kStrips; ++strip) {
      g_strip[strip].gain = std::max(anymix::kFaderMinDb, std::min(anymix::kFaderMaxDb, blob.gain[strip]));
      g_strip[strip].bass = std::max(-12.f, std::min(12.f, blob.bass[strip]));
      g_strip[strip].treble = std::max(-12.f, std::min(12.f, blob.treble[strip]));
      g_strip[strip].pan = std::max(-1.f, std::min(1.f, blob.pan[strip]));
      g_strip[strip].gateDepth = std::max(0.f, std::min(1.f, blob.gateDepth[strip]));
      g_strip[strip].mute = blob.mute[strip] != 0;
      g_strip[strip].solo = blob.solo[strip] != 0;
      g_strip[strip].mono = blob.mono[strip] != 0;
      g_strip[strip].insert = blob.insertOn[strip] != 0;
      g_strip[strip].gate = blob.gateOn[strip] != 0;
      for (int bus = 0; bus < anymix::kBuses; ++bus) {
        g_strip[strip].route[bus] = (blob.route[strip] & static_cast<uint16_t>(1u << bus)) != 0;
      }
      if (blob.label[strip][0] != 0) {
        wcsncpy(g_label[strip], blob.label[strip], 31);
        g_label[strip][31] = 0;
      }
    }
    for (int bus = 0; bus < anymix::kBuses; ++bus) {
      g_busDb[bus] = std::max(anymix::kFaderMinDb, std::min(anymix::kFaderMaxDb, blob.busDb[bus]));
      g_busMute[bus] = blob.busMute[bus] != 0;
      g_busMono[bus] = blob.busMono[bus] != 0;
    }
    for (int bus = 0; bus < anymix::kPhysicalBuses; ++bus) {
      g_busSolo[bus] = blob.busSolo[bus] != 0;
      wcsncpy(g_savedOut[bus], blob.outId[bus], 511);
      g_savedOut[bus][511] = 0;
    }
    for (int mic = 0; mic < anymix::kMicStrips; ++mic) {
      wcsncpy(g_savedMic[mic], blob.micId[mic], 511);
      g_savedMic[mic][511] = 0;
    }
    g_haveDeviceIds = true;
    g_dark = blob.dark != 0;
    g_compact = blob.compact != 0;
    g_anim = blob.anim != 0;
    g_closeToTray = blob.closeToTray != 0;
    g_recBus = std::max(0, std::min(anymix::kBuses - 1, static_cast<int>(blob.recBus)));
    g_recFormat = std::max(0, std::min(kRecFormatCount - 1, static_cast<int>(blob.recFormat)));
    if (blob.page >= 0 && blob.page < kPageCount) {
      g_page = static_cast<Page>(blob.page);
    }
    if (blob.folder[0] != 0) {
      wcsncpy(g_folder, blob.folder, MAX_PATH - 1);
      g_folder[MAX_PATH - 1] = 0;
    }
  }
  DWORD appSize = 0;
  if (RegQueryValueExW(key, L"AppVolumes", nullptr, &type, nullptr, &appSize) == ERROR_SUCCESS &&
      type == REG_BINARY && appSize >= sizeof(uint32_t) * 2 && appSize < 1024 * 1024) {
    std::vector<BYTE> apps(appSize);
    if (RegQueryValueExW(key, L"AppVolumes", nullptr, &type, apps.data(), &appSize) == ERROR_SUCCESS) {
      uint32_t header[2]{};
      std::memcpy(header, apps.data(), sizeof(header));
      const uint32_t count = header[1];
      if (header[0] == kAppMagic && count <= static_cast<uint32_t>(kSavedAppMax) &&
          appSize >= sizeof(header) + sizeof(AppBlobRow) * count) {
        g_savedApps.clear();
        const auto* rows = reinterpret_cast<const AppBlobRow*>(apps.data() + sizeof(header));
        for (uint32_t i = 0; i < count; ++i) {
          if (rows[i].name[0] == 0 || rows[i].strip >= anymix::kVirtualStrips) {
            continue;
          }
          SavedApp saved{};
          wcsncpy(saved.name, rows[i].name, 63);
          saved.strip = rows[i].strip;
          saved.muted = rows[i].muted != 0;
          saved.volume = std::max(0.f, std::min(1.f, rows[i].volume));
          g_savedApps.push_back(saved);
        }
      }
    }
  }
  PlaceBlob place{};
  DWORD placeSize = sizeof(place);
  if (RegQueryValueExW(key, L"Window", nullptr, &type, reinterpret_cast<BYTE*>(&place), &placeSize) == ERROR_SUCCESS &&
      type == REG_BINARY && placeSize == sizeof(place) && place.magic == kPlaceMagic && place.version == 1 &&
      place.right > place.left && place.bottom > place.top) {
    g_place.length = sizeof(g_place);
    g_place.showCmd = place.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    g_place.rcNormalPosition.left = place.left;
    g_place.rcNormalPosition.top = place.top;
    g_place.rcNormalPosition.right = place.right;
    g_place.rcNormalPosition.bottom = place.bottom;
    g_openMini = place.mini != 0;
    g_miniX = place.miniX;
    g_miniY = place.miniY;
    g_havePlace = true;
  }
  DWORD routeValue = 0;
  DWORD routeSize = sizeof(routeValue);
  if (RegQueryValueExW(key, L"RecordOut", nullptr, &type, reinterpret_cast<BYTE*>(&routeValue), &routeSize) ==
          ERROR_SUCCESS &&
      type == REG_DWORD) {
    if ((routeValue & 0x80000000u) != 0) {
      g_recMask = routeValue & ((1u << anymix::kBuses) - 1);
    } else if (routeValue >= 1 && routeValue <= static_cast<DWORD>(anymix::kBuses)) {
      g_recMask = 1u << (routeValue - 1);
    } else {
      g_recMask = 0;
    }
  }
  routeValue = 0;
  routeSize = sizeof(routeValue);
  if (RegQueryValueExW(key, L"PlayOut", nullptr, &type, reinterpret_cast<BYTE*>(&routeValue), &routeSize) ==
          ERROR_SUCCESS &&
      type == REG_DWORD) {
    if ((routeValue & 0x80000000u) != 0) {
      g_playMask = routeValue & ((1u << anymix::kBuses) - 1);
    } else if (routeValue >= 1 && routeValue <= static_cast<DWORD>(anymix::kBuses)) {
      g_playMask = 1u << (routeValue - 1);
    } else {
      g_playMask = 0;
    }
  }
  RegCloseKey(key);
}

void PushEngineBuses() {
  for (int bus = 0; bus < anymix::kBuses; ++bus) {
    g_engine->SetBusGainDb(bus, g_busDb[bus]);
    g_engine->SetBusMute(bus, g_busMute[bus]);
    g_engine->SetBusMono(bus, g_busMono[bus]);
  }
  for (int bus = 0; bus < anymix::kPhysicalBuses; ++bus) {
    g_engine->SetBusSolo(bus, g_busSolo[bus]);
  }
}

void RemoveTray(HWND window) {
  if (!g_trayAdded) {
    return;
  }
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = window;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g_trayAdded = false;
}

void AddTray(HWND window) {
  if (g_trayAdded) {
    return;
  }
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = window;
  nid.uID = 1;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = WM_APP + 1;
  nid.hIcon = g_iconSmall != nullptr ? g_iconSmall : LoadIconW(nullptr, IDI_APPLICATION);
  wcsncpy(nid.szTip, L"AnyMix", sizeof(nid.szTip) / sizeof(wchar_t) - 1);
  if (Shell_NotifyIconW(NIM_ADD, &nid)) {
    g_trayAdded = true;
  }
}

void ShowFromTray(HWND window) {
  RemoveTray(window);
  ShowWindow(window, SW_SHOW);
  ShowWindow(window, SW_RESTORE);
  SetForegroundWindow(window);
}

void ToggleMicMute() {
  const int strip = anymix::kMic1;
  g_strip[strip].mute = !g_strip[strip].mute;
  if (g_engine != nullptr) {
    g_engine->SetMute(strip, g_strip[strip].mute);
  }
}

COLORREF RgbOf(Gdiplus::Color color) {
  return RGB(color.GetR(), color.GetG(), color.GetB());
}

void DrawTrayItem(const DRAWITEMSTRUCT* item) {
  const Palette pal = Colors();
  const bool hot = (item->itemState & ODS_SELECTED) != 0;
  const bool muteRow = item->itemID == 3;
  const bool muted = g_strip[anymix::kMic1].mute;
  const COLORREF fill = hot ? RgbOf(pal.accent) : (muteRow && muted ? RgbOf(pal.strip) : RgbOf(pal.bg));
  HBRUSH brush = CreateSolidBrush(fill);
  FillRect(item->hDC, &item->rcItem, brush);
  DeleteObject(brush);
  if (hot) {
    HBRUSH edge = CreateSolidBrush(RGB(255, 255, 255));
    RECT bar{item->rcItem.left, item->rcItem.top + 6, item->rcItem.left + 3, item->rcItem.bottom - 6};
    FillRect(item->hDC, &bar, edge);
    DeleteObject(edge);
  }
  const wchar_t* label = L"Quit";
  if (item->itemID == 1) {
    label = L"Open";
  } else if (item->itemID == 3) {
    label = L"Mute mic";
  }
  SetBkMode(item->hDC, TRANSPARENT);
  SetTextColor(item->hDC, hot ? RGB(255, 255, 255) : RgbOf(muted && muteRow ? pal.mute : pal.text));
  HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HGDIOBJ old = SelectObject(item->hDC, font);
  RECT text = item->rcItem;
  text.left += 16;
  text.right -= 28;
  DrawTextW(item->hDC, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
  if (muteRow && muted) {
    HBRUSH dot = CreateSolidBrush(hot ? RGB(255, 255, 255) : RgbOf(pal.mute));
    HGDIOBJ oldBrush = SelectObject(item->hDC, dot);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ oldPen = SelectObject(item->hDC, pen);
    const int cy = (item->rcItem.top + item->rcItem.bottom) / 2;
    Ellipse(item->hDC, item->rcItem.right - 20, cy - 4, item->rcItem.right - 12, cy + 4);
    SelectObject(item->hDC, oldPen);
    SelectObject(item->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(dot);
  }
  SelectObject(item->hDC, old);
  DeleteObject(font);
}

void ToggleRoute(int strip, int bus) {
  g_strip[strip].route[bus] = !g_strip[strip].route[bus];
  g_engine->SetRoute(strip, bus, g_strip[strip].route[bus]);
  SaveRoutes();
}

void EnsurePlayLock() {
  if (!g_playLockOn) {
    InitializeCriticalSection(&g_playLock);
    g_playLockOn = true;
  }
}

void StopPlayback() {
  EnsurePlayLock();
  g_playStop.store(true);
  mciSendStringW(L"stop anymixplay", nullptr, 0, nullptr);
  EnterCriticalSection(&g_playLock);
  const HWAVEOUT wave = g_wave;
  LeaveCriticalSection(&g_playLock);
  if (wave != nullptr) {
    waveOutReset(wave);
  }
  if (g_engine != nullptr) {
    g_engine->ClearPlayback();
  }
  if (g_playThread != nullptr) {
    WaitForSingleObject(g_playThread, 5000);
    CloseHandle(g_playThread);
    g_playThread = nullptr;
  }
  if (g_engine != nullptr) {
    g_engine->ClearPlayback();
  }
  mciSendStringW(L"close anymixplay", nullptr, 0, nullptr);
  g_playing = false;
}

void PollPlayback() {
  if (g_playThread != nullptr && WaitForSingleObject(g_playThread, 0) == WAIT_OBJECT_0) {
    CloseHandle(g_playThread);
    g_playThread = nullptr;
    g_playing = false;
  }
}

struct PcmStream {
  static constexpr int kSlots = 4;
  static constexpr DWORD kBytes = 16 * 1024;

  struct Slot {
    WAVEHDR header{};
    std::vector<BYTE> data;
    bool queued = false;
  };

  HWAVEOUT wave = nullptr;
  HANDLE done = nullptr;
  Slot slots[kSlots]{};

  bool Open(const WAVEFORMATEX* format) {
    done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (done == nullptr) {
      return false;
    }
    if (waveOutOpen(&wave, WAVE_MAPPER, format, reinterpret_cast<DWORD_PTR>(done), 0, CALLBACK_EVENT) !=
            MMSYSERR_NOERROR ||
        wave == nullptr) {
      CloseHandle(done);
      done = nullptr;
      wave = nullptr;
      return false;
    }
    EnsurePlayLock();
    EnterCriticalSection(&g_playLock);
    g_wave = wave;
    LeaveCriticalSection(&g_playLock);
    return true;
  }

  void Reclaim() {
    for (Slot& slot : slots) {
      if (!slot.queued || (slot.header.dwFlags & WHDR_DONE) == 0) {
        continue;
      }
      waveOutUnprepareHeader(wave, &slot.header, sizeof(slot.header));
      slot.header = {};
      slot.queued = false;
    }
  }

  bool Busy() const {
    for (const Slot& slot : slots) {
      if (slot.queued) {
        return true;
      }
    }
    return false;
  }

  bool Write(const BYTE* data, DWORD size, WORD align) {
    if (align < 1) {
      align = 1;
    }
    DWORD offset = 0;
    while (offset < size && !g_playStop.load()) {
      Reclaim();
      Slot* slot = nullptr;
      for (Slot& item : slots) {
        if (!item.queued) {
          slot = &item;
          break;
        }
      }
      if (slot == nullptr) {
        WaitForSingleObject(done, 30);
        continue;
      }
      DWORD count = std::min(kBytes, size - offset);
      if (count > align) {
        count -= count % align;
      }
      slot->data.assign(data + offset, data + offset + count);
      slot->header = {};
      slot->header.lpData = reinterpret_cast<LPSTR>(slot->data.data());
      slot->header.dwBufferLength = count;
      if (waveOutPrepareHeader(wave, &slot->header, sizeof(slot->header)) != MMSYSERR_NOERROR) {
        return false;
      }
      if (waveOutWrite(wave, &slot->header, sizeof(slot->header)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(wave, &slot->header, sizeof(slot->header));
        return false;
      }
      slot->queued = true;
      offset += count;
    }
    return !g_playStop.load();
  }

  void Drain() {
    while (Busy() && !g_playStop.load()) {
      WaitForSingleObject(done, 30);
      Reclaim();
    }
  }

  void Close() {
    EnsurePlayLock();
    EnterCriticalSection(&g_playLock);
    if (g_wave == wave) {
      g_wave = nullptr;
    }
    LeaveCriticalSection(&g_playLock);
    if (wave != nullptr) {
      waveOutReset(wave);
      Reclaim();
      waveOutClose(wave);
      wave = nullptr;
    }
    if (done != nullptr) {
      CloseHandle(done);
      done = nullptr;
    }
  }
};

struct PlayCursor {
  double pos = 0;
};

bool FeedPlayback(const BYTE* data, DWORD bytes, int channels, int rate, int bits, PlayCursor& cursor) {
  if (g_engine == nullptr || g_playMask == 0 || data == nullptr || bits != 16 || channels < 1 || channels > 8 ||
      rate < 8000 || rate > 192000) {
    return false;
  }
  const int frameBytes = channels * static_cast<int>(sizeof(int16_t));
  if (bytes < static_cast<DWORD>(frameBytes)) {
    return !g_playStop.load();
  }
  const int inFrames = static_cast<int>(bytes / static_cast<DWORD>(frameBytes));
  const auto* pcm = reinterpret_cast<const int16_t*>(data);
  const double step = static_cast<double>(rate) / static_cast<double>(anymix::kSampleRate);
  std::vector<float> out;
  out.reserve(static_cast<size_t>(static_cast<double>(inFrames) / step + 8) * 2);
  auto sample = [&](int frame, int channel) {
    const int ch = channel < channels ? channel : 0;
    const int index = frame < 0 ? 0 : (frame >= inFrames ? inFrames - 1 : frame);
    return static_cast<float>(pcm[index * channels + ch]) / 32768.f;
  };
  double pos = cursor.pos;
  while (pos < static_cast<double>(inFrames) && !g_playStop.load()) {
    const int index = static_cast<int>(pos);
    const float frac = static_cast<float>(pos - static_cast<double>(index));
    const float left = sample(index, 0) * (1.f - frac) + sample(index + 1, 0) * frac;
    const float right = channels > 1 ? sample(index, 1) * (1.f - frac) + sample(index + 1, 1) * frac : left;
    out.push_back(left);
    out.push_back(right);
    pos += step;
  }
  cursor.pos = pos - static_cast<double>(inFrames);
  if (cursor.pos < 0) {
    cursor.pos = 0;
  }
  if (!out.empty() && !g_playStop.load()) {
    g_engine->PushPlayback(out.data(), static_cast<int>(out.size() / 2));
  }
  return !g_playStop.load();
}

bool PlayReaderToBus(IMFSourceReader* reader) {
  IMFMediaType* current = nullptr;
  if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &current)) ||
      current == nullptr) {
    return false;
  }
  WAVEFORMATEX* format = nullptr;
  UINT32 formatSize = 0;
  const HRESULT made = MFCreateWaveFormatExFromMFMediaType(current, &format, &formatSize);
  current->Release();
  if (FAILED(made) || format == nullptr || format->wBitsPerSample != 16) {
    if (format != nullptr) {
      CoTaskMemFree(format);
    }
    return false;
  }
  const int channels = format->nChannels;
  const int rate = static_cast<int>(format->nSamplesPerSec);
  const int bits = format->wBitsPerSample;
  CoTaskMemFree(format);
  PlayCursor cursor;
  bool heard = false;
  while (!g_playStop.load()) {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    const HRESULT read = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                            nullptr, &sample);
    if (FAILED(read) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      if (sample != nullptr) {
        sample->Release();
      }
      break;
    }
    if (sample == nullptr) {
      continue;
    }
    IMFMediaBuffer* buffer = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr) {
      BYTE* data = nullptr;
      DWORD length = 0;
      if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && data != nullptr && length > 0) {
        const bool wrote = FeedPlayback(data, length, channels, rate, bits, cursor);
        buffer->Unlock();
        if (!wrote) {
          buffer->Release();
          sample->Release();
          break;
        }
        heard = true;
      }
      buffer->Release();
    }
    sample->Release();
  }
  return heard;
}

bool PlayWithFoundation(const wchar_t* path) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
    return false;
  }
  const bool ownCom = SUCCEEDED(com);
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
    if (ownCom) {
      CoUninitialize();
    }
    return false;
  }
  IMFSourceReader* reader = nullptr;
  if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader)) || reader == nullptr) {
    MFShutdown();
    if (ownCom) {
      CoUninitialize();
    }
    return false;
  }
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
  auto wantPcm = [&](bool force) {
    IMFMediaType* type = nullptr;
    if (FAILED(MFCreateMediaType(&type)) || type == nullptr) {
      return false;
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (force) {
      type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
      type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    }
    const HRESULT set = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type);
    type->Release();
    return SUCCEEDED(set);
  };
  if (!wantPcm(false) && !wantPcm(true)) {
    reader->Release();
    MFShutdown();
    if (ownCom) {
      CoUninitialize();
    }
    return false;
  }
  if (g_playMask != 0) {
    const bool heard = PlayReaderToBus(reader);
    reader->Release();
    MFShutdown();
    if (ownCom) {
      CoUninitialize();
    }
    return heard;
  }
  PcmStream stream;
  WORD align = 4;
  auto openStream = [&]() {
    IMFMediaType* current = nullptr;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &current)) ||
        current == nullptr) {
      return false;
    }
    WAVEFORMATEX* format = nullptr;
    UINT32 formatSize = 0;
    const HRESULT made = MFCreateWaveFormatExFromMFMediaType(current, &format, &formatSize);
    current->Release();
    if (FAILED(made) || format == nullptr) {
      return false;
    }
    const WAVEFORMATEX copy = *format;
    CoTaskMemFree(format);
    align = copy.nBlockAlign == 0 ? 4 : copy.nBlockAlign;
    return stream.Open(&copy);
  };
  if (!openStream() && wantPcm(true)) {
    openStream();
  }
  if (stream.wave == nullptr) {
    reader->Release();
    MFShutdown();
    if (ownCom) {
      CoUninitialize();
    }
    return false;
  }
  bool heard = false;
  while (!g_playStop.load()) {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    const HRESULT read = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                            nullptr, &sample);
    if (FAILED(read) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      if (sample != nullptr) {
        sample->Release();
      }
      break;
    }
    if (sample == nullptr) {
      continue;
    }
    IMFMediaBuffer* buffer = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr) {
      BYTE* data = nullptr;
      DWORD length = 0;
      if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && data != nullptr && length > 0) {
        const bool wrote = stream.Write(data, length, align);
        buffer->Unlock();
        if (!wrote) {
          buffer->Release();
          sample->Release();
          break;
        }
        heard = true;
      }
      buffer->Release();
    }
    sample->Release();
  }
  stream.Drain();
  stream.Close();
  reader->Release();
  MFShutdown();
  if (ownCom) {
    CoUninitialize();
  }
  return heard;
}

bool PlayPcmWav(const wchar_t* path) {
  const wchar_t* ext = wcsrchr(path, L'.');
  if (ext == nullptr || _wcsicmp(ext, L".wav") != 0) {
    return false;
  }
  const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  uint8_t header[44]{};
  DWORD got = 0;
  if (!ReadFile(file, header, 44, &got, nullptr) || got != 44 || std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "WAVE", 4) != 0 || std::memcmp(header + 36, "data", 4) != 0) {
    CloseHandle(file);
    return false;
  }
  uint16_t format = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t rate = 0;
  uint32_t dataBytes = 0;
  std::memcpy(&format, header + 20, 2);
  std::memcpy(&channels, header + 22, 2);
  std::memcpy(&rate, header + 24, 4);
  std::memcpy(&bits, header + 34, 2);
  std::memcpy(&dataBytes, header + 40, 4);
  if (format != 1 || channels == 0 || bits == 0 || rate == 0 || (bits % 8) != 0) {
    CloseHandle(file);
    return false;
  }
  WAVEFORMATEX waveFormat{};
  waveFormat.wFormatTag = WAVE_FORMAT_PCM;
  waveFormat.nChannels = channels;
  waveFormat.nSamplesPerSec = rate;
  waveFormat.wBitsPerSample = bits;
  waveFormat.nBlockAlign = static_cast<WORD>(channels * bits / 8);
  waveFormat.nAvgBytesPerSec = rate * waveFormat.nBlockAlign;
  if (g_playMask != 0) {
    if (bits != 16) {
      CloseHandle(file);
      return false;
    }
    PlayCursor cursor;
    std::vector<BYTE> routed(64 * 1024);
    bool heard = false;
    DWORD left = dataBytes;
    const DWORD frameBytes = static_cast<DWORD>(channels) * 2;
    while (left >= frameBytes && !g_playStop.load()) {
      DWORD want = std::min<DWORD>(left, static_cast<DWORD>(routed.size()));
      want -= want % frameBytes;
      DWORD read = 0;
      if (!ReadFile(file, routed.data(), want, &read, nullptr) || read < frameBytes) {
        break;
      }
      read -= read % frameBytes;
      if (!FeedPlayback(routed.data(), read, channels, static_cast<int>(rate), bits, cursor)) {
        break;
      }
      heard = true;
      left -= read;
    }
    CloseHandle(file);
    return heard;
  }
  PcmStream stream;
  if (!stream.Open(&waveFormat)) {
    CloseHandle(file);
    return false;
  }
  std::vector<BYTE> chunk(64 * 1024);
  bool heard = false;
  DWORD left = dataBytes;
  while (left > 0 && !g_playStop.load()) {
    const DWORD want = std::min<DWORD>(left, static_cast<DWORD>(chunk.size()));
    DWORD read = 0;
    if (!ReadFile(file, chunk.data(), want, &read, nullptr) || read == 0) {
      break;
    }
    if (!stream.Write(chunk.data(), read, waveFormat.nBlockAlign)) {
      break;
    }
    heard = true;
    left -= read;
  }
  stream.Drain();
  stream.Close();
  CloseHandle(file);
  return heard;
}

bool PlayWithMci(const wchar_t* path) {
  std::wstring escaped;
  escaped.reserve(wcslen(path) * 2);
  for (const wchar_t* letter = path; *letter != 0; ++letter) {
    if (*letter == L'\\') {
      escaped.push_back(L'\\');
    }
    escaped.push_back(*letter);
  }
  wchar_t cmd[MAX_PATH * 4];
  swprintf(cmd, MAX_PATH * 4, L"open \"%s\" alias anymixplay", escaped.c_str());
  if (mciSendStringW(cmd, nullptr, 0, nullptr) != 0) {
    const wchar_t* ext = wcsrchr(path, L'.');
    const wchar_t* type = (ext != nullptr && _wcsicmp(ext, L".mp3") == 0) ? L"mpegvideo" : L"waveaudio";
    swprintf(cmd, MAX_PATH * 4, L"open \"%s\" type %s alias anymixplay", escaped.c_str(), type);
    if (mciSendStringW(cmd, nullptr, 0, nullptr) != 0) {
      return false;
    }
  }
  const MCIERROR played = mciSendStringW(L"play anymixplay wait", nullptr, 0, nullptr);
  mciSendStringW(L"close anymixplay", nullptr, 0, nullptr);
  return played == 0 && !g_playStop.load();
}

DWORD WINAPI PlayProc(LPVOID param) {
  auto* path = static_cast<wchar_t*>(param);
  const bool played = PlayPcmWav(path) || (!g_playStop.load() && PlayWithFoundation(path)) ||
                      (!g_playStop.load() && PlayWithMci(path));
  if (!played && !g_playStop.load() && g_mainWindow != nullptr) {
    MessageBoxW(g_mainWindow, L"Windows cannot play this file.", L"AnyMix", MB_OK | MB_ICONWARNING);
  }
  delete[] path;
  g_playing = false;
  return 0;
}

bool StartPlayback(HWND window, const wchar_t* path) {
  if (path == nullptr || path[0] == 0 || wcschr(path, L'"') != nullptr) {
    MessageBoxW(window, L"Cannot play this file.", L"AnyMix", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(window, L"File not found.", L"AnyMix", MB_OK | MB_ICONWARNING);
    return false;
  }
  StopPlayback();
  EnsurePlayLock();
  g_playStop.store(false);
  auto* copy = new wchar_t[MAX_PATH];
  wcsncpy(copy, path, MAX_PATH - 1);
  copy[MAX_PATH - 1] = 0;
  wcsncpy(g_playPath, copy, MAX_PATH - 1);
  g_playPath[MAX_PATH - 1] = 0;
  g_playing = true;
  g_playThread = CreateThread(nullptr, 0, PlayProc, copy, 0, nullptr);
  if (g_playThread == nullptr) {
    delete[] copy;
    g_playing = false;
    MessageBoxW(window, L"Cannot play this file.", L"AnyMix", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

void RememberNewestTake() {
  if (g_folder[0] == 0) {
    return;
  }
  wchar_t pattern[MAX_PATH];
  swprintf(pattern, MAX_PATH, L"%s\\*.wav", g_folder);
  WIN32_FIND_DATAW found{};
  const HANDLE handle = FindFirstFileW(pattern, &found);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  FILETIME best{};
  wchar_t bestName[MAX_PATH]{};
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    if (bestName[0] == 0 || CompareFileTime(&found.ftLastWriteTime, &best) > 0) {
      best = found.ftLastWriteTime;
      swprintf(bestName, MAX_PATH, L"%s\\%s", g_folder, found.cFileName);
    }
  } while (FindNextFileW(handle, &found));
  FindClose(handle);
  if (bestName[0] != 0) {
    wcsncpy(g_lastTake, bestName, MAX_PATH - 1);
    g_lastTake[MAX_PATH - 1] = 0;
  }
}

void PlayLastTake(HWND window) {
  if (g_engine->Recording()) {
    MessageBoxW(window, L"Stop the recording before playing it back.", L"AnyMix", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (g_lastTake[0] == 0) {
    RememberNewestTake();
  }
  if (g_lastTake[0] == 0) {
    MessageBoxW(window, L"No recording in this folder yet.", L"AnyMix", MB_OK | MB_ICONINFORMATION);
    return;
  }
  StartPlayback(window, g_lastTake);
}

void OpenAudioFile(HWND window) {
  wchar_t path[MAX_PATH]{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter =
      L"Audio\0*.wav;*.mp3;*.flac;*.aac;*.m4a;*.mp4;*.wma;*.asf;*.ogg;*.oga;*.opus;*.aiff;*.aif;*.aifc;*.ac3;*.mka;*.webm;*.caf;*.amr;*.mid;*.midi\0All files\0*.*\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  dialog.lpstrTitle = L"Play an audio file";
  if (!GetOpenFileNameW(&dialog)) {
    return;
  }
  StartPlayback(window, path);
}

GUID RecordSubtype(int kind) {
  if (kind == kRecMp3) {
    return MFAudioFormat_MP3;
  }
  if (kind == kRecFlac) {
    return {0x0000F1AC, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
  }
  if (kind == kRecAac) {
    return MFAudioFormat_AAC;
  }
  if (kind == kRecWma) {
    return MFAudioFormat_WMAudioV8;
  }
  return GUID_NULL;
}

IMFMediaType* EncoderOutputType(int kind) {
  const GUID subtype = RecordSubtype(kind);
  if (subtype == GUID_NULL) {
    return nullptr;
  }
  const GUID iidTransform = {0xbf94c121, 0x5b05, 0x4e6f, {0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d}};
  const MFT_REGISTER_TYPE_INFO info = {MFMediaType_Audio, subtype};
  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &info,
                       &acts, &count)) ||
      count == 0 || acts == nullptr) {
    return nullptr;
  }
  IMFTransform* transform = nullptr;
  acts[0]->ActivateObject(iidTransform, reinterpret_cast<void**>(&transform));
  IMFMediaType* best = nullptr;
  int bestDistance = 0;
  const UINT32 wantAvg = kind == kRecFlac ? static_cast<UINT32>(anymix::kSampleRate) * 4 : 24000;
  if (transform != nullptr) {
    for (DWORD index = 0; index < 64; ++index) {
      IMFMediaType* available = nullptr;
      if (FAILED(transform->GetOutputAvailableType(0, index, &available)) || available == nullptr) {
        break;
      }
      UINT32 channels = 0;
      UINT32 rate = 0;
      UINT32 avg = 0;
      available->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
      available->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
      available->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avg);
      if (channels == 2 && rate == static_cast<UINT32>(anymix::kSampleRate)) {
        const int distance = avg == 0 ? 1000000 : std::abs(static_cast<int>(avg) - static_cast<int>(wantAvg));
        if (best == nullptr || distance < bestDistance) {
          if (best != nullptr) {
            best->Release();
          }
          best = available;
          bestDistance = distance;
          available = nullptr;
        }
      }
      if (available != nullptr) {
        available->Release();
      }
    }
    transform->Release();
  }
  for (UINT32 index = 0; index < count; ++index) {
    acts[index]->Release();
  }
  CoTaskMemFree(acts);
  return best;
}

bool FillOutputType(IMFMediaType* type, int kind) {
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
  type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(anymix::kSampleRate));
  if (kind == kRecMp3) {
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    return true;
  }
  if (kind == kRecFlac) {
    const GUID flac = {0x0000F1AC, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    type->SetGUID(MF_MT_SUBTYPE, flac);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(anymix::kSampleRate) * 4);
    return true;
  }
  if (kind == kRecAac) {
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    return true;
  }
  if (kind == kRecWma) {
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_WMAudioV8);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    return true;
  }
  return false;
}

bool TranscodeRecording(const wchar_t* source, const wchar_t* dest, int kind) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
    return false;
  }
  const bool ownCom = SUCCEEDED(com);
  bool ok = false;
  IMFSourceReader* reader = nullptr;
  IMFSinkWriter* writer = nullptr;
  IMFMediaType* pcm = nullptr;
  IMFMediaType* out = nullptr;
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
    if (ownCom) {
      CoUninitialize();
    }
    return false;
  }
  if (FAILED(MFCreateSourceReaderFromURL(source, nullptr, &reader)) || reader == nullptr) {
    MFShutdown();
    if (ownCom) {
      CoUninitialize();
    }
    return false;
  }
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
  if (SUCCEEDED(MFCreateMediaType(&pcm)) && pcm != nullptr) {
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(anymix::kSampleRate));
    pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(anymix::kSampleRate) * 4);
  }
  if (pcm != nullptr &&
      SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm))) {
    out = EncoderOutputType(kind);
    if (out == nullptr && SUCCEEDED(MFCreateMediaType(&out)) && (out == nullptr || !FillOutputType(out, kind))) {
      if (out != nullptr) {
        out->Release();
        out = nullptr;
      }
    }
  }
  if (pcm != nullptr && out != nullptr && SUCCEEDED(MFCreateSinkWriterFromURL(dest, nullptr, nullptr, &writer)) &&
      writer != nullptr) {
    DWORD stream = 0;
    if (SUCCEEDED(writer->AddStream(out, &stream)) && SUCCEEDED(writer->SetInputMediaType(stream, pcm, nullptr)) &&
        SUCCEEDED(writer->BeginWriting())) {
      ok = true;
      LONGLONG stamp = 0;
      while (ok) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        const HRESULT read = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                                                &flags, nullptr, &sample);
        if (FAILED(read) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
          if (sample != nullptr) {
            sample->Release();
          }
          ok = SUCCEEDED(read);
          break;
        }
        if (sample == nullptr) {
          continue;
        }
        IMFMediaBuffer* buffer = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr &&
            SUCCEEDED(buffer->GetCurrentLength(&length))) {
          const LONGLONG frames = static_cast<LONGLONG>(length / 4);
          const LONGLONG duration = frames * 10000000 / anymix::kSampleRate;
          sample->SetSampleTime(stamp);
          sample->SetSampleDuration(duration);
          if (FAILED(writer->WriteSample(stream, sample))) {
            ok = false;
          }
          stamp += duration;
          buffer->Release();
        } else {
          ok = false;
        }
        sample->Release();
      }
      if (ok) {
        ok = SUCCEEDED(writer->Finalize());
      }
    }
  }
  if (out != nullptr) {
    out->Release();
  }
  if (pcm != nullptr) {
    pcm->Release();
  }
  if (writer != nullptr) {
    writer->Release();
  }
  reader->Release();
  MFShutdown();
  if (ownCom) {
    CoUninitialize();
  }
  return ok;
}

struct EncodeJob {
  wchar_t source[MAX_PATH];
  wchar_t dest[MAX_PATH];
  int kind;
};

void WithExtension(wchar_t* dest, int count, const wchar_t* source, const wchar_t* ext) {
  wcsncpy(dest, source, static_cast<size_t>(count - 1));
  dest[count - 1] = 0;
  wchar_t* dot = wcsrchr(dest, L'.');
  wchar_t* slash = wcsrchr(dest, L'\\');
  if (dot != nullptr && (slash == nullptr || dot > slash)) {
    *dot = 0;
  }
  wcsncat(dest, ext, static_cast<size_t>(count - static_cast<int>(wcslen(dest)) - 1));
}

DWORD WINAPI EncodeProc(LPVOID param) {
  auto* job = static_cast<EncodeJob*>(param);
  const bool ok = TranscodeRecording(job->source, job->dest, job->kind);
  if (ok) {
    DeleteFileW(job->source);
    wcsncpy(g_lastTake, job->dest, MAX_PATH - 1);
    g_lastTake[MAX_PATH - 1] = 0;
    g_encodeState.store(2);
  } else {
    DeleteFileW(job->dest);
    wcsncpy(g_lastTake, job->source, MAX_PATH - 1);
    g_lastTake[MAX_PATH - 1] = 0;
    g_encodeState.store(3);
  }
  delete job;
  return 0;
}

void SaveRecordFormat() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  const DWORD value = static_cast<DWORD>(std::max(0, std::min(kRecFormatCount - 1, g_recFormat)));
  RegSetValueExW(key, L"RecordFormat", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

void LoadRecordFormat() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnyMix", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"RecordFormat", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
  RegCloseKey(key);
  if (status == ERROR_SUCCESS && type == REG_DWORD && value < static_cast<DWORD>(kRecFormatCount)) {
    g_recFormat = static_cast<int>(value);
  }
}

void BeginEncode(const wchar_t* wav) {
  if (g_encodeThread != nullptr && WaitForSingleObject(g_encodeThread, 0) == WAIT_OBJECT_0) {
    CloseHandle(g_encodeThread);
    g_encodeThread = nullptr;
  }
  if (g_encodeState.load() == 1) {
    wcsncpy(g_lastTake, wav, MAX_PATH - 1);
    g_lastTake[MAX_PATH - 1] = 0;
    return;
  }
  const int format = std::max(0, std::min(kRecFormatCount - 1, g_recFormat));
  auto* job = new EncodeJob{};
  wcsncpy(job->source, wav, MAX_PATH - 1);
  job->source[MAX_PATH - 1] = 0;
  WithExtension(job->dest, MAX_PATH, wav, kRecFormats[format].ext);
  job->kind = kRecFormats[format].kind;
  g_encodeState.store(1);
  g_encodeThread = CreateThread(nullptr, 0, EncodeProc, job, 0, nullptr);
  if (g_encodeThread == nullptr) {
    delete job;
    wcsncpy(g_lastTake, wav, MAX_PATH - 1);
    g_lastTake[MAX_PATH - 1] = 0;
    g_encodeState.store(3);
  }
}

void OnClick(HWND window, int id) {
  if (id >= NavId(0) && id < NavId(kPageCount)) {
    const Page next = static_cast<Page>(id - NavId(0));
    if (next != g_page) {
      g_page = next;
      g_pageAlpha = 0.f;
      g_appsOpen = -1;
    }
    g_pop = 0;
    return;
  }
  if (id == 8730) {
    int group = -1;
    for (int i = 0; i < anymix::kVstGroups; ++i) {
      if (!g_vstGroup[i].used) {
        group = i;
        break;
      }
    }
    if (group < 0) {
      wcsncpy(g_vstStatus, L"Four groups is the limit.", 159);
    } else {
      g_vstGroup[group].used = true;
      g_vstGroup[group].bypass = false;
      swprintf(g_vstGroup[group].name, 32, L"Group %d", group + 1);
      if (g_engine != nullptr) {
        g_engine->SetVstGroupBypass(group, false);
      }
      wcsncpy(g_vstStatus, L"Drag a plug-in onto the group.", 159);
      SaveVstGroups();
    }
    g_vstStatus[159] = 0;
    return;
  }
  if (id >= 8740 && id < 8740 + anymix::kVstGroups) {
    const int group = id - 8740;
    if (g_vstGroup[group].used) {
      g_vstGroup[group].bypass = !g_vstGroup[group].bypass;
      if (g_engine != nullptr) {
        g_engine->SetVstGroupBypass(group, g_vstGroup[group].bypass);
      }
      SaveVstGroups();
    }
    return;
  }
  if (id >= 8750 && id < 8750 + anymix::kVstGroups && g_engine != nullptr) {
    const int group = id - 8750;
    if (g_vstGroup[group].used) {
      for (int slot = 0; slot < anymix::kVstSlots; ++slot) {
        anymix::VstSlotView view{};
        g_engine->VstSlot(slot, view);
        if (view.used && view.group == group) {
          g_engine->RemoveVst(slot);
        }
      }
      g_vstGroup[group].used = false;
      g_vstGroup[group].bypass = false;
      g_vstGroup[group].name[0] = 0;
      g_engine->SetVstGroupBypass(group, false);
      wcsncpy(g_vstStatus, L"Group removed.", 159);
      g_vstStatus[159] = 0;
      SaveVstRack();
    }
    return;
  }
  if (id == 8801) {
    ScanVstFolders();
    return;
  }
  if (id >= 8810 && id < 8810 + anymix::kStrips) {
    g_vstStrip = id - 8810;
    return;
  }
  if (id >= 8820 && id < 8820 + 128) {
    const int index = id - 8820;
    if (g_engine != nullptr && index >= 0 && index < g_vstFileCount) {
      std::wstring error;
      const int slot = g_engine->AddVst(g_vstStrip, g_vstFiles[index].path, error);
      if (slot < 0) {
        wcsncpy(g_vstStatus, error.empty() ? L"The plug-in did not load." : error.c_str(), 159);
      } else {
        wcsncpy(g_vstStatus, L"Plug-in loaded on the selected strip.", 159);
        SaveVstRack();
      }
      g_vstStatus[159] = 0;
    }
    return;
  }
  if (id >= 8950 && id < 8950 + anymix::kVstSlots && g_engine != nullptr) {
    const int slot = id - 8950;
    anymix::VstSlotView view{};
    g_engine->VstSlot(slot, view);
    if (view.used) {
      g_engine->SetVstBypass(slot, !view.bypass);
      SaveVstRack();
    }
    return;
  }
  if (id >= 8960 && id < 8960 + anymix::kVstSlots && g_engine != nullptr) {
    g_engine->ToggleVstEditor(id - 8960);
    return;
  }
  if (id >= 8970 && id < 8970 + anymix::kVstSlots && g_engine != nullptr) {
    g_engine->RemoveVst(id - 8970);
    wcsncpy(g_vstStatus, L"Plug-in removed.", 159);
    g_vstStatus[159] = 0;
    SaveVstRack();
    return;
  }
  if (id == 5001) {
    g_dark = true;
  } else if (id == 5002) {
    g_dark = false;
  } else if (id == 5003) {
    g_compact = false;
  } else if (id == 5004) {
    g_compact = true;
  } else if (id == 5005) {
    g_anim = !g_anim;
  } else if (id == 5006) {
    g_closeToTray = !g_closeToTray;
    SaveCloseToTray();
    if (!g_closeToTray) {
      RemoveTray(window);
    }
  } else if (id == 5007) {
    g_startWithWindows = !g_startWithWindows;
    if (!SyncWindowsStartup()) {
      g_startWithWindows = !g_startWithWindows;
      MessageBoxW(window, L"Windows did not accept the startup entry.", L"AnyMix", MB_OK | MB_ICONWARNING);
    }
  } else if (id >= 2000 && id < 2200) {
    g_pop = id;
    g_popScroll = 0;
    if (const Hot* hot = HotOf(id)) {
      g_popAnchor = hot->r;
    }
  } else if (id >= 8200 && id < 8205) {
    g_appTab = id - 8200;
    g_pop = 0;
  } else if (id >= 8100 && id < 8105) {
    g_pop = id;
    g_popScroll = 0;
    if (const Hot* hot = HotOf(id)) {
      g_popAnchor = hot->r;
    }
  } else if (id >= 4200 && id < 4200 + anymix::kBuses) {
    const int bus = id - 4200;
    g_recMask ^= 1u << bus;
    if (g_engine != nullptr) {
      g_engine->SetMonitorMask(g_recMask);
    }
    SaveState();
  } else if (id >= 4300 && id < 4300 + anymix::kBuses) {
    const int bus = id - 4300;
    g_playMask ^= 1u << bus;
    if (g_engine != nullptr) {
      g_engine->SetPlaybackMask(g_playMask);
    }
    SaveState();
  } else if (id == 4000) {
    g_pop = id;
    g_popScroll = 0;
    if (const Hot* hot = HotOf(id)) {
      g_popAnchor = hot->r;
    }
  } else if (id >= 3000 && id < 4000 && g_pop != 0) {
    const int index = g_popScroll + (id - 3000);
    if (g_pop >= 2000 && g_pop < 2100) {
      const int mic = g_pop - 2000;
      if (mic >= 0 && mic < anymix::kMicStrips && index >= 0 &&
          index < static_cast<int>(g_captures.size())) {
        g_micUser[mic] = true;
        g_micSel[mic] = index;
        if (index > 0) {
          CopyText(g_savedMic[mic], 512, g_captures[static_cast<size_t>(index)].id);
        } else {
          g_savedMic[mic][0] = 0;
        }
        PushDevices();
        SaveState();
      }
    } else if (g_pop >= 2100 && g_pop < 2200) {
      const int bus = g_pop - 2100;
      if (bus >= 0 && bus < anymix::kPhysicalBuses && index >= 0 &&
          index < static_cast<int>(g_renders.size())) {
        g_outSel[bus] = index;
        if (index > 0) {
          CopyText(g_savedOut[bus], 512, g_renders[static_cast<size_t>(index)].id);
        } else {
          g_savedOut[bus][0] = 0;
        }
        PushDevices();
        SaveState();
      }
    } else if (g_pop == 4000 && index >= 0 && index < anymix::kBuses) {
      g_recBus = index;
    } else if (g_pop == 4007 && index >= 0 && index < kRecFormatCount) {
      g_recFormat = index;
      SaveRecordFormat();
    } else if (g_pop >= 8100 && g_pop < 8105) {
      const int strip = g_pop - 8100;
      const wchar_t* endpoint = VirtualId(strip);
      auto& sessions = g_sessions[static_cast<size_t>(strip)];
      if (endpoint != nullptr && index >= 0 && index < static_cast<int>(sessions.size())) {
        std::wstring error;
        if (!anymix::SetSessionMute(endpoint, sessions[static_cast<size_t>(index)].sessionId,
                                    !sessions[static_cast<size_t>(index)].muted, error)) {
          MessageBoxW(window, error.c_str(), L"AnyMix", MB_OK | MB_ICONWARNING);
        }
        RefreshSessions();
      }
    }
    g_pop = 0;
  } else if (id == 4001) {
    std::wstring error;
    if (!g_engine->StartRecording(g_recBus, g_folder, error)) {
      MessageBoxW(window, error.c_str(), L"AnyMix", MB_OK | MB_ICONWARNING);
    } else {
      g_recStart = GetTickCount64();
    }
  } else if (id == 4002) {
    g_engine->StopRecording();
    g_recStart = 0;
    const wchar_t* path = g_engine->RecordingPath();
    if (path != nullptr && path[0] != 0) {
      const int format = std::max(0, std::min(kRecFormatCount - 1, g_recFormat));
      if (kRecFormats[format].kind == kRecWav) {
        wcsncpy(g_lastTake, path, MAX_PATH - 1);
        g_lastTake[MAX_PATH - 1] = 0;
        g_encodeState.store(0);
      } else {
        BeginEncode(path);
      }
    }
  } else if (id == 4007) {
    g_pop = 4007;
    g_popScroll = 0;
    if (const Hot* hot = HotOf(id)) {
      g_popAnchor = hot->r;
    }
  } else if (id == 4004) {
    PlayLastTake(window);
  } else if (id == 4005) {
    OpenAudioFile(window);
  } else if (id == 4006) {
    StopPlayback();
  } else if (id == 4101) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash != nullptr) {
      *slash = 0;
    }
    wchar_t dll[MAX_PATH];
    wsprintfW(dll, L"%s\\AnyMixInsertASIO.dll", exe);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
      MessageBoxW(window, L"AnyMixInsertASIO.dll was not found next to AnyMix.", L"AnyMix",
                  MB_OK | MB_ICONWARNING);
      return;
    }
    wchar_t params[MAX_PATH + 8];
    wsprintfW(params, L"\"%s\"", dll);
    const HINSTANCE launched =
        ShellExecuteW(window, L"runas", L"regsvr32.exe", params, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
      MessageBoxW(window, L"Windows did not start the registration.", L"AnyMix", MB_OK | MB_ICONWARNING);
    }
  } else if (id == 4003) {
    BROWSEINFOW browse{};
    browse.hwndOwner = window;
    browse.lpszTitle = L"Recording folder";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browse);
    if (pidl != nullptr) {
      SHGetPathFromIDListW(pidl, g_folder);
      CoTaskMemFree(pidl);
    }
  } else if (id >= 7100 && id < 7400) {
    const int bus = id % 100;
    if (bus < 0 || bus >= anymix::kBuses) {
      return;
    }
    if (id >= 7300) {
      if (bus >= anymix::kPhysicalBuses) {
        return;
      }
      SetBusSolo(bus, !g_busSolo[bus]);
    } else if (id >= 7200) {
      g_busMono[bus] = !g_busMono[bus];
      g_engine->SetBusMono(bus, g_busMono[bus]);
    } else {
      g_busMute[bus] = !g_busMute[bus];
      g_engine->SetBusMute(bus, g_busMute[bus]);
    }
  } else if (id >= 6000 && id < 7000) {
    const int strip = (id - 6000) / 32;
    const int row = (id - 6000) % 32;
    const wchar_t* endpoint = VirtualId(strip);
    if (endpoint == nullptr) {
      MessageBoxW(window, L"AnyMix virtual playback device is not installed, so Windows has no application stream to mute.",
                  L"AnyMix", MB_OK | MB_ICONINFORMATION);
      return;
    }
    auto& sessions = g_sessions[static_cast<size_t>(strip)];
    if (row < 0 || row >= static_cast<int>(sessions.size())) {
      return;
    }
    std::wstring error;
    if (!anymix::SetSessionMute(endpoint, sessions[static_cast<size_t>(row)].sessionId,
                                !sessions[static_cast<size_t>(row)].muted, error)) {
      MessageBoxW(window, error.c_str(), L"AnyMix", MB_OK | MB_ICONWARNING);
    }
    RefreshSessions();
  } else if (id == 5200) {
    EnterMini();
  } else if (id >= 1000) {
    const int strip = (id - 1000) / 40;
    const int part = (id - 1000) % 40;
    if (strip < 0 || strip >= anymix::kStrips) {
      return;
    }
    g_focusStrip = strip;
    if (part == kMute) {
      g_strip[strip].mute = !g_strip[strip].mute;
      g_engine->SetMute(strip, g_strip[strip].mute);
    } else if (part == kSolo) {
      SetStripSolo(strip, !g_strip[strip].solo);
    } else if (part == kMono) {
      g_strip[strip].mono = !g_strip[strip].mono;
      g_engine->SetMono(strip, g_strip[strip].mono);
    } else if (part == kInsert) {
      g_strip[strip].insert = !g_strip[strip].insert;
      g_engine->SetInsert(strip, g_strip[strip].insert);
      SaveRoutes();
    } else if (part == kLocal) {
      g_strip[strip].local = !g_strip[strip].local;
      g_engine->SetLocalVst(strip, g_strip[strip].local);
      SaveVstLocal();
    } else if (part >= kRoute0 && part < kRoute0 + anymix::kBuses) {
      ToggleRoute(strip, part - kRoute0);
    }
  } else {
    g_pop = 0;
  }
}

bool PointerOnStrip(int strip) {
  if (g_hover >= 1000 && g_hover < 1000 + anymix::kStrips * 40) {
    return (g_hover - 1000) / 40 == strip;
  }
  if (g_hover >= 6000 && g_hover < 6000 + anymix::kVirtualStrips * 32) {
    return (g_hover - 6000) / 32 == strip;
  }
  if (g_drag >= 1000 && g_drag < 1000 + anymix::kStrips * 40) {
    return (g_drag - 1000) / 40 == strip;
  }
  return false;
}

bool PointerOnBus(int bus) {
  if (g_hover >= 7000 && g_hover < 7400 && (g_hover % 100) == bus) {
    return true;
  }
  if (g_hover == 2100 + bus || g_drag == 7000 + bus) {
    return true;
  }
  return false;
}

void FollowPeak(float& meter, float peak) {
  if (!g_anim || peak >= meter) {
    meter = peak;
  } else {
    const float dt = std::max(0.001f, g_frameDt);
    meter *= std::exp(std::log(0.82f) / 0.033f * dt);
    if (meter < peak) {
      meter = peak;
    }
    if (meter < 0.00001f) {
      meter = 0.f;
    }
  }
}

void TickAppMeters() {
  if (g_page != kApps && g_page != kMixer) {
    return;
  }
  static ULONGLONG lastSample = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - lastSample >= 40) {
    lastSample = now;
    for (int strip = 0; strip < anymix::kVirtualStrips; ++strip) {
      const wchar_t* endpoint = VirtualId(strip);
      auto& sessions = g_sessions[static_cast<size_t>(strip)];
      if (endpoint == nullptr || sessions.empty()) {
        continue;
      }
      anymix::SampleSessionPeaks(endpoint, sessions.data(), static_cast<int>(sessions.size()));
    }
  }
  for (int strip = 0; strip < anymix::kVirtualStrips; ++strip) {
    const auto& sessions = g_sessions[static_cast<size_t>(strip)];
    const int rows = std::min(32, static_cast<int>(sessions.size()));
    for (int i = 0; i < rows; ++i) {
      const unsigned long id = sessions[static_cast<size_t>(i)].sessionId;
      if (g_appMeterId[strip][i] != id) {
        g_appMeter[strip][i] = sessions[static_cast<size_t>(i)].peak;
        g_appMeterId[strip][i] = id;
      } else {
        FollowPeak(g_appMeter[strip][i], sessions[static_cast<size_t>(i)].peak);
      }
    }
  }
}

void TickMeters() {
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  g_frameDt = last == 0 ? 0.016f : std::min(0.05f, static_cast<float>(now - last) * 0.001f);
  last = now;
  TickAppMeters();
  for (int i = 0; i < anymix::kStrips; ++i) {
    float left = 0.f;
    float right = 0.f;
    g_engine->PeakStereo(i, left, right);
    FollowPeak(g_meterL[i], left);
    FollowPeak(g_meterR[i], right);
    const int mic = MicOf(i);
    if (mic >= 0) {
      g_gateOpen[mic] = g_engine->GateOpen(i);
    }
    g_meter[i] = std::max(g_meterL[i], g_meterR[i]);
    Approach(g_stripMix[i], PointerOnStrip(i) ? 1.f : 0.f);
  }
  for (int i = 0; i < anymix::kBuses; ++i) {
    float left = 0.f;
    float right = 0.f;
    g_engine->BusPeakStereo(i, left, right);
    FollowPeak(g_busMeterL[i], left);
    FollowPeak(g_busMeterR[i], right);
    g_busMeter[i] = std::max(g_busMeterL[i], g_busMeterR[i]);
    Approach(g_busMix[i], PointerOnBus(i) ? 1.f : 0.f);
  }
  for (int i = 0; i < kPageCount; ++i) {
    const bool on = g_page == static_cast<Page>(i) || g_hover == NavId(i);
    Approach(g_navMix[i], on ? 1.f : 0.f);
  }
  if (g_pageAlpha < 1.f) {
    g_pageAlpha = std::min(1.f, g_pageAlpha + 0.14f * (g_frameDt / 0.033f));
  }
}

void CommitRename(HWND window) {
  if (g_edit == nullptr || (g_editStrip < 0 && g_editGroup < 0)) {
    return;
  }
  wchar_t text[32]{};
  GetWindowTextW(g_edit, text, 32);
  HWND edit = g_edit;
  const int strip = g_editStrip;
  const int group = g_editGroup;
  g_edit = nullptr;
  g_editStrip = -1;
  g_editGroup = -1;
  DestroyWindow(edit);
  if (text[0] != 0 && group >= 0 && group < anymix::kVstGroups && g_vstGroup[group].used) {
    CopyText(g_vstGroup[group].name, 32, text);
    SaveVstGroups();
  } else if (text[0] != 0 && strip >= 0 && strip < anymix::kStrips) {
    CopyText(g_label[strip], 32, text);
  }
  InvalidateRect(window, nullptr, FALSE);
}

void CancelRename() {
  if (g_edit == nullptr) {
    return;
  }
  HWND edit = g_edit;
  g_edit = nullptr;
  g_editStrip = -1;
  g_editGroup = -1;
  DestroyWindow(edit);
}

LRESULT CALLBACK EditProc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
    HWND parent = GetParent(edit);
    if (wparam == VK_RETURN) {
      CommitRename(parent);
    } else {
      CancelRename();
      InvalidateRect(parent, nullptr, FALSE);
    }
    return 0;
  }
  return CallWindowProcW(g_editProc, edit, message, wparam, lparam);
}

void BeginRename(HWND window, int strip, RECT r) {
  if (strip < 0 || strip >= anymix::kStrips) {
    return;
  }
  CancelRename();
  g_editGroup = -1;
  g_editStrip = strip;
  g_edit = CreateWindowExW(0, L"EDIT", StripLabel(strip),
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER, r.left, r.top,
                           std::max(40, static_cast<int>(r.right - r.left)),
                           std::max(18, static_cast<int>(r.bottom - r.top)), window, nullptr, nullptr,
                           nullptr);
  if (g_edit == nullptr) {
    g_editStrip = -1;
    return;
  }
  HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  SendMessageW(g_edit, EM_SETLIMITTEXT, 31, 0);
  g_editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
  SetFocus(g_edit);
  SendMessageW(g_edit, EM_SETSEL, 0, -1);
}

void BeginGroupRename(HWND window, int group, RECT r) {
  if (group < 0 || group >= anymix::kVstGroups || !g_vstGroup[group].used) {
    return;
  }
  CancelRename();
  g_editStrip = -1;
  g_editGroup = group;
  g_edit = CreateWindowExW(0, L"EDIT", g_vstGroup[group].name, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER,
                           r.left, r.top, std::max(48, static_cast<int>(r.right - r.left)),
                           std::max(18, static_cast<int>(r.bottom - r.top)), window, nullptr, nullptr, nullptr);
  if (g_edit == nullptr) {
    g_editGroup = -1;
    return;
  }
  HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  SendMessageW(g_edit, EM_SETLIMITTEXT, 31, 0);
  g_editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
  SetFocus(g_edit);
  SendMessageW(g_edit, EM_SETSEL, 0, -1);
}

int StripAt(HWND window, int x, int y) {
  if (g_page != kMixer) {
    return -1;
  }
  RECT client{};
  GetClientRect(window, &client);
  const int w = client.right;
  const int h = client.bottom;
  const int side = w >= 1180 ? 196 : 68;
  const int topH = 48;
  const int mixerTop = topH + 8;
  const int mixerBottom = h - 36;
  if (y < mixerTop || y > mixerBottom) {
    return -1;
  }
  const int masterW = 100;
  const int gap = 8;
  const int mastersW = anymix::kPhysicalBuses * masterW + (anymix::kPhysicalBuses - 1) * gap;
  const int shown = 6;
  const int viewW = std::max(0, w - side - mastersW - 24);
  const int gaps = (shown - 1) * gap;
  const int stripW = viewW > gaps ? (viewW - gaps) / shown : 1;
  const int order[6] = {anymix::kMic1, anymix::kGame, anymix::kChat, anymix::kMusic, anymix::kAux, anymix::kAdd};
  for (int slot = 0; slot < shown; ++slot) {
    const int left = side + 8 + slot * (stripW + gap);
    if (x >= left && x < left + stripW && order[slot] < anymix::kVirtualStrips) {
      return order[slot];
    }
  }
  return -1;
}

void EndAppPointer(HWND window, int x, int y) {
  const bool drag = g_appDrag;
  const int from = g_appFrom;
  const int row = g_appRow;
  const unsigned long pid = g_appPid;
  g_appPending = false;
  g_appDrag = false;
  g_dropStrip = -1;
  ReleaseCapture();
  if (drag) {
    const int to = StripAt(window, x, y);
    if (to >= 0 && to != from) {
      const wchar_t* dest = VirtualId(to);
      if (dest != nullptr) {
        std::wstring error;
        anymix::MoveSession(pid, dest, error);
        RefreshSessions();
      }
    }
  } else if (from >= 0 && row >= 0) {
    OnClick(window, 6000 + from * 32 + row);
  }
}

int CaptionAt(HWND window, int x, int y) {
  if (y < 0 || y >= 48) {
    return 0;
  }
  RECT client{};
  GetClientRect(window, &client);
  if (x < 0 || x >= client.right) {
    return 0;
  }
  RECT miniR{};
  RECT minR{};
  RECT maxR{};
  RECT closeR{};
  CaptionRects(client.right, miniR, minR, maxR, closeR);
  const POINT pt{x, y};
  if (PtInRect(&closeR, pt)) {
    return 3;
  }
  if (PtInRect(&maxR, pt)) {
    return 2;
  }
  if (PtInRect(&minR, pt)) {
    return 1;
  }
  if (PtInRect(&miniR, pt)) {
    return 4;
  }
  return 0;
}

void RunCaption(HWND window, int which) {
  if (which == 1) {
    ShowWindow(window, SW_MINIMIZE);
  } else if (which == 2) {
    ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
    ApplyWindowFrame(window);
  } else if (which == 3) {
    SendMessageW(window, WM_CLOSE, 0, 0);
  } else if (which == 4) {
    EnterMini();
  }
}

void CaptionRects(int width, RECT& miniR, RECT& minR, RECT& maxR, RECT& closeR) {
  const int bw = 46;
  closeR = R(width - bw, 0, bw, 48);
  maxR = R(width - bw * 2, 0, bw, 48);
  minR = R(width - bw * 3, 0, bw, 48);
  miniR = R(width - bw * 4, 0, bw, 48);
}

const int kMiniOrder[5] = {anymix::kGame, anymix::kChat, anymix::kMusic, anymix::kAux, anymix::kMic1};
const wchar_t* kMiniLabels[5] = {L"GAME", L"CHAT", L"MUSIC", L"AUX", L"MIC"};

void MiniSliderRect(int index, int width, RECT& slider) {
  const int y = 48 + index * 48;
  slider = R(16, y + 34, std::max(40, width - 32), 12);
}

void GainFromMini(int index, int x) {
  if (index < 0 || index >= 5) {
    return;
  }
  RECT slider{};
  RECT client{};
  if (g_miniWindow != nullptr) {
    GetClientRect(g_miniWindow, &client);
  }
  MiniSliderRect(index, std::max(1, static_cast<int>(client.right)), slider);
  const int span = std::max(1, static_cast<int>(slider.right - slider.left));
  float t = static_cast<float>(x - slider.left) / static_cast<float>(span);
  t = std::max(0.f, std::min(1.f, t));
  const int strip = kMiniOrder[index];
  g_strip[strip].gain = anymix::kFaderMinDb + t * (anymix::kFaderMaxDb - anymix::kFaderMinDb);
  g_engine->SetGainDb(strip, g_strip[strip].gain);
}

bool EnsureMiniTarget(HWND window, int w, int h) {
  if (g_d2d == nullptr && !EnsureTarget(g_mainWindow != nullptr ? g_mainWindow : window, 8, 8)) {
    return false;
  }
  if (g_miniRt != nullptr) {
    const D2D1_SIZE_U size = g_miniRt->GetPixelSize();
    if (size.width != static_cast<UINT32>(w) || size.height != static_cast<UINT32>(h)) {
      g_miniRt->Resize(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)));
    }
    return g_miniBrush != nullptr;
  }
  const D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f,
      96.f);
  const D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
      window, D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
  if (FAILED(g_d2d->CreateHwndRenderTarget(rtp, hp, &g_miniRt)) || g_miniRt == nullptr) {
    return false;
  }
  g_miniRt->SetDpi(96.f, 96.f);
  if (FAILED(g_miniRt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f), &g_miniBrush)) || g_miniBrush == nullptr) {
    g_miniRt->Release();
    g_miniRt = nullptr;
    return false;
  }
  return true;
}

void PaintMini(HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  const int w = std::max(1, static_cast<int>(client.right));
  const int h = std::max(1, static_cast<int>(client.bottom));
  if (!EnsureMiniTarget(window, w, h)) {
    return;
  }
  g_miniRt->BeginDraw();
  ID2D1SolidColorBrush* saved = g_brush;
  g_brush = g_miniBrush;
  Canvas g{g_miniRt};
  g_hot.clear();
  const Palette pal = Colors();
  FillRR(g, R(0, 0, w, h), 0, pal.bg);
  Text(g, L"AnyMix", R(16, 8, 160, 32), pal.text, 18, false);
  RECT back = R(w - 46, 0, 46, 48);
  if (g_miniHover == 5511) {
    DrawCaptionPlate(g, back, pal.surface);
  }
  DrawCaptionIcon(g, back, 4, false, pal.text, g_miniHover == 5511 ? pal.surface : pal.bg);
  AddHot(back, 5511, L"Grand mixer");
  for (int i = 0; i < 5; ++i) {
    const int strip = kMiniOrder[i];
    const int y = 48 + i * 48;
    DrawIcon(g, strip, 14.f, static_cast<float>(y + 1), 18.f, pal.text);
    Text(g, kMiniLabels[i], R(38, y, 120, 20), pal.text, 12, false);
    if (strip == anymix::kMic1) {
      const bool armed = g_micGate[0].level >= 0.05f;
      const bool open = armed && g_gateOpen[0];
      Gdiplus::SolidBrush led(armed && open ? pal.ok : (armed ? pal.warn : pal.line));
      g.FillEllipse(&led, static_cast<float>(w - 72), static_cast<float>(y + 4), 8.f, 8.f);
    }
    const float db = DbOf(g_strip[strip].mute ? 0.f : g_meter[strip]);
    const float level = std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
    RECT vu = R(16, y + 22, w - 32, 6);
    FillRR(g, vu, 3, pal.bg);
    const int vuW = static_cast<int>(level * static_cast<float>(vu.right - vu.left));
    if (vuW > 1) {
      const Gdiplus::Color zone = g_strip[strip].mute ? pal.dim : (db >= -6.f ? pal.clip : pal.meter);
      FillRR(g, R(vu.left, vu.top, vuW, vu.bottom - vu.top), 3, zone);
    }
    RECT slider{};
    MiniSliderRect(i, w, slider);
    const float t = std::max(0.f, std::min(1.f, (g_strip[strip].gain - anymix::kFaderMinDb) /
                                                     (anymix::kFaderMaxDb - anymix::kFaderMinDb)));
    FillRR(g, R(slider.left, slider.top + 3, slider.right - slider.left, 6), 3, pal.line);
    const int fill = static_cast<int>(t * static_cast<float>(slider.right - slider.left));
    if (fill > 0) {
      FillRR(g, R(slider.left, slider.top + 3, fill, 6), 3, pal.accent);
    }
    const int knob = slider.left + fill;
    Gdiplus::SolidBrush knobBrush(pal.text);
    g.FillEllipse(&knobBrush, static_cast<float>(knob - 6), static_cast<float>(slider.top), 12.f, 12.f);
    wchar_t pct[8];
    wsprintfW(pct, L"%d%%", static_cast<int>(t * 100.f + 0.5f));
    Text(g, pct, R(w - 52, y, 40, 20), pal.dim, 12, true);
    AddHot(R(slider.left, slider.top - 4, slider.right - slider.left, 20), 5500 + i, L"Volume");
  }
  const bool muted = g_strip[anymix::kMic1].mute;
  RECT mute = R(16, h - 52, w - 32, 36);
  FillRR(g, mute, 8, muted ? pal.mute : pal.surface);
  DrawIcon(g, anymix::kMic1, static_cast<float>(mute.left + 28), static_cast<float>(mute.top + 7), 22.f,
           muted ? Gdiplus::Color(255, 255, 255, 255) : pal.text);
  Text(g, muted ? L"MIC MUTED" : L"MUTE MIC", R(mute.left + 52, mute.top, mute.right - mute.left - 64, 36),
       muted ? Gdiplus::Color(255, 255, 255, 255) : pal.text, 13, true);
  AddHot(mute, 5510, L"Mute the microphone");
  const HRESULT drawn = g_miniRt->EndDraw();
  g_brush = saved;
  if (drawn == static_cast<HRESULT>(0x8899000CL)) {
    if (g_miniBrush != nullptr) {
      g_miniBrush->Release();
      g_miniBrush = nullptr;
    }
    if (g_miniRt != nullptr) {
      g_miniRt->Release();
      g_miniRt = nullptr;
    }
  }
}

void LeaveMini() {
  g_miniMode = false;
  g_miniDrag = -1;
  if (g_miniWindow != nullptr) {
    ShowWindow(g_miniWindow, SW_HIDE);
  }
  if (g_mainWindow != nullptr) {
    ShowWindow(g_mainWindow, SW_SHOW);
    ApplyWindowFrame(g_mainWindow);
    SetForegroundWindow(g_mainWindow);
  }
}

void EnterMini() {
  NoteMainWindow();
  if (g_mainWindow == nullptr) {
    return;
  }
  if (g_miniWindow == nullptr) {
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.style = CS_DBLCLKS;
    klass.lpfnWndProc = MiniProc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"AnyMixMini";
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&klass);
    RECT main{};
    GetWindowRect(g_mainWindow, &main);
    const int mw = 320;
    const int mh = 348;
    int x = main.right - mw - 16;
    int y = main.top + 48;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (x < work.left) {
      x = work.left;
    }
    if (y < work.top) {
      y = work.top;
    }
    if (x + mw > work.right) {
      x = work.right - mw;
    }
    if (y + mh > work.bottom) {
      y = work.bottom - mh;
    }
    g_miniWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AnyMixMini", L"AnyMix", WS_POPUP, x, y, mw, mh,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_miniWindow == nullptr) {
      return;
    }
    ApplyWindowFrame(g_miniWindow);
  }
  g_miniMode = true;
  ShowWindow(g_miniWindow, SW_SHOW);
  SetWindowPos(g_miniWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(g_miniWindow);
  ShowWindow(g_mainWindow, SW_HIDE);
}

LRESULT CALLBACK MiniProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(window, &ps);
      PaintMini(window);
      EndPaint(window, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &pt);
      RECT client{};
      GetClientRect(window, &client);
      if (pt.y >= 0 && pt.y < 48 && pt.x >= 0 && pt.x < client.right - 46) {
        return HTCAPTION;
      }
      return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      if (g_miniDrag >= 0) {
        GainFromMini(g_miniDrag, x);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      const int id = HitAt(x, y);
      if (id != g_miniHover) {
        g_miniHover = id;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const int x = GET_X_LPARAM(lparam);
      const int id = HitAt(x, GET_Y_LPARAM(lparam));
      if (id >= 5500 && id < 5505) {
        g_miniDrag = id - 5500;
        SetCapture(window);
        GainFromMini(g_miniDrag, x);
        InvalidateRect(window, nullptr, FALSE);
      } else if (id == 5510) {
        g_strip[anymix::kMic1].mute = !g_strip[anymix::kMic1].mute;
        g_engine->SetMute(anymix::kMic1, g_strip[anymix::kMic1].mute);
        InvalidateRect(window, nullptr, FALSE);
      } else if (id == 5511) {
        LeaveMini();
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      const int id = HitAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (id >= 5500 && id < 5505) {
        const int strip = kMiniOrder[id - 5500];
        g_strip[strip].gain = 0.f;
        g_engine->SetGainDb(strip, 0.f);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      g_miniDrag = -1;
      ReleaseCapture();
      return 0;
    case WM_EXITSIZEMOVE:
      NoteMiniWindow();
      return 0;
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &pt);
      const int id = HitAt(pt.x, pt.y);
      if (id >= 5500 && id < 5505) {
        const int strip = kMiniOrder[id - 5500];
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        g_strip[strip].gain = std::max(anymix::kFaderMinDb,
                                       std::min(anymix::kFaderMaxDb, g_strip[strip].gain + static_cast<float>(delta)));
        g_engine->SetGainDb(strip, g_strip[strip].gain);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case WM_CLOSE:
      LeaveMini();
      return 0;
    case WM_DESTROY:
      g_miniDrag = -1;
      if (g_miniBrush != nullptr) {
        g_miniBrush->Release();
        g_miniBrush = nullptr;
      }
      if (g_miniRt != nullptr) {
        g_miniRt->Release();
        g_miniRt = nullptr;
      }
      if (g_miniWindow == window) {
        g_miniWindow = nullptr;
      }
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void ClearVstDrag() {
  g_vstPending = false;
  g_vstDrag = false;
  g_vstFromSlot = -1;
  g_vstFromFile = -1;
  g_vstHoverGroup = -2;
  g_vstDragName[0] = 0;
}

void FinishVstDrag(HWND window, int x, int y) {
  const bool dragged = g_vstDrag;
  const int slot = g_vstFromSlot;
  const int file = g_vstFromFile;
  const int drop = dragged && g_page == kVst ? VstDropGroup(x, y) : -2;
  ClearVstDrag();
  if (!dragged) {
    if (file >= 0) {
      OnClick(window, 8820 + file);
    }
    return;
  }
  if (g_engine == nullptr || drop < -1) {
    return;
  }
  if (slot >= 0) {
    g_engine->SetVstGroup(slot, drop);
    SaveVstGroups();
    wcsncpy(g_vstStatus, drop < 0 ? L"Plug-in removed from its group." : L"Plug-in added to the group.", 159);
    g_vstStatus[159] = 0;
    return;
  }
  if (file < 0 || file >= g_vstFileCount) {
    return;
  }
  std::wstring error;
  const int added = g_engine->AddVst(g_vstStrip, g_vstFiles[file].path, error);
  if (added < 0) {
    wcsncpy(g_vstStatus, error.empty() ? L"The plug-in did not load." : error.c_str(), 159);
  } else {
    if (drop >= 0) {
      g_engine->SetVstGroup(added, drop);
    }
    wcsncpy(g_vstStatus, drop < 0 ? L"Plug-in loaded on the selected strip." : L"Plug-in loaded into the group.", 159);
    SaveVstRack();
  }
  g_vstStatus[159] = 0;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCALCSIZE: {
      RECT* area = wparam != 0 ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                               : reinterpret_cast<RECT*>(lparam);
      if (area != nullptr && IsZoomed(window)) {
        const int x = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        const int y = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        area->left += x;
        area->right -= x;
        area->top += y;
        area->bottom -= y;
      }
      return 0;
    }
    case WM_NCPAINT:
    case 0x00AE:
    case 0x00AF:
      return 0;
    case WM_NCACTIVATE:
      ApplyWindowFrame(window);
      return DefWindowProcW(window, message, wparam, static_cast<LPARAM>(-1));
    case WM_ACTIVATE:
    case 0x031E:
      ApplyWindowFrame(window);
      return DefWindowProcW(window, message, wparam, lparam);
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &pt);
      RECT client{};
      GetClientRect(window, &client);
      const bool zoomed = IsZoomed(window);
      const int border = zoomed ? 0 : 8;
      const int width = client.right;
      const int height = client.bottom;
      RECT miniR{};
      RECT minR{};
      RECT maxR{};
      RECT closeR{};
      CaptionRects(width, miniR, minR, maxR, closeR);
      const bool onCaptionButton = PtInRect(&miniR, pt) || PtInRect(&minR, pt) || PtInRect(&maxR, pt) ||
                                   PtInRect(&closeR, pt);
      int hot = 0;
      LRESULT hit = HTCLIENT;
      if (!zoomed && !onCaptionButton && pt.y >= 0 && pt.y < height && pt.x >= 0 && pt.x < width) {
        if (pt.y < border && pt.x < border) {
          hit = HTTOPLEFT;
        } else if (pt.y < border && pt.x >= width - border) {
          hit = HTTOPRIGHT;
        } else if (pt.y >= height - border && pt.x < border) {
          hit = HTBOTTOMLEFT;
        } else if (pt.y >= height - border && pt.x >= width - border) {
          hit = HTBOTTOMRIGHT;
        } else if (pt.y < border) {
          hit = HTTOP;
        } else if (pt.y >= height - border) {
          hit = HTBOTTOM;
        } else if (pt.x < border) {
          hit = HTLEFT;
        } else if (pt.x >= width - border) {
          hit = HTRIGHT;
        }
      }
      if (hit == HTCLIENT && pt.y >= 0 && pt.y < 48 && pt.x >= 0 && pt.x < width) {
        if (PtInRect(&miniR, pt)) {
          hot = 4;
        } else if (PtInRect(&minR, pt)) {
          hot = 1;
        } else if (PtInRect(&maxR, pt)) {
          hot = 2;
        } else if (PtInRect(&closeR, pt)) {
          hot = 3;
        } else {
          hit = HTCAPTION;
        }
      }
      if (hot != g_captionHot) {
        g_captionHot = hot;
        InvalidateRect(window, nullptr, FALSE);
      }
      return hit;
    }
    case WM_NCMOUSELEAVE:
      if (g_captionHot != 0) {
        g_captionHot = 0;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(window, &ps);
      Paint(window);
      EndPaint(window, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      ApplyWindowFrame(window);
      NoteMainWindow();
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_EXITSIZEMOVE:
      NoteMainWindow();
      return 0;
    case WM_MOUSELEAVE:
      if (g_captionPress == 0 && g_captionHot != 0) {
        g_captionHot = 0;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      if (y < 48) {
        const int hot = CaptionAt(window, x, y);
        if (hot != g_captionHot) {
          g_captionHot = hot;
          InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        TrackMouseEvent(&track);
      } else if (g_captionHot != 0) {
        g_captionHot = 0;
        InvalidateRect(window, nullptr, FALSE);
      }
      const int id = HitAt(x, y);
      bool changed = false;
      if (g_appPending) {
        const int dx = x - g_appDownX;
        const int dy = y - g_appDownY;
        if (!g_appDrag && dx * dx + dy * dy > 36) {
          g_appDrag = true;
        }
        if (g_appDrag) {
          g_appX = x;
          g_appY = y;
          g_dropStrip = StripAt(window, x, y);
          changed = true;
        }
      }
      if (g_drag != 0) {
        ApplyDrag(x, y);
        changed = true;
      }
      if (g_vstPending) {
        const int dx = x - g_vstDownX;
        const int dy = y - g_vstDownY;
        if (!g_vstDrag && dx * dx + dy * dy > 36) {
          g_vstDrag = true;
        }
        if (g_vstDrag) {
          g_vstDragX = x;
          g_vstDragY = y;
          g_vstHoverGroup = g_page == kVst ? VstDropGroup(x, y) : -2;
          changed = true;
        }
      }
      if (id != g_hover) {
        g_hover = id;
        changed = true;
        if (const Hot* hot = HotOf(id)) {
          CopyText(g_hint, 256, hot->tip);
        }
      }
      if (changed) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      SetFocus(window);
      const int caption = CaptionAt(window, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (caption != 0) {
        g_captionPress = caption;
        SetCapture(window);
        return 0;
      }
      const int id = HitAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      const int strip = id >= 1000 ? (id - 1000) / 40 : -1;
      const int part = id >= 1000 ? (id - 1000) % 40 : 0;
      if (id >= 9000 && id < 9000 + anymix::kVirtualStrips * 32) {
        g_drag = id;
        SetCapture(window);
        ApplyDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      } else if (id >= 7000 && id < 7000 + anymix::kBuses) {
        g_drag = id;
        SetCapture(window);
        ApplyDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      } else if (strip >= 0 && strip < anymix::kStrips &&
          (part == kFader || part == kBass || part == kTreble || part == kPan || part == kGateDepth ||
           IsGateSlider(part))) {
        g_drag = id;
        g_focusStrip = strip;
        SetCapture(window);
        ApplyDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      } else if (id >= 6000 && id < 6000 + anymix::kVirtualStrips * 32) {
        const int appStrip = (id - 6000) / 32;
        const int row = (id - 6000) % 32;
        if (appStrip >= 0 && appStrip < anymix::kVirtualStrips && row >= 0 &&
            row < static_cast<int>(g_sessions[static_cast<size_t>(appStrip)].size())) {
          g_appPending = true;
          g_appDrag = false;
          g_appFrom = appStrip;
          g_appRow = row;
          g_appPid = g_sessions[static_cast<size_t>(appStrip)][static_cast<size_t>(row)].sessionId;
          CopyText(g_appName, 128, g_sessions[static_cast<size_t>(appStrip)][static_cast<size_t>(row)].name);
          g_appDownX = GET_X_LPARAM(lparam);
          g_appDownY = GET_Y_LPARAM(lparam);
          g_appX = g_appDownX;
          g_appY = g_appDownY;
          g_dropStrip = -1;
          SetCapture(window);
        }
      } else if ((id >= 8700 && id < 8700 + anymix::kVstSlots) || (id >= 8820 && id < 8820 + g_vstFileCount)) {
        g_vstPending = true;
        g_vstDrag = false;
        g_vstHoverGroup = -2;
        g_vstFromSlot = id >= 8700 && id < 8700 + anymix::kVstSlots ? id - 8700 : -1;
        g_vstFromFile = id >= 8820 && id < 8820 + g_vstFileCount ? id - 8820 : -1;
        g_vstDownX = GET_X_LPARAM(lparam);
        g_vstDownY = GET_Y_LPARAM(lparam);
        g_vstDragX = g_vstDownX;
        g_vstDragY = g_vstDownY;
        g_vstDragName[0] = 0;
        if (g_vstFromSlot >= 0 && g_engine != nullptr) {
          anymix::VstSlotView view{};
          g_engine->VstSlot(g_vstFromSlot, view);
          wcsncpy(g_vstDragName, view.name, 127);
        } else if (g_vstFromFile >= 0) {
          wcsncpy(g_vstDragName, g_vstFiles[g_vstFromFile].name, 127);
        }
        g_vstDragName[127] = 0;
        SetCapture(window);
      } else {
        OnClick(window, id);
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_RBUTTONUP: {
      SetFocus(window);
      const int id = HitAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (id >= 8300 && id < 8300 + anymix::kVirtualStrips) {
        const int strip = id - 8300;
        g_appsOpen = g_appsOpen == strip ? -1 : strip;
        InvalidateRect(window, nullptr, FALSE);
      } else if (id >= 9000 && id < 9000 + anymix::kVirtualStrips * 32) {
        const int strip = (id - 9000) / 32;
        if (strip >= 0 && strip < anymix::kVirtualStrips) {
          g_appsOpen = g_appsOpen == strip ? -1 : strip;
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id >= 6000 && id < 6000 + anymix::kVirtualStrips * 32) {
        const int strip = (id - 6000) / 32;
        if (strip >= 0 && strip < anymix::kVirtualStrips) {
          g_appsOpen = g_appsOpen == strip ? -1 : strip;
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id >= 8760 && id < 8760 + anymix::kVstGroups) {
        if (const Hot* hot = HotOf(id)) {
          BeginGroupRename(window, id - 8760, hot->r);
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id >= 1000) {
        const int strip = (id - 1000) / 40;
        const int part = (id - 1000) % 40;
        if (part == kName) {
          if (const Hot* hot = HotOf(id)) {
            BeginRename(window, strip, hot->r);
          }
        }
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (g_captionPress != 0) {
        const int pressed = g_captionPress;
        g_captionPress = 0;
        ReleaseCapture();
        if (CaptionAt(window, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)) == pressed) {
          RunCaption(window, pressed);
        }
        return 0;
      }
      if (g_appPending) {
        EndAppPointer(window, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g_vstPending) {
        FinishVstDrag(window, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g_drag != 0) {
        if (g_drag >= 1000 && (g_drag - 1000) % 40 == kGateDepth) {
          SaveGateDepth();
        }
        if (g_drag >= 1000 && IsGateSlider((g_drag - 1000) % 40)) {
          SaveNoiseGate();
          SaveRoutes();
        }
        ReleaseCapture();
        g_drag = 0;
      }
      return 0;
    case WM_CAPTURECHANGED:
      g_captionPress = 0;
      if (g_appPending && reinterpret_cast<HWND>(lparam) != window) {
        g_appPending = false;
        g_appDrag = false;
        g_dropStrip = -1;
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g_vstPending && reinterpret_cast<HWND>(lparam) != window) {
        ClearVstDrag();
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDBLCLK: {
      if (g_drag != 0) {
        ReleaseCapture();
        g_drag = 0;
      }
      const int id = HitAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (id >= 7000 && id < 7000 + anymix::kBuses) {
        const int bus = id - 7000;
        g_busDb[bus] = 0.f;
        g_engine->SetBusGainDb(bus, 0.f);
      } else if (id >= 1000) {
        const int strip = (id - 1000) / 40;
        const int part = (id - 1000) % 40;
        if (strip >= 0 && strip < anymix::kStrips && part == kFader) {
          g_strip[strip].gain = 0.f;
          g_engine->SetGainDb(strip, 0.f);
        } else if (part == kBass) {
          g_strip[strip].bass = 0.f;
          g_engine->SetBassDb(strip, 0.f);
        } else if (part == kTreble) {
          g_strip[strip].treble = 0.f;
          g_engine->SetTrebleDb(strip, 0.f);
        } else if (part == kPan) {
          g_strip[strip].pan = 0.f;
          g_engine->SetPan(strip, 0.f);
        } else if (part == kGateDepth) {
          g_strip[strip].gateDepth = 1.f;
          g_engine->SetGateDepth(strip, 1.f);
          SaveGateDepth();
        } else if (IsGateSlider(part)) {
          const int mic = MicOf(strip);
          if (mic >= 0) {
            ApplyMicGateLevel(mic, 0.f);
            SaveNoiseGate();
            SaveRoutes();
          }
        }
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &pt);
      const int id = HitAt(pt.x, pt.y);
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1;
      if (g_pop != 0) {
        const int maxScroll = std::max(0, PopupCount() - 12);
        g_popScroll = std::max(0, std::min(maxScroll, g_popScroll - delta));
      } else if (g_page == kVst) {
        const int maxScroll = std::max(0, g_vstFileCount - g_vstVisible);
        g_vstScroll = std::max(0, std::min(maxScroll, g_vstScroll - delta));
      } else if (id >= 9000 && id < 9000 + anymix::kVirtualStrips * 32) {
        const int strip = (id - 9000) / 32;
        const int row = (id - 9000) % 32;
        if (strip >= 0 && strip < anymix::kVirtualStrips && row >= 0 &&
            row < static_cast<int>(g_sessions[static_cast<size_t>(strip)].size())) {
          auto& session = g_sessions[static_cast<size_t>(strip)][static_cast<size_t>(row)];
          float level = session.volume + 0.05f * static_cast<float>(delta);
          if (level < 0.f) {
            level = 0.f;
          } else if (level > 1.f) {
            level = 1.f;
          }
          session.volume = level;
          const wchar_t* endpoint = VirtualId(strip);
          if (endpoint != nullptr) {
            std::wstring error;
            anymix::SetSessionVolume(endpoint, session.sessionId, level, error);
          }
        }
      } else if (id >= 7000 && id < 7000 + anymix::kBuses) {
        const int bus = id - 7000;
        g_busDb[bus] = std::max(anymix::kFaderMinDb, std::min(anymix::kFaderMaxDb, g_busDb[bus] + static_cast<float>(delta)));
        g_engine->SetBusGainDb(bus, g_busDb[bus]);
      } else if (id >= 1000) {
        const int strip = (id - 1000) / 40;
        const int part = (id - 1000) % 40;
        if (strip >= 0 && strip < anymix::kStrips && part == kFader) {
          g_strip[strip].gain = std::max(anymix::kFaderMinDb,
                                         std::min(anymix::kFaderMaxDb, g_strip[strip].gain + static_cast<float>(delta)));
          g_engine->SetGainDb(strip, g_strip[strip].gain);
        } else if (part == kBass) {
          g_strip[strip].bass = std::max(-12.f, std::min(12.f, g_strip[strip].bass + 0.5f * static_cast<float>(delta)));
          g_engine->SetBassDb(strip, g_strip[strip].bass);
        } else if (part == kTreble) {
          g_strip[strip].treble =
              std::max(-12.f, std::min(12.f, g_strip[strip].treble + 0.5f * static_cast<float>(delta)));
          g_engine->SetTrebleDb(strip, g_strip[strip].treble);
        } else if (part == kPan) {
          g_strip[strip].pan = std::max(-1.f, std::min(1.f, g_strip[strip].pan + 0.08f * static_cast<float>(delta)));
          g_engine->SetPan(strip, g_strip[strip].pan);
        } else if (part == kGateDepth) {
          g_strip[strip].gateDepth =
              std::max(0.f, std::min(1.f, g_strip[strip].gateDepth + 0.05f * static_cast<float>(delta)));
          g_engine->SetGateDepth(strip, g_strip[strip].gateDepth);
          SaveGateDepth();
        } else if (IsGateSlider(part)) {
          const int mic = MicOf(strip);
          if (mic >= 0) {
            ApplyMicGateLevel(mic, g_micGate[mic].level + 0.5f * static_cast<float>(delta));
            SaveNoiseGate();
            SaveRoutes();
          }
        }
      } else if (g_page == kMixer) {
        g_scroll -= delta * 48;
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_KEYDOWN: {
      const int strip = g_focusStrip;
      if (strip < 0 || strip >= anymix::kStrips) {
        return 0;
      }
      if (wparam == 'M') {
        g_strip[strip].mute = !g_strip[strip].mute;
        g_engine->SetMute(strip, g_strip[strip].mute);
      } else if (wparam == 'S') {
        SetStripSolo(strip, !g_strip[strip].solo);
      } else if (wparam == 'O') {
        g_strip[strip].mono = !g_strip[strip].mono;
        g_engine->SetMono(strip, g_strip[strip].mono);
      } else if (wparam == VK_UP) {
        g_strip[strip].gain = std::min(anymix::kFaderMaxDb, g_strip[strip].gain + 1.f);
        g_engine->SetGainDb(strip, g_strip[strip].gain);
      } else if (wparam == VK_DOWN) {
        g_strip[strip].gain = std::max(anymix::kFaderMinDb, g_strip[strip].gain - 1.f);
        g_engine->SetGainDb(strip, g_strip[strip].gain);
      } else if (wparam >= '1' && wparam <= '5') {
        const int bus = static_cast<int>(wparam - '1') + ((GetKeyState(VK_SHIFT) & 0x8000) ? 5 : 0);
        ToggleRoute(strip, bus);
      } else if (wparam == VK_ESCAPE) {
        g_pop = 0;
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_COMMAND:
      if (HIWORD(wparam) == EN_KILLFOCUS && g_edit != nullptr &&
          reinterpret_cast<HWND>(lparam) == g_edit) {
        CommitRename(window);
      }
      return 0;
    case WM_TIMER:
      if (wparam == 1) {
        TickMeters();
        Autosave();
      } else if (wparam == 2) {
        if (g_pop < 2000 || g_pop >= 2200) {
          ReloadDevices(false);
        }
        PollPlayback();
        if (g_page == kApps || g_page == kMixer) {
          RefreshSessions();
        }
      }
      if (IsWindowVisible(window) && !IsIconic(window)) {
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g_miniWindow != nullptr && IsWindowVisible(g_miniWindow)) {
        InvalidateRect(g_miniWindow, nullptr, FALSE);
      }
      return 0;
    case WM_CLOSE:
      SaveState(true);
      if (g_closeToTray && !g_forceQuit) {
        AddTray(window);
        if (g_trayAdded) {
          ShowWindow(window, SW_HIDE);
          return 0;
        }
      }
      RemoveTray(window);
      DestroyWindow(window);
      return 0;
    case WM_MEASUREITEM: {
      auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
      if (measure->CtlType == ODT_MENU) {
        measure->itemWidth = 168;
        measure->itemHeight = 32;
        return TRUE;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_DRAWITEM: {
      auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (item->CtlType == ODT_MENU) {
        DrawTrayItem(item);
        return TRUE;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_APP + 1:
      if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) {
        ShowFromTray(window);
      } else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_OWNERDRAW, 1, L"Open");
        AppendMenuW(menu, MF_OWNERDRAW, 3, L"Mute mic");
        AppendMenuW(menu, MF_OWNERDRAW, 2, L"Quit");
        const Palette pal = Colors();
        HBRUSH back = CreateSolidBrush(RgbOf(pal.bg));
        MENUINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND | MIM_STYLE;
        info.dwStyle = MNS_NOCHECK;
        info.hbrBack = back;
        SetMenuInfo(menu, &info);
        SetForegroundWindow(window);
        const unsigned cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(menu);
        DeleteObject(back);
        if (cmd == 1) {
          ShowFromTray(window);
        } else if (cmd == 3) {
          ToggleMicMute();
        } else if (cmd == 2) {
          g_forceQuit = true;
          RemoveTray(window);
          DestroyWindow(window);
        }
      }
      return 0;
    case WM_SHOWWINDOW:
      if (wparam != 0 && g_miniMode) {
        g_miniMode = false;
        if (g_miniWindow != nullptr) {
          ShowWindow(g_miniWindow, SW_HIDE);
        }
      }
      if (wparam != 0) {
        RemoveTray(window);
        ApplyWindowFrame(window);
      }
      return DefWindowProcW(window, message, wparam, lparam);
    case WM_QUERYENDSESSION:
      SaveState(true);
      g_forceQuit = true;
      return TRUE;
    case WM_ENDSESSION:
      if (wparam != 0) {
        SaveState(true);
        g_forceQuit = true;
      }
      return 0;
    case WM_DESTROY:
      SaveState(true);
      StopPlayback();
      if (g_encodeThread != nullptr) {
        WaitForSingleObject(g_encodeThread, 60000);
        CloseHandle(g_encodeThread);
        g_encodeThread = nullptr;
      }
      if (g_miniWindow != nullptr) {
        DestroyWindow(g_miniWindow);
      }
      RemoveTray(window);
      KillTimer(window, 1);
      KillTimer(window, 2);
      g_engine->Stop();
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
  const bool startupLaunch = commandLine != nullptr && wcsstr(commandLine, L"--startup") != nullptr;
  SetLastError(0);
  HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\AnyMix.SingleInstance");
  if (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (!startupLaunch) {
      HWND existing = FindWindowW(L"AnyMixWindow", nullptr);
      if (existing != nullptr) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
      }
    }
    CloseHandle(instanceMutex);
    return 0;
  }
  using SetAppId = HRESULT(WINAPI*)(PCWSTR);
  if (HMODULE shell = GetModuleHandleW(L"shell32.dll")) {
    if (auto setId = reinterpret_cast<SetAppId>(GetProcAddress(shell, "SetCurrentProcessExplicitAppUserModelID"))) {
      setId(L"AnyMix.Mixer");
    }
  }
  Gdiplus::GdiplusStartupInput input;
  if (Gdiplus::GdiplusStartup(&g_gdi, &input, nullptr) != Gdiplus::Ok) {
    return 1;
  }
  LoadLogos();
  g_iconBig = IconFromImage(g_logo, 256);
  g_iconSmall = IconFromImage(g_logo, 16);
  anymix::Engine engine;
  g_engine = &engine;
  for (int i = 0; i < anymix::kStrips; ++i) {
    CopyText(g_label[i], 32, anymix::kStripNames[i]);
  }
  for (int i = 0; i < anymix::kVirtualStrips; ++i) {
    g_strip[i].route[0] = true;
  }
  g_strip[anymix::kMic1].route[0] = true;
  LoadRoutes();
  LoadGateDepth();
  LoadRecordFormat();
  LoadCloseToTray();
  LoadStartWithWindows();
  LoadState();
  LoadNoiseGate();
  LoadVstLocal();
  if (g_folder[0] == 0) {
    SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, g_folder);
  }
  ReloadDevices(true);

  WNDCLASSEXW klass{};
  klass.cbSize = sizeof(klass);
  klass.style = CS_DBLCLKS | CS_DROPSHADOW;
  klass.lpfnWndProc = WindowProc;
  klass.hInstance = instance;
  klass.lpszClassName = L"AnyMixWindow";
  klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  klass.hbrBackground = nullptr;
  klass.hIcon = g_iconBig;
  klass.hIconSm = g_iconSmall;
  RegisterClassExW(&klass);
  const int side = 196;
  const int masterW = 100;
  const int gap = 8;
  const int mastersW = anymix::kPhysicalBuses * masterW + (anymix::kPhysicalBuses - 1) * gap;
  const int stripsW = 6 * 158 + 5 * gap;
  RECT frame{0, 0, side + mastersW + stripsW + 24, 760};
  AdjustWindowRect(&frame, kMainStyle, FALSE);
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int winW = frame.right - frame.left;
  int winH = frame.bottom - frame.top;
  const int maxW = work.right - work.left;
  const int maxH = work.bottom - work.top;
  if (winW > maxW) {
    winW = maxW;
  }
  if (winH > maxH) {
    winH = maxH;
  }
  const int winX = work.left + std::max(0, (maxW - winW) / 2);
  const int winY = work.top + std::max(0, (maxH - winH) / 2);
  HWND window = CreateWindowExW(0, L"AnyMixWindow", L"AnyMix", kMainStyle, winX,
                                winY, winW, winH, nullptr, nullptr, instance, nullptr);
  g_mainWindow = window;
  UINT openShow = static_cast<UINT>(show);
  if (g_havePlace) {
    WINDOWPLACEMENT place = g_place;
    place.length = sizeof(place);
    place.showCmd = SW_HIDE;
    SetWindowPlacement(window, &place);
    RECT now{};
    GetWindowRect(window, &now);
    if (MonitorFromRect(&now, MONITOR_DEFAULTTONULL) == nullptr) {
      SetWindowPos(window, nullptr, winX, winY, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
      openShow = SW_SHOWNORMAL;
    } else {
      openShow = g_place.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
  }
  ApplyWindowFrame(window);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_iconBig));
  SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_iconSmall));
  PushDevices();
  for (int strip = 0; strip < anymix::kStrips; ++strip) {
    PushEngineStrip(strip);
  }
  PushEngineBuses();
  g_engine->SetMonitorMask(g_recMask);
  g_engine->SetPlaybackMask(g_playMask);
  std::wstring error;
  g_engineOn = engine.Start(error);
  LoadVstRack();
  SyncWindowsStartup();
  if (!g_engineOn) {
    MessageBoxW(window, error.c_str(), L"AnyMix", MB_OK | MB_ICONERROR);
  }
  timeBeginPeriod(1);
  SetTimer(window, 1, 8, nullptr);
  SetTimer(window, 2, 1000, nullptr);
  RefreshSessions();
  if (g_openMini) {
    EnterMini();
    RECT mini{g_miniX, g_miniY, g_miniX + 320, g_miniY + 348};
    if (g_miniWindow != nullptr && MonitorFromRect(&mini, MONITOR_DEFAULTTONULL) != nullptr) {
      SetWindowPos(g_miniWindow, HWND_TOPMOST, g_miniX, g_miniY, 320, 348, SWP_SHOWWINDOW);
    }
  } else {
    ShowWindow(window, static_cast<int>(openShow));
    ApplyWindowFrame(window);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  delete g_logo;
  DropTarget();
  for (TextFmt& slot : g_fonts) {
    if (slot.fmt != nullptr) {
      slot.fmt->Release();
      slot.fmt = nullptr;
    }
  }
  if (g_write != nullptr) {
    g_write->Release();
    g_write = nullptr;
  }
  if (g_d2d != nullptr) {
    g_d2d->Release();
    g_d2d = nullptr;
  }
  timeEndPeriod(1);
  delete g_logoLongOnDark;
  delete g_logoLong;
  Gdiplus::GdiplusShutdown(g_gdi);
  return 0;
}
