#include "ui_manager.h"
#include "gui.h"
#include "gfx.h"
#include "compositor.h"

#define UI_MGR_MAX UI_MAX_ELEMENTS

static ui_node_t g_nodes[UI_MGR_MAX];
static int       g_node_count;
static int       g_hover_element_index = -1;
static int       g_last_focus_element  = -1;

int ui_manager_focused_node_index = -1;

static void ui_damage_element_bounds(int ei) {
    UI_Element* e;
    if (ei < 0 || ei >= ui_element_count)
        return;
    e = &ui_elements[ei];
    compositor_damage_rect_pad(e->x, e->y, e->w, e->h, 12);
}

void ui_manager_clear(void) {
    g_node_count                      = 0;
    ui_manager_focused_node_index     = -1;
    g_hover_element_index             = -1;
    g_last_focus_element              = -1;
}

void ui_manager_sync_from_elements(void) {
    int i;

    g_node_count                  = 0;
    ui_manager_focused_node_index = -1;

    for (i = 0; i < ui_element_count; i++) {
        UI_Element* e = &ui_elements[i];
        ui_node_t*  n = &g_nodes[g_node_count++];

        n->x             = e->x;
        n->y             = e->y;
        n->width         = e->w;
        n->height        = e->h;
        n->is_focusable  = e->is_focusable;
        n->is_hovered    = 0;
        n->element_index = i;
        n->on_click      = e->callback;
        n->on_key        = e->on_keypress;
        n->has_focus     = (i == focused_element_index) ? 1 : 0;
        if (n->has_focus)
            ui_manager_focused_node_index = g_node_count - 1;
    }
}

void ui_manager_sync_focus_flags(void) {
    int i;

    if (focused_element_index != g_last_focus_element) {
        if (g_last_focus_element >= 0)
            ui_damage_element_bounds(g_last_focus_element);
        if (focused_element_index >= 0)
            ui_damage_element_bounds(focused_element_index);
        g_last_focus_element = focused_element_index;
    }

    ui_manager_focused_node_index = -1;
    for (i = 0; i < g_node_count; i++) {
        g_nodes[i].has_focus = (g_nodes[i].element_index == focused_element_index) ? 1 : 0;
        if (g_nodes[i].has_focus)
            ui_manager_focused_node_index = i;
    }
}

static int ui_mgr_hit_topmost(int mx, int my) {
    int i;

    for (i = g_node_count - 1; i >= 0; i--) {
        const ui_node_t* n = &g_nodes[i];
        if (mx >= n->x && mx < n->x + n->width && my >= n->y && my < n->y + n->height)
            return i;
    }
    return -1;
}

void ui_manager_update_hover(int mouse_x, int mouse_y) {
    int i;
    int hit     = ui_mgr_hit_topmost(mouse_x, mouse_y);
    int new_el  = (hit >= 0) ? g_nodes[hit].element_index : -1;

    if (new_el != g_hover_element_index) {
        if (g_hover_element_index >= 0)
            ui_damage_element_bounds(g_hover_element_index);
        if (new_el >= 0)
            ui_damage_element_bounds(new_el);
        g_hover_element_index = new_el;
    }

    for (i = 0; i < g_node_count; i++)
        g_nodes[i].is_hovered = (i == hit) ? 1 : 0;
}

int ui_manager_hover_element_index(void) {
    return g_hover_element_index;
}

int ui_manager_element_is_hovered(int element_index) {
    return element_index >= 0 && element_index == g_hover_element_index;
}

int ui_manager_handle_primary_click(int mouse_x, int mouse_y) {
    int hi;

    if (g_node_count <= 0)
        return 0;

    ui_manager_update_hover(mouse_x, mouse_y);
    hi = ui_mgr_hit_topmost(mouse_x, mouse_y);
    if (hi < 0)
        return 0;

    if (g_nodes[hi].is_focusable)
        focused_element_index = g_nodes[hi].element_index;

    ui_sync_focus_ring_from_mouse();
    ui_activate_focused();
    return 1;
}

void ui_manager_draw_focus_rings(void) {
    int         i;
    unsigned int blue = RGB(0, 122, 255);
    int         t;

    for (i = 0; i < g_node_count; i++) {
        const ui_node_t* n = &g_nodes[i];
        int              x, y, w, h, pad;

        if (!n->has_focus || !n->is_focusable)
            continue;

        x   = n->x;
        y   = n->y;
        w   = n->width;
        h   = n->height;
        pad = 3;

        /* Anillo de 3 px: franjas semitransparentes (estilo sistema). */
        for (t = 0; t < pad; t++) {
            unsigned a = (unsigned)(55u + (unsigned)(70u * (unsigned)t / (unsigned)(pad > 1 ? pad - 1 : 1)));
            int o = pad - t;

            gfx_blend_rect(x - o, y - o, w + 2 * o, 1, blue, a);
            gfx_blend_rect(x - o, y + h + o - 1, w + 2 * o, 1, blue, a);
            gfx_blend_rect(x - o, y - o + 1, 1, h + 2 * o - 2, blue, a);
            gfx_blend_rect(x + w + o - 1, y - o + 1, 1, h + 2 * o - 2, blue, a);
        }
    }
}
