/*
 * Mod: in-game menu (Dear ImGui) and status overlay.
 *
 * Takes the game's own Direct3D 9 device (s2render: tSingleton<cDX9Advance> ->
 * cDX9Base::getDevice/getHwnd), hooks the device's Present/Reset vtable slots in
 * memory and subclasses the game window. Home opens the menu: every module's
 * settings are drawn from its Setting table, changes apply live, "Guardar" writes
 * the ini. While the menu is open the game does not receive window mouse/keyboard
 * messages. A small overlay shows ON/OFF and each module's status.
 */
#include "modkit.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef void *(__cdecl *Fn_InstancePtr)(void);
typedef IDirect3DDevice9 *(__thiscall *Fn_GetDevice)(const void *self);
typedef HWND (__thiscall *Fn_GetHwnd)(const void *self);
typedef HRESULT (__stdcall *Fn_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (__stdcall *Fn_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

static Fn_InstancePtr advInstance;
static Fn_GetDevice getDevice;
static Fn_GetHwnd getHwnd;

static struct {
    int menuKey, overlay, overlayDetail, hideWithF11, corner;
    float scale;
} C = { VK_HOME, 1, 1, 1, 0, 0.0f };

static const Setting settings[] = {
    { "MenuKey", "Tecla del menu", "Abre y cierra este menu. Inicio = 36.", ST_KEY, &C.menuKey, 0, 255 },
    { "Overlay", "Indicador en pantalla", "Muestra si la automatizacion esta activa.", ST_BOOL, &C.overlay, 0, 1 },
    { "OverlayDetail", "Indicador detallado", "Muestra tambien lo que hace cada mod.", ST_BOOL, &C.overlayDetail, 0, 1 },
    { "OverlayCorner", "Esquina del indicador", "0 arriba-izq, 1 arriba-der, 2 abajo-izq, 3 abajo-der.", ST_INT, &C.corner, 0, 3 },
    { "HideWithF11", "Ocultar con F11", "F11 (ocultar interfaz del juego) oculta tambien el indicador.", ST_BOOL, &C.hideWithF11, 0, 1 },
    { "Scale", "Escala (0 = auto)", "Tamano del menu. 0 lo ajusta a la resolucion.", ST_FLOAT, &C.scale, 0.0f, 4.0f },
};

static IDirect3DDevice9 *g_dev;
static HWND g_hwnd;
static WNDPROC g_origWndProc;
static void **g_vtbl;
static Fn_Present g_origPresent;
static Fn_Reset g_origReset;
static int g_hooked, g_imgui, g_failed, g_hiddenF11, g_saveFlash;
static Key g_menuKey, g_f11;
static int *g_capture;  /* setting waiting for a key press */
static char g_status[96];

/* ------------------------------------------------------------------ helpers */
static int in_image(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.Type == MEM_IMAGE;
}

static int patch_slot(void **slot, void *fn, void **orig)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (orig) *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

static float ui_scale(void)
{
    RECT r;
    if (C.scale > 0.05f) return C.scale;
    if (g_hwnd && GetClientRect(g_hwnd, &r) && r.bottom > 0) {
        float s = (float)r.bottom / 1080.0f;
        return s < 1.0f ? 1.0f : s;
    }
    return 1.0f;
}

/* ------------------------------------------------------------------ UI */
static void setting_widget(const Setting *s)
{
    ImGui::PushID(s);
    switch (s->type) {
    case ST_BOOL: {
        bool v = *(int *)s->ptr != 0;
        if (ImGui::Checkbox(s->label, &v)) *(int *)s->ptr = v ? 1 : 0;
        break;
    }
    case ST_INT:
        ImGui::SliderInt(s->label, (int *)s->ptr, (int)s->min, (int)s->max);
        break;
    case ST_FLOAT:
        ImGui::SliderFloat(s->label, (float *)s->ptr, s->min, s->max, "%.2f");
        break;
    case ST_PERCENT:
        ImGui::SliderFloat(s->label, (float *)s->ptr, s->min, s->max, "%.0f %%");
        break;
    case ST_KEY: {
        int *vk = (int *)s->ptr;
        char btn[64];
        if (g_capture == vk) snprintf(btn, sizeof btn, "Pulsa una tecla... (Esc cancela)");
        else snprintf(btn, sizeof btn, "%s", *vk ? key_name(*vk) : "(ninguna)");
        if (ImGui::Button(btn, ImVec2(ImGui::GetFontSize() * 14, 0))) g_capture = vk;
        ImGui::SameLine();
        ImGui::TextUnformatted(s->label);
        break;
    }
    }
    if (s->help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s->help);
    ImGui::PopID();
}

static void capture_key(void)
{
    if (!g_capture) return;
    for (int vk = 8; vk < 255; vk++) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue; /* use the L/R variants */
        if (GetAsyncKeyState(vk) & 1) {
            if (vk != VK_ESCAPE) *g_capture = vk;
            g_capture = NULL;
            return;
        }
    }
}

static void draw_menu(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 40), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Sacred 2 Gold ModKit " MK_VERSION, &open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }

    bool active = S.active != 0;
    if (ImGui::Checkbox("Automatizacion activa", &active)) S.active = active ? 1 : 0;
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", key_name(*(int *)S.modules[0]->settings[1].ptr));
    for (int k = 1; k < S.nModules; k++) {
        Module *m = S.modules[k];
        if (m->disabled) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s: desactivado (faltan funciones del juego)", m->name);
        else if (m->status) ImGui::TextUnformatted(m->status());
    }
    ImGui::Separator();

    for (int k = 0; k < S.nModules; k++) {
        Module *m = S.modules[k];
        if (!m->nSettings) continue;
        if (ImGui::CollapsingHeader(m->name, k == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::PushItemWidth(ImGui::GetFontSize() * 12);
            for (int i = 0; i < m->nSettings; i++) setting_widget(&m->settings[i]);
            ImGui::PopItemWidth();
            if (m->info && !m->disabled) {
                char line[192];
                for (int i = 0; i < 64 && m->info(i, line, sizeof line); i++) ImGui::TextUnformatted(line);
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Guardar")) { mk_save_config(); g_saveFlash = 120; }
    ImGui::SameLine();
    if (ImGui::Button("Recargar del archivo")) mk_reload_config();
    ImGui::SameLine();
    if (ImGui::Button("Cerrar")) open = false;
    if (g_saveFlash > 0) { g_saveFlash--; ImGui::SameLine(); ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Guardado"); }
    ImGui::TextDisabled("Pasa el raton por encima de una opcion para ver que hace.");
    ImGui::End();
    if (!open) S.menuOpen = 0;
}

static void draw_overlay(void)
{
    ImGuiIO &io = ImGui::GetIO();
    const float pad = 12.0f * ui_scale();
    ImVec2 pos(C.corner & 1 ? io.DisplaySize.x - pad : pad, C.corner & 2 ? io.DisplaySize.y - pad : pad * 6);
    ImVec2 pivot(C.corner & 1 ? 1.0f : 0.0f, C.corner & 2 ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.35f);
    if (ImGui::Begin("##mk_overlay", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoInputs)) {
        if (S.active) ImGui::TextColored(ImVec4(0.45f, 1, 0.45f, 1), "ModKit ON");
        else ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "ModKit OFF");
        if (S.active && S.sinceInput < S.inputPause) { ImGui::SameLine(); ImGui::TextDisabled("(pausa)"); }
        if (C.overlayDetail)
            for (int k = 1; k < S.nModules; k++)
                if (!S.modules[k]->disabled && S.modules[k]->status && S.modules[k] != S.modules[0])
                    ImGui::TextDisabled("%s", S.modules[k]->status());
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ module drawing */
static void d_line(float x0, float y0, float x1, float y1, unsigned col, float thick)
{
    ImGui::GetBackgroundDrawList()->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col, thick);
}
static void d_rect(float x0, float y0, float x1, float y1, unsigned col, int filled)
{
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (filled) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
    else dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col);
}
static void d_circle(float x, float y, float r, unsigned col, int filled)
{
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (filled) dl->AddCircleFilled(ImVec2(x, y), r, col);
    else dl->AddCircle(ImVec2(x, y), r, col, 0, 1.5f);
}
static void d_text(float x, float y, unsigned col, const char *s)
{
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(x, y), col, s);
}

/* Lets other modules draw on screen (the radar of the map memory, for example). */
static void draw_modules(void)
{
    DrawApi d;
    ImGuiIO &io = ImGui::GetIO();
    d.w = io.DisplaySize.x; d.h = io.DisplaySize.y; d.scale = ui_scale();
    d.line = d_line; d.rect = d_rect; d.circle = d_circle; d.text = d_text;
    for (int k = 0; k < S.nModules; k++)
        if (!S.modules[k]->disabled && S.modules[k]->overlay) S.modules[k]->overlay(&d);
}

/* ------------------------------------------------------------------ hooks */
static void imgui_init(void)
{
    char ini[MAX_PATH];
    static char iniPath[MAX_PATH];
    float s = ui_scale();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    snprintf(ini, sizeof ini, "%sPlugin_ModKit_imgui.ini", mk_dir());
    memcpy(iniPath, ini, sizeof ini);
    io.IniFilename = iniPath;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImFontConfig fc;
    fc.OversampleH = 2;
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * s, &fc, io.Fonts->GetGlyphRangesDefault());
    else
        io.FontGlobalScale = s;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6; st.FrameRounding = 4; st.GrabRounding = 4;
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.42f, 0.22f, 0.10f, 1);
    st.Colors[ImGuiCol_Header] = ImVec4(0.45f, 0.28f, 0.12f, 0.6f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.34f, 0.15f, 0.8f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.75f, 0.35f, 1);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.55f, 0.25f, 1);
    st.Colors[ImGuiCol_Button] = ImVec4(0.45f, 0.28f, 0.12f, 0.7f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.6f, 0.37f, 0.16f, 1);
    st.ScaleAllSizes(s);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(g_dev);
    g_imgui = 1;
    logmsg("menu: ImGui %s ready (scale %.2f)", IMGUI_VERSION, s);
}

