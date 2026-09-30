// wt-rangefinder: manual distance estimator for War Thunder minimaps.
//
// Left click (or drag) on the map places your position, right click places
// the enemy. Distance = pixel distance * (grid cell meters / grid cell pixels).
//
// Copyright (C) 2026 the wt-rangefinder contributors.
// Licensed under the GNU Affero General Public License v3.0 or later.
// See the LICENSE file for the full license text.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "icon_png.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct MapInfo {
    std::string mode;       // game-mode category, used to filter the map list
    std::string name;
    std::string imageRel;   // path as written in maps.cfg
    fs::path imagePath;     // resolved path
    int gridCells = 0;      // grid squares per side
    double gridMeters = 0;  // size of one grid square in meters
    // Optional calibration in image pixels. If pxPerCell <= 0 the grid is
    // assumed to cover the whole image, starting at (0, 0).
    double originX = 0;
    double originY = 0;
    double pxPerCell = 0;
};

struct Texture {
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
};

struct Vec2d {
    double x;
    double y;
};

struct Grid {
    double ox;
    double oy;
    double cell;  // pixels per grid square
};

// In-game distances aren't exact anyway, so round to the nearest 5 m for
// quicker, easier-to-call-out numbers.
static double RoundTo5(double meters)
{
    return std::lround(meters / 5.0) * 5.0;
}

// Snaps x to the closest value in a sorted, deduplicated list.
static int NearestInSorted(const std::vector<int>& sorted, int x)
{
    auto it = std::lower_bound(sorted.begin(), sorted.end(), x);
    if (it == sorted.begin())
        return sorted.front();
    if (it == sorted.end())
        return sorted.back();
    int hi = *it, lo = *(it - 1);
    return (x - lo <= hi - x) ? lo : hi;
}

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string Trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Format: mode | name | image | grid cells | cell meters [| originX | originY | pxPerCell]
static std::vector<MapInfo> LoadMaps(const fs::path& cfgPath, std::string& err)
{
    std::vector<MapInfo> maps;
    std::ifstream in(cfgPath);
    if (!in) {
        err = "Cannot open " + cfgPath.string() + "\n";
        return maps;
    }

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '|'))
            f.push_back(Trim(part));

        if (f.size() != 5 && f.size() != 8) {
            err += "maps.cfg line " + std::to_string(lineNo) + ": expected 5 or 8 fields\n";
            continue;
        }

        try {
            MapInfo m;
            m.mode = f[0];
            m.name = f[1];
            m.imageRel = f[2];
            m.imagePath = cfgPath.parent_path() / f[2];
            m.gridCells = std::stoi(f[3]);
            m.gridMeters = std::stod(f[4]);
            if (f.size() == 8) {
                m.originX = std::stod(f[5]);
                m.originY = std::stod(f[6]);
                m.pxPerCell = std::stod(f[7]);
            }
            if (m.gridCells <= 0 || m.gridCells > 26 || m.gridMeters <= 0)
                throw std::invalid_argument("out of range");
            maps.push_back(m);
        } catch (...) {
            err += "maps.cfg line " + std::to_string(lineNo) + ": invalid number\n";
        }
    }
    return maps;
}

static bool LoadTexture(SDL_Renderer* r, const fs::path& path, Texture& out)
{
    int w, h, n;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!data)
        return false;

    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture* t = s ? SDL_CreateTextureFromSurface(r, s) : nullptr;
    if (s)
        SDL_FreeSurface(s);
    stbi_image_free(data);
    if (!t)
        return false;

    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    out = {t, w, h};
    return true;
}

static Grid GetGrid(const MapInfo& m, const Texture& t)
{
    if (m.pxPerCell > 0)
        return {m.originX, m.originY, m.pxPerCell};
    return {0.0, 0.0, double(t.w) / m.gridCells};
}

// War Thunder style sector name: letter = row (top to bottom), number = column
static std::string SectorName(const Grid& g, int cells, Vec2d p)
{
    int col = int(std::floor((p.x - g.ox) / g.cell));
    int row = int(std::floor((p.y - g.oy) / g.cell));
    if (col < 0 || row < 0 || col >= cells || row >= cells)
        return "outside grid";
    char buf[8];
    std::snprintf(buf, sizeof buf, "%c%d", 'A' + row, col + 1);
    return buf;
}

static void DrawPin(ImDrawList* dl, ImVec2 p, ImU32 color, const char* label)
{
    dl->AddCircleFilled(p, 7.0f, color);
    dl->AddCircle(p, 7.0f, IM_COL32(0, 0, 0, 255), 0, 2.0f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp(p.x - ts.x * 0.5f, p.y - 12.0f - ts.y);
    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(tp.x + ts.x + 3, tp.y + ts.y + 1),
                      IM_COL32(0, 0, 0, 170), 3.0f);
    dl->AddText(tp, color, label);
}

static const char* kAuthorName = "Fede Barrios";
static const char* kAuthorUrl = "https://www.fedebarriosd.com";
static const char* kLicenseUrl = "https://github.com/Fedebarriosd/wt-rangefinder/blob/main/LICENSE";
static const char* kSourceUrl = "https://github.com/Fedebarriosd/wt-rangefinder";

// Text that opens a URL in the default browser when clicked.
static void LinkText(const char* label, const char* url)
{
    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
    ImGui::TextColored(col, "%s", label);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, ImGui::GetColorU32(col));
        ImGui::SetTooltip("%s", url);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            SDL_OpenURL(url);
    }
}

