//
// (c) 2026 Eduardo Doria.
//


#ifndef Input_h
#define Input_h

//Based on GLFW modifier bits
#define D_MODIFIER_SHIFT          0x0001
#define D_MODIFIER_CONTROL        0x0002
#define D_MODIFIER_ALT            0x0004
#define D_MODIFIER_SUPER          0x0008
#define D_MODIFIER_CAPS_LOCK      0x0010
#define D_MODIFIER_NUM_LOCK       0x0020

//Based on GLFW key codes
#define D_KEY_UNKNOWN             -1
#define D_KEY_SPACE               32
#define D_KEY_APOSTROPHE          39  /* ' */
#define D_KEY_COMMA               44  /* , */
#define D_KEY_MINUS               45  /* - */
#define D_KEY_PERIOD              46  /* . */
#define D_KEY_SLASH               47  /* / */
#define D_KEY_0                   48
#define D_KEY_1                   49
#define D_KEY_2                   50
#define D_KEY_3                   51
#define D_KEY_4                   52
#define D_KEY_5                   53
#define D_KEY_6                   54
#define D_KEY_7                   55
#define D_KEY_8                   56
#define D_KEY_9                   57
#define D_KEY_SEMICOLON           59 /* ; */
#define D_KEY_EQUAL               61  /* = */
#define D_KEY_A                   65
#define D_KEY_B                   66
#define D_KEY_C                   67
#define D_KEY_D                   68
#define D_KEY_E                   69
#define D_KEY_F                   70
#define D_KEY_G                   71
#define D_KEY_H                   72
#define D_KEY_I                   73
#define D_KEY_J                   74
#define D_KEY_K                   75
#define D_KEY_L                   76
#define D_KEY_M                   77
#define D_KEY_N                   78
#define D_KEY_O                   79
#define D_KEY_P                   80
#define D_KEY_Q                   81
#define D_KEY_R                   82
#define D_KEY_S                   83
#define D_KEY_T                   84
#define D_KEY_U                   85
#define D_KEY_V                   86
#define D_KEY_W                   87
#define D_KEY_X                   88
#define D_KEY_Y                   89
#define D_KEY_Z                   90
#define D_KEY_LEFT_BRACKET        91 /* [ */
#define D_KEY_BACKSLASH           92  /* \ */
#define D_KEY_RIGHT_BRACKET       93  /* ] */
#define D_KEY_GRAVE_ACCENT        96 /* ` */
#define D_KEY_WORLD_1             161 /* non-US #1 */
#define D_KEY_WORLD_2             162 /* non-US #2 */
#define D_KEY_ESCAPE              256
#define D_KEY_ENTER               257
#define D_KEY_TAB                 258
#define D_KEY_BACKSPACE           259
#define D_KEY_INSERT              260
#define D_KEY_DELETE              261
#define D_KEY_RIGHT               262
#define D_KEY_LEFT                263
#define D_KEY_DOWN                264
#define D_KEY_UP                  265
#define D_KEY_PAGE_UP             266
#define D_KEY_PAGE_DOWN           267
#define D_KEY_HOME                268
#define D_KEY_END                 269
#define D_KEY_CAPS_LOCK           280
#define D_KEY_SCROLL_LOCK         281
#define D_KEY_NUM_LOCK            282
#define D_KEY_PRINT_SCREEN        283
#define D_KEY_PAUSE               284
#define D_KEY_F1                  290
#define D_KEY_F2                  291
#define D_KEY_F3                  292
#define D_KEY_F4                  293
#define D_KEY_F5                  294
#define D_KEY_F6                  295
#define D_KEY_F7                  296
#define D_KEY_F8                  297
#define D_KEY_F9                  298
#define D_KEY_F10                 299
#define D_KEY_F11                 300
#define D_KEY_F12                 301
#define D_KEY_F13                 302
#define D_KEY_F14                 303
#define D_KEY_F15                 304
#define D_KEY_F16                 305
#define D_KEY_F17                 306
#define D_KEY_F18                 307
#define D_KEY_F19                 308
#define D_KEY_F20                 309
#define D_KEY_F21                 310
#define D_KEY_F22                 311
#define D_KEY_F23                 312
#define D_KEY_F24                 313
#define D_KEY_F25                 314
#define D_KEY_KP_0                320
#define D_KEY_KP_1                321
#define D_KEY_KP_2                322
#define D_KEY_KP_3                323
#define D_KEY_KP_4                324
#define D_KEY_KP_5                325
#define D_KEY_KP_6                326
#define D_KEY_KP_7                327
#define D_KEY_KP_8                328
#define D_KEY_KP_9                329
#define D_KEY_KP_DECIMAL          330
#define D_KEY_KP_DIVIDE           331
#define D_KEY_KP_MULTIPLY         332
#define D_KEY_KP_SUBTRACT         333
#define D_KEY_KP_ADD              334
#define D_KEY_KP_ENTER            335
#define D_KEY_KP_EQUAL            336
#define D_KEY_LEFT_SHIFT          340
#define D_KEY_LEFT_CONTROL        341
#define D_KEY_LEFT_ALT            342
#define D_KEY_LEFT_SUPER          343
#define D_KEY_RIGHT_SHIFT         344
#define D_KEY_RIGHT_CONTROL       345
#define D_KEY_RIGHT_ALT           346
#define D_KEY_RIGHT_SUPER         347
#define D_KEY_MENU                348
#define D_KEY_LAST                D_KEY_MENU