static HRESULT __stdcall hk_present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *d)
{
    int hidden = C.hideWithF11 && g_hiddenF11, drawers = 0;
    for (int k = 0; k < S.nModules; k++) if (!S.modules[k]->disabled && S.modules[k]->overlay) drawers = 1;
    int show = S.menuOpen || (!hidden && (C.overlay || drawers));
    if (dev == g_dev && show) {
        if (!g_imgui) imgui_init();
        if (g_imgui && SUCCEEDED(dev->BeginScene())) {
            ImGui::GetIO().MouseDrawCursor = S.menuOpen != 0;
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (!hidden) draw_modules();
            if (S.menuOpen) { capture_key(); draw_menu(); }
            else if (C.overlay) draw_overlay();
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            dev->EndScene();
        }
    }
    return g_origPresent(dev, a, b, w, d);
}

static HRESULT __stdcall hk_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    HRESULT r;
    if (g_imgui) ImGui_ImplDX9_InvalidateDeviceObjects();
    r = g_origReset(dev, pp);
    if (g_imgui && SUCCEEDED(r)) ImGui_ImplDX9_CreateDeviceObjects();
    return r;
}

static LRESULT CALLBACK hk_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (S.menuOpen && g_imgui) {
        if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
        switch (msg) {   /* the menu owns the mouse and keyboard while open */
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_INPUT:
            return msg == WM_INPUT ? DefWindowProcA(h, msg, wp, lp) : 0;
        }
    } else if (g_imgui) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp); /* keep ImGui's key state in sync */
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static void try_hook(void)
{
    void *adv;
    if (g_hooked || g_failed) return;
    adv = advInstance();
    if (!adv) return;                     /* renderer not created yet */
    g_dev = getDevice(adv);
    if (!g_dev) return;
    g_vtbl = *(void ***)g_dev;
    if (!in_image(g_vtbl) || !in_image(g_vtbl[17]) || !in_image(g_vtbl[16])) {
        logmsg("menu: device vtable does not look like D3D9, menu disabled");
        g_failed = 1;
        return;
    }
    g_hwnd = getHwnd ? getHwnd(adv) : NULL;
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(g_dev->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
    }
    if (!g_hwnd || !IsWindow(g_hwnd)) { logmsg("menu: no game window"); g_failed = 1; return; }
    patch_slot(&g_vtbl[17], (void *)hk_present, (void **)&g_origPresent);
    patch_slot(&g_vtbl[16], (void *)hk_reset, (void **)&g_origReset);
    g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndproc);
    g_hooked = 1;
    logmsg("menu: hooked device %p window %p", (void *)g_dev, (void *)g_hwnd);
}