// --- UI themes ---
enum class ThemeId {
    Dark = 0,
    Light,
    Amber,
    Green,
    Gruvbox,
    Blue,
    Purple,
    Red,
    Teal,
    Pink,
    Nord,
    CatppuccinLatte,
    CatppuccinFrappe,
    CatppuccinMacchiato,
    CatppuccinMocha,
    Count
};
struct ThemeGroup {
    const char* title;
    std::vector<ThemeId> themes;  // alphabetical by display name
};
static const std::vector<ThemeGroup> kThemeGroups = {
    {"Default", {ThemeId::Dark, ThemeId::Light}},
    {"Colors",
     {ThemeId::Amber, ThemeId::Blue, ThemeId::Green, ThemeId::Pink, ThemeId::Purple, ThemeId::Red, ThemeId::Teal}},
    {"Palettes",
     {ThemeId::CatppuccinFrappe, ThemeId::CatppuccinLatte, ThemeId::CatppuccinMacchiato, ThemeId::CatppuccinMocha,
      ThemeId::Gruvbox, ThemeId::Nord}},
};
static const char* kThemeNames[(int)ThemeId::Count] = {
    "Dark",   "Light",  "Amber",   "Green",   "Gruvbox", "Blue",      "Purple",
    "Red",    "Teal",   "Pink",    "Nord",    "Catppuccin Latte",     "Catppuccin Frappe",
    "Catppuccin Macchiato", "Catppuccin Mocha"};

