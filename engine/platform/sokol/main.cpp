#include "Engine.h"
#include "DoriaxSokol.h"
#include "Input.h"

#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"

static float mousePosX = 0.0f;
static float mousePosY = 0.0f;

void sokol_init(void) {
    doriax::Engine::systemViewLoaded();
    doriax::Engine::systemViewChanged();
}

void sokol_frame(void) {
    doriax::Engine::systemDraw();
}

int convMouseButtom(sapp_mousebutton mouse_button){
    if (mouse_button == SAPP_MOUSEBUTTON_LEFT){
        return D_MOUSE_BUTTON_LEFT;
    }else if (mouse_button == SAPP_MOUSEBUTTON_RIGHT){
        return D_MOUSE_BUTTON_RIGHT;
    }else if (mouse_button == SAPP_MOUSEBUTTON_MIDDLE){
        return D_MOUSE_BUTTON_MIDDLE;
    }
    return D_MOUSE_BUTTON_LEFT;
}

static void sokol_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_RESIZED){
        doriax::Engine::systemViewChanged();
    }else if (e->type == SAPP_EVENTTYPE_CHAR){
        if (e->char_code != 127) // fix macos backspace
            doriax::Engine::systemCharInput(e->char_code);
    }else if (e->type == SAPP_EVENTTYPE_KEY_DOWN){
        if (e->key_code == SAPP_KEYCODE_TAB)
            doriax::Engine::systemCharInput('\t');
        if (e->key_code == SAPP_KEYCODE_BACKSPACE)
            doriax::Engine::systemCharInput('\b');
        if (e->key_code == SAPP_KEYCODE_ENTER)
            doriax::Engine::systemCharInput('\r');
        if (e->key_code == SAPP_KEYCODE_ESCAPE)
            doriax::Engine::systemCharInput('\33'); // removed 'e' to avoid warnings
        // use same keycode of GLFW
        doriax::Engine::systemKeyDown(e->key_code, e->key_repeat, e->modifiers);
    }else if (e->type == SAPP_EVENTTYPE_KEY_UP){
        // use same keycode of GLFW
        doriax::Engine::systemKeyUp(e->key_code, e->key_repeat, e->modifiers);
    }else if (e->type == SAPP_EVENTTYPE_SUSPENDED){
        doriax::Engine::systemPause();
    }else if (e->type == SAPP_EVENTTYPE_RESUMED){
        doriax::Engine::systemResume();  
    }else if (e->type == SAPP_EVENTTYPE_MOUSE_UP){
        const bool mouseLocked = sapp_mouse_locked();
        if (!mouseLocked){
            mousePosX = e->mouse_x;
            mousePosY = e->mouse_y;
        }
        doriax::Engine::systemMouseUp(convMouseButtom(e->mouse_button), mousePosX, mousePosY, e->modifiers);
    }else if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN){
        const bool mouseLocked = sapp_mouse_locked();
        if (!mouseLocked){
            mousePosX = e->mouse_x;
            mousePosY = e->mouse_y;
        }
        doriax::Engine::systemMouseDown(convMouseButtom(e->mouse_button), mousePosX, mousePosY, e->modifiers);
    }else if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE){
        if (sapp_mouse_locked()){
            mousePosX += e->mouse_dx;
            mousePosY += e->mouse_dy;
        }else{
            mousePosX = e->mouse_x;
            mousePosY = e->mouse_y;
        }
        doriax::Engine::systemMouseMove(mousePosX, mousePosY, e->modifiers);
    }else if (e->type == SAPP_EVENTTYPE_MOUSE_SCROLL){
        doriax::Engine::systemMouseScroll(e->scroll_x, e->scroll_y, e->modifiers);
    }else if (e->type == SAPP_EVENTTYPE_MOUSE_ENTER){
        doriax::Engine::systemMouseEnter();
    }else if (e->type == SAPP_EVENTTYPE_MOUSE_LEAVE){
        doriax::Engine::systemMouseLeave();
    }
}

void sokol_cleanup(void) {
    doriax::Engine::systemViewDestroyed();
    doriax::Engine::systemShutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    sapp_desc app_desc = {0};

    app_desc.init_cb = sokol_init;
    app_desc.frame_cb = sokol_frame;
    app_desc.event_cb = sokol_event;
    app_desc.cleanup_cb = sokol_cleanup;
    app_desc.width = DEFAULT_WINDOW_WIDTH;
    app_desc.height = DEFAULT_WINDOW_HEIGHT;
    app_desc.sample_count = 4;
    app_desc.window_title = "Doriax";

    doriax::Engine::systemInit(argc, argv, new DoriaxSokol());

    return app_desc;
}