/* ------------------------------------------------------------------ module */
static int resolve(void)
{
    int ok = 1;
    if (!E.render) { logmsg("menu: s2render.dll not loaded"); return 0; }
    ok &= mk_resolve(E.render, &advInstance, "?InstancePtr@?$tSingleton@VcDX9Advance@@@Orc@@SAPAVcDX9Advance@@XZ", 1);
    ok &= mk_resolve(E.render, &getDevice, "?getDevice@cDX9Base@@QBEPAUIDirect3DDevice9@@XZ", 1);
    mk_resolve(E.render, &getHwnd, "?getHwnd@cDX9Base@@QBEPAUHWND__@@XZ", 0);
    return ok;
}

static void frame(float dt)
{
    (void)dt;
    if (!advInstance) return;  /* not resolved yet */
    try_hook();
    g_menuKey.vk = C.menuKey;
    g_f11.vk = VK_F11;
    if (game_has_focus()) {
        if (key_pressed(&g_menuKey) && !g_capture) {
            S.menuOpen = !S.menuOpen;
            if (S.menuOpen && !g_hooked) logmsg("menu: not available (%s)", g_failed ? "hook failed" : "renderer not ready");
            if (!g_hooked) S.menuOpen = 0;
        }
        if (key_pressed(&g_f11)) g_hiddenF11 = !g_hiddenF11;
    }
}

static const char *status(void)
{
    snprintf(g_status, sizeof g_status, "Menu: %s", g_hooked ? key_name(C.menuKey) : (g_failed ? "no disponible" : "esperando"));
    return g_status;
}

static void stop(void)
{
    if (!g_hooked) return;
    if (g_hwnd && g_origWndProc) SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    patch_slot(&g_vtbl[17], (void *)g_origPresent, NULL);
    patch_slot(&g_vtbl[16], (void *)g_origReset, NULL);
    if (g_imgui) { ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    g_hooked = g_imgui = 0;
}

extern "C" MK_MODULE(mod_menu) = {
    "Menu e indicador", "Menu", settings, sizeof settings / sizeof settings[0], 90,
    0, resolve, frame, 0, 0, 0, status, stop, 0,
};