static ImVec4 ColorFromHex(unsigned int hex, float alpha = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

// A full dark palette built from shades of a single hue (not just one
// accent color dropped on the default dark theme) - background, borders and
// widget states all move through the same hue, from near-black up to the
// brightest highlight. Used for the Amber/Green presets.
struct HuePalette {
    unsigned int bg0, bg1, bg2, bg3;     // background -> raised surfaces
    unsigned int border;
    unsigned int text, textDisabled;
    unsigned int dim, mid, bright, hot;  // low -> high saturation/brightness
};

static void ApplyHuePalette(const HuePalette& p)
{
    ImGui::StyleColorsDark();
    ImVec4* c = ImGui::GetStyle().Colors;
    auto h = [](unsigned int hex) { return ColorFromHex(hex); };
    c[ImGuiCol_Text] = h(p.text);
    c[ImGuiCol_TextDisabled] = h(p.textDisabled);
    c[ImGuiCol_WindowBg] = h(p.bg0);
    c[ImGuiCol_ChildBg] = h(p.bg0);
    c[ImGuiCol_PopupBg] = h(p.bg1);
    c[ImGuiCol_Border] = h(p.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = h(p.bg1);
    c[ImGuiCol_FrameBgHovered] = h(p.bg2);
    c[ImGuiCol_FrameBgActive] = h(p.bg3);
    c[ImGuiCol_TitleBg] = h(p.bg0);
    c[ImGuiCol_TitleBgActive] = h(p.bg1);
    c[ImGuiCol_TitleBgCollapsed] = h(p.bg0);
    c[ImGuiCol_MenuBarBg] = h(p.bg1);
    c[ImGuiCol_ScrollbarBg] = h(p.bg0);
    c[ImGuiCol_ScrollbarGrab] = h(p.bg3);
    c[ImGuiCol_ScrollbarGrabHovered] = h(p.border);
    c[ImGuiCol_ScrollbarGrabActive] = h(p.dim);
    c[ImGuiCol_CheckMark] = h(p.bright);
    c[ImGuiCol_SliderGrab] = h(p.mid);
    c[ImGuiCol_SliderGrabActive] = h(p.bright);
    c[ImGuiCol_Button] = h(p.bg3);
    c[ImGuiCol_ButtonHovered] = h(p.dim);
    c[ImGuiCol_ButtonActive] = h(p.mid);
    c[ImGuiCol_Header] = h(p.bg3);
    c[ImGuiCol_HeaderHovered] = h(p.dim);
    c[ImGuiCol_HeaderActive] = h(p.mid);
    c[ImGuiCol_Separator] = h(p.border);
    c[ImGuiCol_SeparatorHovered] = h(p.mid);
    c[ImGuiCol_SeparatorActive] = h(p.bright);
    c[ImGuiCol_ResizeGrip] = h(p.bg3);
    c[ImGuiCol_ResizeGripHovered] = h(p.dim);
    c[ImGuiCol_ResizeGripActive] = h(p.mid);
    c[ImGuiCol_Tab] = h(p.bg1);
    c[ImGuiCol_TabHovered] = h(p.dim);
    c[ImGuiCol_TabActive] = h(p.bg3);
    c[ImGuiCol_TabUnfocused] = h(p.bg0);
    c[ImGuiCol_TabUnfocusedActive] = h(p.bg1);
    c[ImGuiCol_PlotLines] = h(p.bright);
    c[ImGuiCol_PlotLinesHovered] = h(p.hot);
    c[ImGuiCol_PlotHistogram] = h(p.mid);
    c[ImGuiCol_PlotHistogramHovered] = h(p.bright);
    c[ImGuiCol_TextSelectedBg] = ColorFromHex(p.mid, 0.35f);
}

static void ApplyGruvboxTheme()
{
    ImGui::StyleColorsDark();
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = ColorFromHex(0xebdbb2);
    c[ImGuiCol_TextDisabled] = ColorFromHex(0x928374);
    c[ImGuiCol_WindowBg] = ColorFromHex(0x282828);
    c[ImGuiCol_ChildBg] = ColorFromHex(0x282828);
    c[ImGuiCol_PopupBg] = ColorFromHex(0x3c3836);
    c[ImGuiCol_Border] = ColorFromHex(0x504945);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ColorFromHex(0x3c3836);
    c[ImGuiCol_FrameBgHovered] = ColorFromHex(0x504945);
    c[ImGuiCol_FrameBgActive] = ColorFromHex(0x665c54);
    c[ImGuiCol_TitleBg] = ColorFromHex(0x1d2021);
    c[ImGuiCol_TitleBgActive] = ColorFromHex(0x3c3836);
    c[ImGuiCol_TitleBgCollapsed] = ColorFromHex(0x1d2021);
    c[ImGuiCol_MenuBarBg] = ColorFromHex(0x3c3836);
    c[ImGuiCol_ScrollbarBg] = ColorFromHex(0x1d2021);
    c[ImGuiCol_ScrollbarGrab] = ColorFromHex(0x504945);
    c[ImGuiCol_ScrollbarGrabHovered] = ColorFromHex(0x665c54);
    c[ImGuiCol_ScrollbarGrabActive] = ColorFromHex(0x7c6f64);
    c[ImGuiCol_CheckMark] = ColorFromHex(0xb8bb26);
    c[ImGuiCol_SliderGrab] = ColorFromHex(0xd79921);
    c[ImGuiCol_SliderGrabActive] = ColorFromHex(0xfabd2f);
    c[ImGuiCol_Button] = ColorFromHex(0x504945);
    c[ImGuiCol_ButtonHovered] = ColorFromHex(0x665c54);
    c[ImGuiCol_ButtonActive] = ColorFromHex(0x7c6f64);
    c[ImGuiCol_Header] = ColorFromHex(0x504945);
    c[ImGuiCol_HeaderHovered] = ColorFromHex(0x665c54);
    c[ImGuiCol_HeaderActive] = ColorFromHex(0x7c6f64);
    c[ImGuiCol_Separator] = ColorFromHex(0x504945);
    c[ImGuiCol_SeparatorHovered] = ColorFromHex(0xd79921);
    c[ImGuiCol_SeparatorActive] = ColorFromHex(0xfabd2f);
    c[ImGuiCol_ResizeGrip] = ColorFromHex(0x504945);
    c[ImGuiCol_ResizeGripHovered] = ColorFromHex(0x689d6a);
    c[ImGuiCol_ResizeGripActive] = ColorFromHex(0x8ec07c);
    c[ImGuiCol_Tab] = ColorFromHex(0x3c3836);
    c[ImGuiCol_TabHovered] = ColorFromHex(0x665c54);
    c[ImGuiCol_TabActive] = ColorFromHex(0x504945);
    c[ImGuiCol_TabUnfocused] = ColorFromHex(0x282828);
    c[ImGuiCol_TabUnfocusedActive] = ColorFromHex(0x3c3836);
    c[ImGuiCol_PlotLines] = ColorFromHex(0x83a598);
    c[ImGuiCol_PlotLinesHovered] = ColorFromHex(0xfe8019);
    c[ImGuiCol_PlotHistogram] = ColorFromHex(0xd79921);
    c[ImGuiCol_PlotHistogramHovered] = ColorFromHex(0xfabd2f);
    c[ImGuiCol_TextSelectedBg] = ColorFromHex(0xd79921, 0.35f);
}

// Classic Nord (arctic, north-bluish) palette: https://www.nordtheme.com
static void ApplyNordTheme()
{
    ImGui::StyleColorsDark();
    ImVec4* c = ImGui::GetStyle().Colors;
    const unsigned int nord0 = 0x2e3440, nord1 = 0x3b4252, nord2 = 0x434c5e, nord3 = 0x4c566a;
    const unsigned int nord6 = 0xeceff4;
    const unsigned int nord7 = 0x8fbcbb, nord8 = 0x88c0d0, nord9 = 0x81a1c1;
    const unsigned int nord12 = 0xd08770, nord13 = 0xebcb8b;
    c[ImGuiCol_Text] = ColorFromHex(nord6);
    c[ImGuiCol_TextDisabled] = ColorFromHex(nord3);
    c[ImGuiCol_WindowBg] = ColorFromHex(nord0);
    c[ImGuiCol_ChildBg] = ColorFromHex(nord0);
    c[ImGuiCol_PopupBg] = ColorFromHex(nord1);
    c[ImGuiCol_Border] = ColorFromHex(nord2);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ColorFromHex(nord1);
    c[ImGuiCol_FrameBgHovered] = ColorFromHex(nord2);
    c[ImGuiCol_FrameBgActive] = ColorFromHex(nord3);
    c[ImGuiCol_TitleBg] = ColorFromHex(nord0);
    c[ImGuiCol_TitleBgActive] = ColorFromHex(nord1);
    c[ImGuiCol_TitleBgCollapsed] = ColorFromHex(nord0);
    c[ImGuiCol_MenuBarBg] = ColorFromHex(nord1);
    c[ImGuiCol_ScrollbarBg] = ColorFromHex(nord0);
    c[ImGuiCol_ScrollbarGrab] = ColorFromHex(nord2);
    c[ImGuiCol_ScrollbarGrabHovered] = ColorFromHex(nord3);
    c[ImGuiCol_ScrollbarGrabActive] = ColorFromHex(nord9);
    c[ImGuiCol_CheckMark] = ColorFromHex(nord8);
    c[ImGuiCol_SliderGrab] = ColorFromHex(nord9);
    c[ImGuiCol_SliderGrabActive] = ColorFromHex(nord8);
    c[ImGuiCol_Button] = ColorFromHex(nord2);
    c[ImGuiCol_ButtonHovered] = ColorFromHex(nord3);
    c[ImGuiCol_ButtonActive] = ColorFromHex(nord9);
    c[ImGuiCol_Header] = ColorFromHex(nord2);
    c[ImGuiCol_HeaderHovered] = ColorFromHex(nord3);
    c[ImGuiCol_HeaderActive] = ColorFromHex(nord9);
    c[ImGuiCol_Separator] = ColorFromHex(nord2);
    c[ImGuiCol_SeparatorHovered] = ColorFromHex(nord9);
    c[ImGuiCol_SeparatorActive] = ColorFromHex(nord8);
    c[ImGuiCol_ResizeGrip] = ColorFromHex(nord2);
    c[ImGuiCol_ResizeGripHovered] = ColorFromHex(nord9);
    c[ImGuiCol_ResizeGripActive] = ColorFromHex(nord8);
    c[ImGuiCol_Tab] = ColorFromHex(nord1);
    c[ImGuiCol_TabHovered] = ColorFromHex(nord3);
    c[ImGuiCol_TabActive] = ColorFromHex(nord2);
    c[ImGuiCol_TabUnfocused] = ColorFromHex(nord0);
    c[ImGuiCol_TabUnfocusedActive] = ColorFromHex(nord1);
    c[ImGuiCol_PlotLines] = ColorFromHex(nord8);
    c[ImGuiCol_PlotLinesHovered] = ColorFromHex(nord7);
    c[ImGuiCol_PlotHistogram] = ColorFromHex(nord12);
    c[ImGuiCol_PlotHistogramHovered] = ColorFromHex(nord13);
    c[ImGuiCol_TextSelectedBg] = ColorFromHex(nord9, 0.35f);
}

// The four official Catppuccin flavors: https://catppuccin.com
struct CatppuccinPalette {
    unsigned int base, mantle, crust;
    unsigned int surface0, surface1, surface2;
    unsigned int overlay0, overlay1;
    unsigned int text;
    unsigned int blue, mauve, green, peach, yellow, teal;
};

static void ApplyCatppuccinPalette(const CatppuccinPalette& p)
{
    ImGui::StyleColorsDark();
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = ColorFromHex(p.text);
    c[ImGuiCol_TextDisabled] = ColorFromHex(p.overlay0);
    c[ImGuiCol_WindowBg] = ColorFromHex(p.base);
    c[ImGuiCol_ChildBg] = ColorFromHex(p.base);
    c[ImGuiCol_PopupBg] = ColorFromHex(p.mantle);
    c[ImGuiCol_Border] = ColorFromHex(p.surface1);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ColorFromHex(p.surface0);
    c[ImGuiCol_FrameBgHovered] = ColorFromHex(p.surface1);
    c[ImGuiCol_FrameBgActive] = ColorFromHex(p.surface2);
    c[ImGuiCol_TitleBg] = ColorFromHex(p.crust);
    c[ImGuiCol_TitleBgActive] = ColorFromHex(p.mantle);
    c[ImGuiCol_TitleBgCollapsed] = ColorFromHex(p.crust);
    c[ImGuiCol_MenuBarBg] = ColorFromHex(p.mantle);
    c[ImGuiCol_ScrollbarBg] = ColorFromHex(p.crust);
    c[ImGuiCol_ScrollbarGrab] = ColorFromHex(p.surface2);
    c[ImGuiCol_ScrollbarGrabHovered] = ColorFromHex(p.overlay0);
    c[ImGuiCol_ScrollbarGrabActive] = ColorFromHex(p.overlay1);
    c[ImGuiCol_CheckMark] = ColorFromHex(p.green);
    c[ImGuiCol_SliderGrab] = ColorFromHex(p.blue);
    c[ImGuiCol_SliderGrabActive] = ColorFromHex(p.mauve);
    c[ImGuiCol_Button] = ColorFromHex(p.surface2);
    c[ImGuiCol_ButtonHovered] = ColorFromHex(p.overlay0);
    c[ImGuiCol_ButtonActive] = ColorFromHex(p.blue);
    c[ImGuiCol_Header] = ColorFromHex(p.surface2);
    c[ImGuiCol_HeaderHovered] = ColorFromHex(p.overlay0);
    c[ImGuiCol_HeaderActive] = ColorFromHex(p.blue);
    c[ImGuiCol_Separator] = ColorFromHex(p.surface1);
    c[ImGuiCol_SeparatorHovered] = ColorFromHex(p.blue);
    c[ImGuiCol_SeparatorActive] = ColorFromHex(p.mauve);
    c[ImGuiCol_ResizeGrip] = ColorFromHex(p.surface2);
    c[ImGuiCol_ResizeGripHovered] = ColorFromHex(p.overlay0);
    c[ImGuiCol_ResizeGripActive] = ColorFromHex(p.blue);
    c[ImGuiCol_Tab] = ColorFromHex(p.mantle);
    c[ImGuiCol_TabHovered] = ColorFromHex(p.overlay0);
    c[ImGuiCol_TabActive] = ColorFromHex(p.surface2);
    c[ImGuiCol_TabUnfocused] = ColorFromHex(p.base);
    c[ImGuiCol_TabUnfocusedActive] = ColorFromHex(p.mantle);
    c[ImGuiCol_PlotLines] = ColorFromHex(p.blue);
    c[ImGuiCol_PlotLinesHovered] = ColorFromHex(p.teal);
    c[ImGuiCol_PlotHistogram] = ColorFromHex(p.peach);
    c[ImGuiCol_PlotHistogramHovered] = ColorFromHex(p.yellow);
    c[ImGuiCol_TextSelectedBg] = ColorFromHex(p.blue, 0.35f);
}

static void ApplyTheme(ThemeId id)
{
    switch (id) {
    case ThemeId::Light:
        ImGui::StyleColorsLight();
        break;
    case ThemeId::Amber:
        ApplyHuePalette({0x1c1410, 0x2b2018, 0x3d2e1f, 0x4f3c28, 0x5c4630, 0xf0dfc0, 0x8a7355, 0x8a5a20, 0xd68910,
                         0xf5a623, 0xffb84d});
        break;
    case ThemeId::Green:
        ApplyHuePalette({0x10180f, 0x1d2b1a, 0x2a3f25, 0x375330, 0x39512f, 0xdcecd7, 0x7a9678, 0x4f7a41, 0x5a9a3d,
                         0x7cc954, 0x9de571});
        break;
    case ThemeId::Gruvbox:
        ApplyGruvboxTheme();
        break;
    case ThemeId::Blue:
        ApplyHuePalette({0x0d1a26, 0x14283a, 0x1c3a52, 0x254d6b, 0x2c5a80, 0xd7ecf5, 0x6f93a8, 0x2c6f99, 0x3a8fc7,
                         0x5bb3ea, 0x8ed0f7});
        break;
    case ThemeId::Purple:
        ApplyHuePalette({0x160f1f, 0x241a33, 0x352548, 0x47325e, 0x543a6e, 0xe8dcf5, 0x8c7aa3, 0x6b3f96, 0x8b4fc2,
                         0xab6fe0, 0xc99bf0});
        break;
    case ThemeId::Red:
        ApplyHuePalette({0x1f0f0f, 0x331a1a, 0x4a2323, 0x612e2e, 0x703636, 0xf5dcdc, 0xa37a7a, 0x963232, 0xc23e3e,
                         0xe0605f, 0xf28f8e});
        break;
    case ThemeId::Teal:
        ApplyHuePalette({0x0a1f1c, 0x123330, 0x1a4a44, 0x226158, 0x287168, 0xd6f5ef, 0x74a89e, 0x1f8577, 0x2aab97,
                         0x4ecdb6, 0x86e3d0});
        break;
    case ThemeId::Pink:
        ApplyHuePalette({0x1f0f18, 0x331a29, 0x4a2339, 0x612e49, 0x703656, 0xf5dcec, 0xa37a94, 0x96326d, 0xc23e8c,
                         0xe0609f, 0xf28fc0});
        break;
    case ThemeId::Nord:
        ApplyNordTheme();
        break;
    case ThemeId::CatppuccinLatte:
        ApplyCatppuccinPalette({0xeff1f5, 0xe6e9ef, 0xdce0e8, 0xccd0da, 0xbcc0cc, 0xacb0be, 0x9ca0b0, 0x8c8fa1,
                                0x4c4f69, 0x1e66f5, 0x8839ef, 0x40a02b, 0xfe640b, 0xdf8e1d, 0x179299});
        break;
    case ThemeId::CatppuccinFrappe:
        ApplyCatppuccinPalette({0x303446, 0x292c3c, 0x232634, 0x414559, 0x51576d, 0x626880, 0x737994, 0x838ba7,
                                0xc6d0f5, 0x8caaee, 0xca9ee6, 0xa6d189, 0xef9f76, 0xe5c890, 0x81c8be});
        break;
    case ThemeId::CatppuccinMacchiato:
        ApplyCatppuccinPalette({0x24273a, 0x1e2030, 0x181926, 0x363a4f, 0x494d64, 0x5b6078, 0x6e738d, 0x8087a2,
                                0xcad3f5, 0x8aadf4, 0xc6a0f6, 0xa6da95, 0xf5a97f, 0xeed49f, 0x8bd5ca});
        break;
    case ThemeId::CatppuccinMocha:
        ApplyCatppuccinPalette({0x1e1e2e, 0x181825, 0x11111b, 0x313244, 0x45475a, 0x585b70, 0x6c7086, 0x7f849c,
                                0xcdd6f4, 0x89b4fa, 0xcba6f7, 0xa6e3a1, 0xfab387, 0xf9e2af, 0x94e2d5});
        break;
    case ThemeId::Dark:
    default:
        ImGui::StyleColorsDark();
        break;
    }
}

int main(int, char**)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // maps.cfg (and the persisted window size) live next to the executable
    fs::path base;
    if (char* b = SDL_GetBasePath()) {
        base = b;
        SDL_free(b);
    }
    const fs::path windowCfgPath = base / "window.cfg";

    int initialW = 1280, initialH = 860;
    int initialTheme = 0;
    {
        std::ifstream wf(windowCfgPath);
        int w, h, th;
        if (wf >> w >> h && w >= 200 && h >= 200) {
            initialW = w;
            initialH = h;
        }
        if (wf >> th && th >= 0 && th < (int)ThemeId::Count)
            initialTheme = th;
    }

    SDL_Window* window = SDL_CreateWindow("WT Rangefinder", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          initialW, initialH, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !renderer) {
        std::fprintf(stderr, "Window/renderer creation failed: %s\n", SDL_GetError());
        return 1;
    }

    {
        int iw, ih, ic;
        unsigned char* px = stbi_load_from_memory(kIconPng, int(sizeof kIconPng), &iw, &ih, &ic, 4);
        if (px) {
            SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(px, iw, ih, 32, iw * 4, SDL_PIXELFORMAT_RGBA32);
            if (icon) {
                SDL_SetWindowIcon(window, icon);
                SDL_FreeSurface(icon);
            }
            stbi_image_free(px);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ThemeId theme = (ThemeId)initialTheme;
    ApplyTheme(theme);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    std::string loadErr;
    std::vector<MapInfo> maps = LoadMaps(base / "maps.cfg", loadErr);

    // Distinct grid-square sizes that actually occur across maps.cfg. The
    // range-ring slider snaps to these instead of an arbitrary linear scale,
    // since only these values ever correspond to a real in-game grid.
    // A couple of degenerate single-cell manifest entries (gridCells == 1)
    // report a "grid size" that's really just the whole map, which would
    // blow out the slider range. Ignore anything past a sane cap.
    const int kRingOptionCap = 550;
    std::vector<int> ringOptions;
    for (auto& m : maps) {
        int v = int(std::lround(m.gridMeters));
        if (v <= kRingOptionCap)
            ringOptions.push_back(v);
    }
    std::sort(ringOptions.begin(), ringOptions.end());
    ringOptions.erase(std::unique(ringOptions.begin(), ringOptions.end()), ringOptions.end());
    if (ringOptions.empty())
        ringOptions.push_back(200);

    // State
    int current = -1;
    Texture tex;
    std::optional<Vec2d> me, enemy, calA;
    enum class Mode { Normal, CalibFirst, CalibSecond } mode = Mode::Normal;
    bool showGrid = false;
    bool showRings = true;
    bool onTop = false;
    bool inGameMode = false;
    int ringStep = ringOptions.front();
    std::string cfgLine;
    char mapSearch[128] = "";
    const float panelW = 300.0f;
    int lastWinW = 0, lastWinH = 0;
    SDL_GetWindowSize(window, &lastWinW, &lastWinH);
    // Size and position of the window the last time it was in normal
    // (non-in-game) mode, so F1 can restore them instead of leaving the
    // window at the in-game size/spot. Position is only remembered for this
    // run (not persisted to window.cfg) since in-game mode always overrides it.
    int savedNormalW = lastWinW, savedNormalH = lastWinH;
    int savedNormalX = SDL_WINDOWPOS_CENTERED, savedNormalY = SDL_WINDOWPOS_CENTERED;
    SDL_GetWindowPosition(window, &savedNormalX, &savedNormalY);

    // Keeps the window's aspect ratio matched to (map image + side panel) so
    // the map area never shows letterboxed black bars. Adjusts height to fit
    // the current width, since width is what stays stable across map/panel
    // changes (map switch, entering/exiting in-game mode).
    auto snapToAspect = [&]() {
        if (!tex.tex)
            return;
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        float pw = inGameMode ? 0.0f : panelW;
        float mapAvailW = std::max(1.0f, float(w) - pw);
        int targetH = std::max(1, int(std::lround(mapAvailW * (double(tex.h) / tex.w))));
        if (targetH != h)
            SDL_SetWindowSize(window, w, targetH);
        SDL_GetWindowSize(window, &lastWinW, &lastWinH);
    };

    auto selectMap = [&](int i) {
        if (tex.tex)
            SDL_DestroyTexture(tex.tex);
        tex = {};
        me.reset();
        enemy.reset();
        calA.reset();
        mode = Mode::Normal;
        cfgLine.clear();
        mapSearch[0] = '\0';
        current = i;
        if (!LoadTexture(renderer, maps[i].imagePath, tex))
            loadErr += "Cannot load image " + maps[i].imagePath.string() + "\n";
        ringStep = NearestInSorted(ringOptions, int(std::lround(maps[i].gridMeters)));
        snapToAspect();
    };

    // In-game mode hides everything but the map so the window can be laid
    // always-on-top over the game. Turning it on also enables "always on
    // top" and snaps the window to the size and screen position of War
    // Thunder's own minimap at 1920x1080 (measured from a reference
    // screenshot), so it drops in already lined up with the real thing
    // after alt-tabbing back.
    const int kInGameW = 325;
    const int kInGameH = 330;
    const int kInGameX = 1581;
    const int kInGameY = 738;
    const float kInGameUnfocusedOpacity = 0.35f;
    auto setInGameMode = [&](bool v) {
        if (v == inGameMode)
            return;
        inGameMode = v;
        SDL_SetWindowTitle(window, v ? "WT Rangefinder - F1 to exit" : "WT Rangefinder");
        if (v) {
            SDL_GetWindowSize(window, &savedNormalW, &savedNormalH);
            SDL_GetWindowPosition(window, &savedNormalX, &savedNormalY);
            if (!onTop) {
                onTop = true;
                SDL_SetWindowAlwaysOnTop(window, SDL_TRUE);
            }
            SDL_SetWindowSize(window, kInGameW, kInGameH);
            SDL_SetWindowPosition(window, kInGameX, kInGameY);
            bool focused = SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
            SDL_SetWindowOpacity(window, focused ? 1.0f : kInGameUnfocusedOpacity);
        } else {
            SDL_SetWindowOpacity(window, 1.0f);
            SDL_SetWindowSize(window, savedNormalW, savedNormalH);
            SDL_SetWindowPosition(window, savedNormalX, savedNormalY);
        }
        snapToAspect();
        SDL_GetWindowSize(window, &lastWinW, &lastWinH);
    };

    std::vector<std::string> modeList = {"All"};
    {
        std::vector<std::string> uniq;
        for (auto& m : maps)
            uniq.push_back(m.mode);
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        for (auto& mo : uniq)
            modeList.push_back(mo);
    }
    int modeFilterIdx = 0;

    if (!maps.empty()) {
        int initial = 0;
        for (int i = 0; i < int(maps.size()); ++i) {
            if (ToLower(maps[i].mode) != "test") {
                initial = i;
                break;
            }
        }
        selectMap(initial);
    }

    const ImU32 colMe = IM_COL32(80, 170, 255, 255);
    const ImU32 colEnemy = IM_COL32(255, 80, 80, 255);

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT)
                running = false;
            // Keep the window locked to the (map + panel) aspect ratio while
            // resizing, in every mode, so the map area never letterboxes.
            // Whichever dimension moved more is treated as the one the user
            // actually dragged; the other is recomputed to match it exactly.
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_RESIZED && tex.tex) {
                int newW = e.window.data1;
                int newH = e.window.data2;
                float pw = inGameMode ? 0.0f : panelW;
                double aspect = double(tex.w) / tex.h;
                int targetW = newW, targetH = newH;
                if (std::abs(newW - lastWinW) >= std::abs(newH - lastWinH)) {
                    float mapAvailW = std::max(1.0f, float(newW) - pw);
                    targetH = std::max(1, int(std::lround(mapAvailW / aspect)));
                } else {
                    double mapAvailW = std::max(1.0, float(newH) * aspect);
                    targetW = std::max(1, int(std::lround(mapAvailW + pw)));
                }
                if (targetW != newW || targetH != newH)
                    SDL_SetWindowSize(window, targetW, targetH);
                lastWinW = targetW;
                lastWinH = targetH;
                if (!inGameMode) {
                    savedNormalW = targetW;
                    savedNormalH = targetH;
                }
            }
            // In-game mode: fade out when the game (or anything else) has
            // focus, so the real minimap shows through underneath; snap
            // back to fully opaque as soon as you click the tool to use it.
            if (e.type == SDL_WINDOWEVENT && inGameMode) {
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    SDL_SetWindowOpacity(window, 1.0f);
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    SDL_SetWindowOpacity(window, kInGameUnfocusedOpacity);
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImVec2 disp = io.DisplaySize;
        const bool haveMap = tex.tex != nullptr;
        const MapInfo* map = haveMap ? &maps[current] : nullptr;

        // Fit the map into the area right of the panel, keeping aspect ratio
        const float activePanelW = inGameMode ? 0.0f : panelW;
        ImVec2 mapMin(0, 0), mapMax(0, 0);
        float scale = 1.0f;
        Grid g{0, 0, 1};
        double mPerPx = 0;
        if (haveMap) {
            float availW = std::max(1.0f, disp.x - activePanelW);
            float availH = std::max(1.0f, disp.y);
            scale = std::min(availW / tex.w, availH / tex.h);
            float dw = tex.w * scale, dh = tex.h * scale;
            mapMin = ImVec2(activePanelW + (availW - dw) * 0.5f, (availH - dh) * 0.5f);
            mapMax = ImVec2(mapMin.x + dw, mapMin.y + dh);
            g = GetGrid(*map, tex);
            mPerPx = map->gridMeters / g.cell;
        }
        auto toScreen = [&](Vec2d p) {
            return ImVec2(mapMin.x + float(p.x) * scale, mapMin.y + float(p.y) * scale);
        };
        auto toImage = [&](ImVec2 s) {
            return Vec2d{(s.x - mapMin.x) / scale, (s.y - mapMin.y) / scale};
        };

        // --- Mouse input on the map ---
        if (haveMap && !io.WantCaptureMouse) {
            ImVec2 mp = io.MousePos;
            bool inside = mp.x >= mapMin.x && mp.x <= mapMax.x && mp.y >= mapMin.y && mp.y <= mapMax.y;
            if (inside) {
                Vec2d p = toImage(mp);
                if (mode == Mode::Normal) {
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        me = p;
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
                        enemy = p;
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (mode == Mode::CalibFirst) {
                        calA = p;
                        mode = Mode::CalibSecond;
                    } else {
                        MapInfo& m = maps[current];
                        double cw = (p.x - calA->x) / m.gridCells;
                        double ch = (p.y - calA->y) / m.gridCells;
                        if (cw > 1.0 && ch > 1.0) {
                            m.originX = calA->x;
                            m.originY = calA->y;
                            m.pxPerCell = (cw + ch) * 0.5;
                            char buf[512];
                            std::snprintf(buf, sizeof buf, "%s | %s | %d | %g | %.1f | %.1f | %.3f",
                                          m.name.c_str(), m.imageRel.c_str(), m.gridCells, m.gridMeters,
                                          m.originX, m.originY, m.pxPerCell);
                            cfgLine = buf;
                        }
                        mode = Mode::Normal;
                        calA.reset();
                    }
                }
            }
        }

        // --- Keyboard shortcuts ---
        if (!io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_C)) {
                me.reset();
                enemy.reset();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                mode = Mode::Normal;
                calA.reset();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F1))
                setInGameMode(!inGameMode);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q))
                running = false;

            // hjkl/arrows nudge the active pin: Shift targets the enemy pin,
            // otherwise it's "me". hjkl moves half a grid square (fast
            // travel across the map), arrows move a single pixel (precise).
            if (haveMap && mode == Mode::Normal) {
                std::optional<Vec2d>& target = io.KeyShift ? enemy : me;
                auto nudge = [&](double dx, double dy) {
                    if (!target)
                        target = Vec2d{tex.w / 2.0, tex.h / 2.0};
                    target->x = std::clamp(target->x + dx, 0.0, double(tex.w));
                    target->y = std::clamp(target->y + dy, 0.0, double(tex.h));
                };
                double coarse = g.cell * 0.5;
                if (ImGui::IsKeyPressed(ImGuiKey_H))
                    nudge(-coarse, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_L))
                    nudge(coarse, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_K))
                    nudge(0, -coarse);
                if (ImGui::IsKeyPressed(ImGuiKey_J))
                    nudge(0, coarse);
                const double fine = 2.0;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                    nudge(-fine, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                    nudge(fine, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    nudge(0, -fine);
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    nudge(0, fine);
            }
        }

        std::optional<double> dist;
        if (haveMap && me && enemy)
            dist = RoundTo5(std::hypot(enemy->x - me->x, enemy->y - me->y) * mPerPx);

        // --- Map drawing ---
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (haveMap) {
            dl->AddImage((ImTextureID)(intptr_t)tex.tex, mapMin, mapMax);

            if (showGrid) {
                const ImU32 gc = IM_COL32(255, 255, 0, 110);
                const int n = map->gridCells;
                for (int i = 0; i <= n; ++i) {
                    double off = i * g.cell;
                    dl->AddLine(toScreen({g.ox + off, g.oy}), toScreen({g.ox + off, g.oy + n * g.cell}), gc);
                    dl->AddLine(toScreen({g.ox, g.oy + off}), toScreen({g.ox + n * g.cell, g.oy + off}), gc);
                }
            }

            if (me && showRings && ringStep > 0) {
                double maxR = std::hypot(double(tex.w), double(tex.h)) * mPerPx;
                for (double r = ringStep; r < maxR; r += ringStep)
                    dl->AddCircle(toScreen(*me), float(r / mPerPx) * scale, IM_COL32(80, 170, 255, 70), 128, 1.0f);
            }

            if (me && enemy) {
                ImVec2 a = toScreen(*me), b = toScreen(*enemy);
                dl->AddLine(a, b, IM_COL32(255, 255, 255, 220), 2.0f);
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.0f m", *dist);
                ImVec2 ts = ImGui::CalcTextSize(buf);
                ImVec2 mid((a.x + b.x) * 0.5f - ts.x * 0.5f, (a.y + b.y) * 0.5f - ts.y * 0.5f);
                dl->AddRectFilled(ImVec2(mid.x - 4, mid.y - 2), ImVec2(mid.x + ts.x + 4, mid.y + ts.y + 2),
                                  IM_COL32(0, 0, 0, 200), 3.0f);
                dl->AddText(mid, IM_COL32(255, 255, 255, 255), buf);
            }

            if (me)
                DrawPin(dl, toScreen(*me), colMe, "ME");
            if (enemy)
                DrawPin(dl, toScreen(*enemy), colEnemy, "ENEMY");
            if (calA)
                dl->AddCircle(toScreen(*calA), 6.0f, IM_COL32(255, 255, 0, 255), 0, 2.0f);
        }

        // --- Side panel ---
        if (inGameMode) {
            // Just the map; the way back (F1) is shown in the window title
            // so nothing covers the minimap itself.
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            continue;
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(panelW, disp.y));
        ImGui::Begin("WT Rangefinder", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        if (ImGui::Button("In-game mode (F1)", ImVec2(-1, 0)))
            setInGameMode(true);

        ImGui::SetNextItemWidth(-1);
        int themeIdx = (int)theme;
        if (ImGui::BeginCombo("##theme", kThemeNames[themeIdx])) {
            for (const ThemeGroup& group : kThemeGroups) {
                ImGui::SeparatorText(group.title);
                for (ThemeId t : group.themes)
                    if (ImGui::Selectable(kThemeNames[(int)t], (int)t == themeIdx)) {
                        theme = t;
                        ApplyTheme(theme);
                    }
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        if (maps.empty()) {
            ImGui::TextWrapped("No maps loaded. Add entries to maps.cfg next to the executable.");
        } else {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##mode", modeList[modeFilterIdx].c_str())) {
                for (int i = 0; i < int(modeList.size()); ++i)
                    if (ImGui::Selectable(modeList[i].c_str(), i == modeFilterIdx))
                        modeFilterIdx = i;
                ImGui::EndCombo();
            }

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##map", current >= 0 ? maps[current].name.c_str() : "(none)")) {
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##mapsearch", "Search maps...", mapSearch, sizeof(mapSearch));
                const std::string searchLower = ToLower(mapSearch);
                const std::string& modeFilter = modeList[modeFilterIdx];

                ImGui::BeginChild("##maplist", ImVec2(-1, 220));
                for (int i = 0; i < int(maps.size()); ++i) {
                    bool isTest = ToLower(maps[i].mode) == "test";
                    if (modeFilterIdx == 0 && isTest)
                        continue;  // "All" excludes the Test category
                    if (modeFilterIdx != 0 && maps[i].mode != modeFilter)
                        continue;
                    if (!searchLower.empty() && ToLower(maps[i].name).find(searchLower) == std::string::npos)
                        continue;
                    if (ImGui::Selectable(maps[i].name.c_str(), i == current)) {
                        if (i != current)
                            selectMap(i);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndChild();
                ImGui::EndCombo();
            }
        }

        if (haveMap) {
            ImGui::Text("Grid: %dx%d, %g m per square", map->gridCells, map->gridCells, map->gridMeters);
            ImGui::Separator();

            ImGui::Text("Distance");
            ImGui::SetWindowFontScale(2.2f);
            if (dist)
                ImGui::Text("%.0f m", *dist);
            else
                ImGui::TextDisabled("---");
            ImGui::SetWindowFontScale(1.0f);
            if (dist)
                ImGui::TextDisabled("%.2f grid squares", *dist / map->gridMeters);

            ImGui::Spacing();
            ImGui::TextColored(ImColor(colMe), "Me:    %s", me ? SectorName(g, map->gridCells, *me).c_str() : "-");
            ImGui::TextColored(ImColor(colEnemy), "Enemy: %s",
                               enemy ? SectorName(g, map->gridCells, *enemy).c_str() : "-");

            ImGui::Spacing();
            if (ImGui::Button("Clear pins (C)", ImVec2(-1, 0))) {
                me.reset();
                enemy.reset();
            }

            ImGui::Separator();
            ImGui::Checkbox("Grid overlay", &showGrid);
            ImGui::Checkbox("Range rings", &showRings);
            if (showRings) {
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##ring", &ringStep, ringOptions.front(), ringOptions.back(), "every %d m");
                ringStep = NearestInSorted(ringOptions, ringStep);
            }
            if (ImGui::Checkbox("Always on top", &onTop))
                SDL_SetWindowAlwaysOnTop(window, onTop ? SDL_TRUE : SDL_FALSE);

            ImGui::Separator();
            ImGui::Text("Calibration");
            if (mode == Mode::Normal) {
                if (ImGui::Button("Calibrate grid", ImVec2(-1, 0))) {
                    mode = Mode::CalibFirst;
                    cfgLine.clear();
                }
                ImGui::TextDisabled(map->pxPerCell > 0 ? "Using calibrated grid" : "Grid assumed to fill the image");
                if (map->pxPerCell > 0 && ImGui::Button("Reset to default", ImVec2(-1, 0))) {
                    MapInfo& m = maps[current];
                    m.originX = 0;
                    m.originY = 0;
                    m.pxPerCell = 0;
                    cfgLine.clear();
                }
            } else {
                ImGui::TextWrapped(mode == Mode::CalibFirst ? "Click the TOP-LEFT corner of the grid (A1)."
                                                            : "Click the BOTTOM-RIGHT corner of the grid.");
                ImGui::TextDisabled("Esc to cancel");
            }
            if (!cfgLine.empty()) {
                ImGui::TextWrapped("Paste this into maps.cfg to keep it:");
                ImGui::TextWrapped("%s", cfgLine.c_str());
                if (ImGui::Button("Copy line", ImVec2(-1, 0)))
                    ImGui::SetClipboardText(cfgLine.c_str());
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Left click / drag: your position");
        ImGui::TextDisabled("Right click / drag: enemy");
        ImGui::TextDisabled("hjkl / arrows: nudge (+Shift = enemy)");
        ImGui::TextDisabled("Ctrl+Q: quit");

        if (!loadErr.empty()) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
            ImGui::TextWrapped("%s", loadErr.c_str());
            ImGui::PopStyleColor();
        }

        // Credits + license footer, pinned to the bottom of the panel
        const float footerH = ImGui::GetTextLineHeightWithSpacing() * 3 + ImGui::GetStyle().ItemSpacing.y * 2;
        const float footerY = ImGui::GetWindowHeight() - footerH - ImGui::GetStyle().WindowPadding.y;
        if (ImGui::GetCursorPosY() < footerY)
            ImGui::SetCursorPosY(footerY);
        ImGui::Separator();
        ImGui::TextDisabled("Made by");
        ImGui::SameLine(0, 4);
        LinkText(kAuthorName, kAuthorUrl);
        ImGui::TextDisabled("Free software under the AGPLv3");
        LinkText("License", kLicenseUrl);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        LinkText("Source code", kSourceUrl);
        ImGui::End();

        // --- Render ---
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    {
        std::ofstream wf(windowCfgPath);
        wf << savedNormalW << " " << savedNormalH << " " << (int)theme << "\n";
    }

    if (tex.tex)
        SDL_DestroyTexture(tex.tex);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