//Mouse
#define D_MOUSE_BUTTON_1    0
#define D_MOUSE_BUTTON_2    1
#define D_MOUSE_BUTTON_3    2
#define D_MOUSE_BUTTON_4    3
#define D_MOUSE_BUTTON_5    4
#define D_MOUSE_BUTTON_6    5
#define D_MOUSE_BUTTON_7    6
#define D_MOUSE_BUTTON_8    7

#define D_MOUSE_BUTTON_LAST     D_MOUSE_BUTTON_8
#define D_MOUSE_BUTTON_LEFT     D_MOUSE_BUTTON_1
#define D_MOUSE_BUTTON_RIGHT    D_MOUSE_BUTTON_2
#define D_MOUSE_BUTTON_MIDDLE   D_MOUSE_BUTTON_3

#include <map>
#include <vector>
#include "math/Vector2.h"

namespace doriax {

    typedef struct Touch{
        int pointer;
        Vector2 position;
    } Touch;
    
    class DORIAX_API Input {
        
        friend class Engine;
        
    private:
        
        static std::map<int,bool> keyPressed;
        static std::map<int,bool> mousePressed;
        static Vector2 mousePosition;
        static Vector2 mouseScroll;
        static std::vector<Touch> touches;
        static bool mousedEntered;
        static int modifiers;
        
        static void addKeyPressed(int key);
        static void releaseKeyPressed(int key);
        
        static void addMousePressed(int button);
        static void releaseMousePressed(int button);
        static void setMousePosition(float x, float y);
        static void setMouseScroll(float xoffset, float yoffset);
        
        static void addTouch(int pointer, float x, float y);
        static void setTouchPosition(int pointer, float x, float y);
        static void removeTouch(int pointer);
        static void clearTouches();

        static void addMouseEntered();
        static void releaseMouseEntered();
        
        static void setModifiers(int mods);
        
    public:
        
        static bool isKeyPressed(int key);
        static bool isMousePressed(int button);
        static bool isTouch();
        static bool isMouseEntered();
        
        static Vector2 getMousePosition();
        static Vector2 getMouseScroll();
        static Vector2 getTouchPosition(int pointer);

        static std::vector<Touch> getTouches();
        static size_t numTouches();
        
        static int getModifiers();
        
        static size_t findTouchIndex(int pointer);
    };
}

#endif /* Input_h */
